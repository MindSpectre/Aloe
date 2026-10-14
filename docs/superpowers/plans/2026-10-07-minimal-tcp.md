# Minimal TCP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the minimal TCP brick, its stream contract and the optional runtime adapter, with hand-written and task-based echoes interoperating with Linux.

**Architecture:** `tcp::Stack<Ip>` runs on its connection's owning thread over `net::Ipv4<Device>` and `loop::TimerWheel`. Applications drain explicit connection-event snapshots, read retained packet payloads in place and encode directly into prepared transmit packets. The brick is built and verified first; the runtime then adds tick hooks, wait slots, senders and cross-shard ARP forwarding over it.

**Tech Stack:** C++26, libstdc++, clang 22 and GCC 16, CMake and Ninja, GoogleTest, the existing vcpkg dependencies, the fabric and frames fixtures, DPDK only through `ethdev`. No new dependency.

**Spec:** [Minimal TCP design](../specs/2026-10-07-minimal-tcp-design.md), including the latency-review revision. Read the spec, `AGENTS.md` and `codestyle.md` before implementation. This plan resolves the spec's schematic declarations into concrete C++ and carries the code of every file; it does not add full TCP or a two-core connection.

## Global Constraints

- "Every connection runs on one thread in this phase, including receive, transmit and maintenance."
- "Multiple shards may run concurrently, each with its own connections."
- "The optional runtime serves applications that want scheduling and coroutines over the same bricks."
- "`send(bytes)` is a convenience over `prepare` and `commit`, not a promise of POSIX socket buffering or a file-descriptor API."
- "The in-place path remains primary." Receive payloads stay in packets; only the explicit `send(bytes)` helper copies application bytes.
- "No unsent byte queue is added to imitate `send(2)`."
- "A commit is one segment: two commits are two segments on the wire whatever their size"; no Nagle, no implicit coalescing.
- "Processing a later burst, an empty burst, a timer firing or a transmit flush never consumes an event."
- "Acceptance means ownership passed to the local transmit ring; it does not mean the frame has reached the wire."
- "Two minimal Aloe peers cannot recover lost data until phase 2"; a test that needs recovery uses the scripted peer or Linux, never an invented retransmitter.
- "Actual two-core execution, delegated transmission allowances, maintenance-core scheduling, asymmetric fences and hardware-specific fast paths require later design and measurement."
- TCP links `net`, `stream`, `loop`, `wire`, `device` and `core`; never `execution`, `log` or DPDK. Fixtures depend on `common`, never the reverse.
- Every hot operation is `noexcept`, allocates nothing but packets from the existing pool, and takes its time from a supplied `core::TimePoint` or `core::Duration`. Setup validates and throws `std::invalid_argument`.
- Only `common/execution/execution/execution.hpp` names `stdexec::` or `exec::`; only `common/log/log/log.hpp` names `quill::`. Public headers define no macros. `CMAKE_CXX_SCAN_FOR_MODULES` stays `OFF`. One namespace per module; header basenames unique across modules.
- Default TCP configuration: 1024 connections, 8 listeners, 32 held packets per connection, 2048 shared receive nodes, ephemeral ports 32768 to 60999 inclusive, initial control retry 1 second, 5 retransmissions, unresolved-neighbour retry 10 milliseconds. No window scaling; byte credit is at most 65535. A peer without an MSS option gets 536.
- Automated tests need no root, hugepages or NIC. The Linux tap suite has the `manual` label, excluded by every test preset. Record actual execution of manual gates separately from compilation.
- Completion gates are the `debug`, `gcc-debug`, `asan` and `tsan` builds and tests plus `./scripts/check-format.sh`; clang builds run clang-tidy with warnings as errors.

## Verified Beforehand

Nothing in this plan was built end to end; the author asked for the plan without a prototype. These facts were read from the code on 2026-10-08, and the two probes were compiled in a scratchpad and discarded.

| Fact | Where |
|---|---|
| `net::Ipv4`'s template parameter is named `Device`, so the spec's `using Device = Device;` is ill-formed. The parameter becomes `DeviceT` and the alias `Device`. | `common/net/stack/ipv4.hpp:106` |
| The IP brick drops a frame whose source is our own address as martian, so the spec's "connect to our own address over `net_ring`" cannot work. The ring test uses a scripted peer at another address, as `test_net_ring.cpp` already does for UDP. | `common/net/stack/ipv4.hpp:438`, `tests/integration_tests/net/test_net_ring.cpp:117` |
| `Ipv4::send` takes `Packet&&`, returns `std::expected<void, SendError>` with `NoRoute`, `Unresolved`, `Oversized`, `Refused`, asserts `headroom() >= headers_size` (34) and hands the packet back unchanged on failure. `allocate()` returns `std::optional<Packet>`. `received(protocol)` is a `std::span<Datagram<Packet>>` valid until the next `process`; `resolved()` a `std::span<const ArpResolution>`; `learn(address, mac, now)` appends nothing to it. | `common/net/stack/ipv4.hpp` |
| `net::Datagram<Packet>` has `packet`, `source`, `destination`, `protocol`, `l3_offset`, `l3_length`, `l4_length`, `l4_checksum` and `l4()`; `net::ArpResolution` has `address` and `mac`. | `common/net/stack/datagram.hpp` |
| `device::packet_headroom` is 128 on every backend; `IsPacket` offers `data()` only on a non-const packet, plus `prepend`, `append`, `trim_front`, `trim_back`, `rx()`, `tx()`, `set_tx`. `RxMetadata::rss_hash` is optional. | `common/device/packet/packet.hpp` |
| `device::flow_hash(rss, tuple)` hashes the 4-tuple only when the protocol's type is enabled, else the 2-tuple when `ipv4` is, else 0; `queue_for` returns 0 when steering is off or the table empty. `round_robin_rss` enables all three types. | `common/device/steering/rss.hpp` |
| A single-queue fabric port has `steering().enabled == false` and no `rss_hash`; a multi-queue port steers by `round_robin_rss` and stamps `rss_hash = flow_hash(...)`. | `fixtures/fabric/fabric/fabric.cpp:39-56,93-97` |
| `loop::ShardQueue<Device>` has `allocate`, `receive`, `transmit(Packet&&)` (moves only on success), `flush`, `discard`, `pending`, `capacity`, `index`, `mac`, `mtu`, `capabilities`, `steering`, `queue_count`, `device`. | `common/loop/queue/shard_queue.hpp` |
| `loop::Timer` is immovable with `fire` of type `void (*)(Timer&) noexcept`, called with the timer already unarmed; `TimerWheel::arm`, `cancel`, `advance(now)`, `pending()`. | `common/loop/timer/timer_wheel.hpp` |
| `loop::Work` is immovable with an atomic `next` and `run`; `Inbox::push` is wait-free from any thread; `RunQueue::push`, `take`, `run_chain`. | `common/loop/work/*.hpp` |
| `runtime::ShardContext` has `run_once(now)`, `request_stop`, `stop_requested`, `drained()` (stop requested, scope empty, both queues empty), `now()`, `index()`, `ready()`, `inbox()`, `timers()`, `scope()`, `counters()`, `current()` and the `Current` guard; no siblings, no `set_now`. `Shard::step` is receive, `on_receive`, free leftovers, `run_once`, flush; `Shard::run` loops until `drained()`. `IsStack` requires `on_receive(burst)` only. `Runtime::stop` pushes one owned `StopWork` per shard through the inbox; a failed thread hook returns from the thread body before `run()`. | `common/runtime/context/shard_context.hpp`, `.cpp`, `shard/shard.hpp`, `runtime/runtime.hpp` |
| `runtime::detail::ShardSenderAttributes{context}` answers the completion-scheduler and completion-behaviour queries; `detail::TimerOperation` is the operation state pattern: a `loop::Work` and a `loop::Timer`, a `stop_callback_for_t` on the receiver's token, `set_stopped` inline from its `cancel`. `runtime::task<T>` is `execution::task<T, ShardEnvironment>`. | `common/runtime/scheduler/scheduler.hpp`, `task/shard_task.hpp` |
| The runtime's unit targets are `Aloe.Tests.Unit.Runtime` and `Aloe.Tests.Unit.Runtime.Threads`, both with sources directly in `tests/unit_tests/common/runtime/`; there is no `threads/` subdirectory. The net target is `Aloe.Tests.Unit.Net`, the wire target `Aloe.Tests.Unit.Wire`, the integration ring target `Aloe.Tests.Integration.Net.Ring`, the manual tap target `Aloe.Tests.Manual.Net.Tap`. | `tests/unit_tests/common/*/CMakeLists.txt`, `tests/integration_tests/net/CMakeLists.txt`, `tests/manual_tests/net/CMakeLists.txt` |
| `frames` has `ethernet_frame`, `Ipv4Spec`, `ipv4_frame` (a 20-byte TCP header with ACK only, for device tests), `with_ipv4_options`, `flow_of`, `fill`, `bytes_of`, `pattern`, `Checksums::{Correct, Zero, Seeded, Wrong}`; `net_frames.hpp` has `ParsedFrame{ethernet, arp, ipv4, icmp, ipv4_header, l4}`, `parse_frame` and `l4_checksum_residue`. | `fixtures/frames/frames/frames.hpp`, `net_frames.hpp` |
| `testing::NetFixture` is a `TestWithParam<fabric::EmulatedOffloads>` over a fabric with `harness_` and `port_`, `queue_`, `ip_`, `now_`, `inject`, `process_pending`, `local`, `harness_received`, `burst_empty`; constants `stack_mac`, `harness_mac`, `gateway_mac`, `stack_ip`, `harness_ip`, `gateway_ip`, `far_ip`, `offloads_name`. | `tests/shared/net/net_fixture.hpp` |
| `add_combined_library(Target DIRECTORIES ... SOURCES ... LIBRARIES ...)` makes the umbrella interface target; `add_unit_test`, `add_integration_test`, `add_manual_test` register binaries; `${TEST_LIBS}`, `${UNIT_TESTING_TARGET}`, `${INTEGRATION_TESTING_TARGET}`, `${MANUAL_TESTING_TARGET}`, `${SHARED_TESTING_TARGET}` are set in `tests/CMakeLists.txt`. `common/CMakeLists.txt` adds `runtime` before `net`. | `cmake/combined_library.cmake`, `cmake/tests.cmake`, `common/CMakeLists.txt` |
| `fabric::Port` keeps its constructor public behind a private `PrivateTag` that only `Fabric` can name: the passkey idiom this plan reuses for connections. `.clang-tidy` enables `modernize-make-unique`, so `new T[n]` is flagged. | `fixtures/fabric/fabric/fabric.hpp:47-56`, `.clang-tidy:82` |
| `ethdev::register_thread()` exists and is the runtime's `thread_hook` for DPDK; `testing::EalEnvironment{"net_ring0"}` and `testing::probe_vdev("net_ring")` give fresh ring ports; `ethdev::PortConfig{name, queues, mtu, descriptors, pool_size}`. | `common/ethdev/eal/eal.hpp`, `tests/shared/dpdk/eal_environment.hpp`, `common/ethdev/port/port.hpp` |
| The ARP cache carries `// TODO: Issue#5 - research replacing this table with abseil's flat_hash_set ...`. | `common/net/arp/arp_cache.hpp:39-40` |
| `codestyle.md` uses `aloe::net::tcp` twice: the namespace row of the naming table and the nested-namespace example. | `codestyle.md:49,583` |
| **Probe 1.** A view whose iterator has `iterator_concept = std::forward_iterator_tag`, `iterator_category = std::input_iterator_tag`, `value_type = std::span<const std::byte>` and a by-value `operator*` satisfies `std::forward_iterator`, `std::ranges::forward_range` and the spec's `IsReadView`, on clang 22 and GCC 16 with `-Wall -Wextra -Wconversion -Werror`. A `std::vector<std::byte>` does not satisfy `IsReadView`. | scratchpad probe, discarded |
| **Probe 2.** A sender with `completion_signatures<set_value_t(std::expected<std::size_t, Error>), set_stopped_t()>`, whose operation state is a plain node with a `stop_callback_for_t` on the receiver's token, is awaited from the pinned stdexec `task<T>` as `const std::expected<std::size_t, Error> r = co_await sender;`. Compiled on both compilers with stdexec as a system include; with `-I` instead of `-isystem`, `-Wmissing-field-initializers` fires inside stdexec's `__finally.hpp`. The build already uses system includes for vcpkg. | scratchpad probe, discarded |
| **Vectors.** FNV-1a over the eight network-order key bytes: `10.0.0.1:40000 -> 7` is `0x12854375`, `10.0.0.1:40001 -> 7` is `0x4f6bf39a`, `10.0.0.2:40000 -> 7` is `0x43a64bdc`, eight zero bytes is `0x9be17165`. The spec's finalizer: `0 -> 0`, `1 -> 0x514e28b7`, `0xdeadbeef -> 0x0de5c6a9`, `0x80000000 -> 0x6d3c65a0`. Sixty-four hashes sharing their low four bits map to sixty-four distinct home buckets in a 2048-entry table after mixing, and to one bucket before. A SYN `10.0.0.1:40000 -> 10.0.0.2:7`, sequence 1000, window 65535, MSS 1460 is `9c40 0007 000003e8 00000000 60 02 ffff 0000 0000 0204 05b4` with TCP checksum `0xe3f4`; a PSH+ACK with sequence 1001, acknowledgement 5001, window 65535 and payload `abc` has checksum `0x23ab`. | computed with Python, 2026-10-08 |

## Read This First

For an executor who has the repository, the spec and this plan, and nothing else.

**Baseline.** From a clean checkout the first configure clones vcpkg and builds DPDK, about five minutes:

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Configure, build and test presets share names: `debug`, `gcc-debug`, `asan`, `tsan`, `release`. The lint check is a clean clang build: clang-tidy runs inside it with warnings as errors. `./scripts/check-format.sh` checks formatting; `.githooks/pre-commit` runs it on staged files.

**A change is done** when `debug`, `gcc-debug`, `asan` and `tsan` all build and pass, and the format check passes. Build `debug` after every step that says so; build the other presets at the end of each task from Task 9 on, and all of them in Task 14.

**Focused commands.** This repository registers one CTest test per binary, not per GoogleTest case:

```bash
cmake --build --preset debug --target Aloe.Tests.Unit.Tcp
ctest --preset debug -R '^Aloe[.]Tests[.]Unit[.]Tcp$' --output-on-failure
./build/debug/tests/unit_tests/common/tcp/Aloe.Tests.Unit.Tcp --gtest_filter='TcpHandshake.*'   # diagnosing only
```

Run `cmake --preset debug` again after every CMake addition.

**Traps already found** (from `AGENTS.md` and the earlier plans, plus what this module meets):

- `EXPECT_THROW(std::ignore = T{.a = 1, .b = 2}, E)` needs the expression in an extra pair of parentheses, or the macro splits it at the commas.
- One process starts DPDK's EAL once. A test binary that uses DPDK registers one `aloe::testing::EalEnvironment` and probes fresh virtual devices with `aloe::testing::probe_vdev`. A closed port cannot be reopened.
- The ring driver has no RSS, sets no MTU and reports no checksum offload, so over it the brick always takes the software checksum path; what a queue transmits comes back on the same queue.
- `-Wconversion` is an error: every narrowing from `std::size_t` to `std::uint16_t` or `std::uint8_t` is a `static_cast`, and arithmetic on `std::uint8_t` or `std::uint16_t` promotes to `int`, so cast the result back.
- clang-tidy's `hicpp-multiway-paths-covered` wants a `default:` on a `switch` over an `enum class` whose values are not all listed.
- clang-tidy's `readability-make-member-function-const` fires on a member that only writes through a pointer member. Call `core::force_non_const(this)` first when the type owns what the pointer reaches; make the member `const` when the type is a non-owning handle.
- clang-tidy's `cppcoreguidelines-pro-type-member-init` wants every member initialised: give every member a default member initialiser.
- clang-tidy's `performance-unnecessary-value-param` fires on a by-value parameter of a non-trivial type that is only read. `wire::MacAddress`, `wire::Ipv4Address`, `wire::TcpSequence`, `wire::TcpFlags`, `tcp::Endpoint` and `tcp::FlowKey` are trivially copyable and pass by value; `const TcpHeader&`, `const TcpSpec&` and `const ParsedFrame&` pass by reference.
- clang-tidy's `cppcoreguidelines-pro-type-static-cast-downcast` is in non-strict mode: `static_cast` from `loop::Work&` or `loop::Timer&` to the derived node is allowed, as `TimerOperation` does.
- clang-tidy's `bugprone-use-after-move` flags a packet used after a refused `ShardQueue::transmit` or `Ipv4::send`, which moved nothing. Mark those lines `NOLINT(bugprone-use-after-move)` with the reason, as `net::Ipv4::send` does.
- clang-tidy's `cert-msc51-cpp` fires on a fixed random seed; the ISN engine is seeded from `std::random_device` on the cold path and a reproducible test keeps its seed with a one-line `NOLINT`.
- clang-tidy's `modernize-make-unique` flags `new T[n]`; and `std::make_unique<T[]>` and `std::deque::emplace_back` construct through code that is not a friend, so a private constructor does not work. A connection has a public constructor taking a private `PrivateTag` that only `Stack` can name, and lives in a `std::deque`, which never moves an element.
- A local class cannot declare member templates, so a test sender with a templated `connect` goes in the anonymous namespace.
- GCC's `-Wshadow` warns when a constructor parameter shadows a member function (`device` against `device()`). Rename the parameter, keep the member.
- Inside a class template, a call to a member template of a dependent object needs `.template`.
- `std::expected<void, E>`: return `{}` on success and `std::unexpected{E::Value}` on failure; `<expected>` is the header.
- `std::chrono::steady_clock::time_point{}` is the epoch. Tests start there and add offsets; a "never" is an `std::optional<TimePoint>`.
- A member function of a class template that is declared but never defined only fails to link once something instantiates it. Every function `process` reaches must therefore be defined in the task that first instantiates `process`; that is why Task 6 carries the whole receive dispatcher.
- A shard task awaits only its own shard's senders; the scope asserts completions arrive on the thread that spawned. Every stream sender completes on the shard by being queued through the context's run queue, never by calling a receiver from inside a wake pass.
- `log::Logger::flush` spins forever when no `Logging` is alive. Runtime test binaries register `aloe::testing::LoggingEnvironment` once, in one file per binary.
- The fabric never refuses a transmit and refuses frames shorter than an Ethernet header. A test that needs a refused send wraps the port in `testing::TcpTestDevice`; a test of a short frame builds the packet on the port's own pool and calls the brick directly.
- `docs/superpowers/` is git-ignored; the spec and this plan are force-added.

**Where things are.** Module layout and target names are in `AGENTS.md`. Test registration helpers are in `cmake/tests.cmake`. The byte helpers are `wire::load_be16`, `load_be32`, `store_be16`, `store_be32`; the checksum helpers `wire::checksum_add`, `checksum_finish`, `internet_checksum`, `ipv4_pseudo_header_sum(source, destination, protocol, length)`, `ipv4_l4_checksum(source, destination, protocol, segment)`; the addresses `wire::Ipv4Address{10, 0, 0, 1}` with `bytes()`, `load`, `store`, `parse`, and `wire::MacAddress`. The IPv4 header pattern to copy is `common/wire/ipv4/ipv4_header.hpp`: a struct with defaulted fields, a `static constexpr std::optional<T> parse(std::span<const std::byte>) noexcept` and a `constexpr void write(std::span<std::byte>) const noexcept` that asserts the size.

## How to Execute and Review

Use `superpowers:subagent-driven-development`; `superpowers:executing-plans` is the fallback when no subagent tool exists. Before Task 1, commit the revised spec, the overview edit and this plan on `tcp/minimal-tcp` (force-add the two ignored files as commit `3ed151c` did), then create the implementation worktree with `superpowers:using-git-worktrees` from that commit; a worktree does not carry uncommitted work.

For each task, in order:

1. An implementer subagent gets the task text, the spec and `AGENTS.md`, and implements it test-first exactly as the steps say, committing at the end of the task.
2. A spec-compliance reviewer checks the diff against the task's "Interfaces" block and the spec sections the task names. It rejects renamed or missing names, behaviour the spec forbids, and scope beyond the task.
3. A code-quality reviewer checks the list below. Both reviewers get the diff and the spec, not the chat.
4. Fix findings, re-run the task's tests, and move on only when both reviewers approve.

After the last task, one whole-branch review against the spec's "Done when" list and the Review Focus below, then the verification in Task 14.

What the reviewers check for this repository, beyond correctness:

- Namespaces: `aloe::wire`, `aloe::stream`, `aloe::tcp`, `aloe::net`, `aloe::runtime`, `aloe::frames`, `aloe::testing`; other modules' names qualified; nothing declared directly in `aloe`; helpers in a nested `detail`.
- `grep -rn 'aloe/execution\|aloe/log\|stdexec\|exec::\|quill' common/tcp common/stream common/wire common/net fixtures` prints nothing. `grep -rn 'using namespace' common fixtures` prints nothing outside `std::chrono_literals` in tests.
- `grep -rn 'Aloe::Common::Execution\|Aloe::Common::Log\|Aloe::Dpdk' common/tcp/CMakeLists.txt common/stream/CMakeLists.txt` prints nothing.
- By-value parameters `const` in definitions, not in bodiless declarations; written-through spans unqualified.
- `[[nodiscard]]` on getters, factories and anything whose result is a bug to ignore; `explicit` on single-argument constructors; default member initialisers on every member.
- No macros in public headers; no `std::cout`, no logging in `tcp`, `stream`, `net`.
- The hot-path functions are `noexcept` and allocation-free except for packets; every list and table is sized at construction and never grown.
- Every drop and every send failure increments exactly one counter.
- Events are raised through `raise` only; nothing clears a connection's flags except `poll_event` and `release`.
- Tests pin behaviour; no test exists only to show a forwarding helper runs.
- Documentation updated in the same change where the task says so.
- Commit subjects are conventional with the module scope.

## Review Focus

Conditions the spec implies but does not spell out, most likely to bite first. Each is pinned by a test in the task that owns the code.

1. **A ready completion outlives its connection slot.** Release or cancel must stop or detach both parked and already queued operations before the slot is reused; a late work item cannot read a new connection's state. Pinned in Task 11 (`Streams.ReleaseQueuedCompletionThenReuseSlot`, `Streams.CancelQueuedCompletion`).
2. **The last handshake ACK also carries application data or a FIN.** The connection is established and the same segment keeps being processed; its tail is not discarded on the state transition. Pinned in Task 7 (`TcpReceive.FinalHandshakeAckCarriesData`) and Task 9 (`TcpClose.HandshakeAckWithFin`).
3. **The peer window shrinks, sequence numbers wrap, or a duplicate FIN arrives.** Serial arithmetic everywhere, usable credit clamped before unsigned subtraction, duplicate control acknowledged without advancing `rcv_nxt` twice. Pinned in Task 7 (`TcpWindow.WrapsNearTheTop`), Task 8 (`TcpSend.PeerWindowShrinkBelowInFlight`) and Task 9 (`TcpClose.DuplicateFin`).
4. **An ARP forward races a sibling's final drain.** Either the target accepts and runs the work, or the source keeps and frees the rejected node; no post to an abandoned inbox. Pinned in Task 10 (`ControlPost.AcceptedBeforeDrainOrRejectedAfterClosure`) and Task 12 (`TcpSteering.StopRacesArpForward`) under TSan and ASan.
5. **Receive metadata has no usable TCP RSS hash, or steering is disabled on a nonzero queue.** Software lookup agrees with insertion and an outbound open never silently chooses a queue replies cannot reach. Pinned in Task 5 (`TcpTable.HardwareSoftwareHashAgreement`), Task 6 (`TcpHandshake.PassiveOpen` on a single-queue port) and Task 9 (`TcpPlacement.DisabledSteeringCannotReachNonzeroQueue`).

---

## Concrete resolutions of the spec sketches

These decisions make the schematic declarations implementable. Their spellings are carried into the architecture pages and the spec's examples in Task 14.

1. **Backend-dependent types stay concrete.** `template <typename Ip> class tcp::Connection`, `template <device::IsPacket Packet> class tcp::ReadView` and `template <typename Ip> struct tcp::ConnectionEvent`. `Stack<Ip>` publishes `using ConnectionType = Connection<Ip>` and `using EventType = ConnectionEvent<Ip>`; its methods return `ConnectionType*`, `ConnectionType&` and `std::optional<EventType>`. The spec's bare names are shorthand for these. No type-erased packet, no virtual dispatch. The node pool is a template in a header; only the backend-independent `FlowTable` has a `.cpp`.
2. **Storage and flags.** Connections live in a `std::deque<ConnectionType>`, which never moves an element, constructed once with `emplace_back(PrivateTag{})` from `Stack` through the passkey idiom `fabric::Port` uses. Each slot's stack pointer, index and timer trampoline are set at construction. `stream::Events` is a class with named queries, constructible from an initializer list of `Event` and from a raw value. `ReadView<Packet>` is a lightweight `forward_range` without `view_interface`. Pool and list indices are 32-bit with the sentinel `detail::no_node`; capacity arithmetic is validated before allocation.
3. **IP seams.** `Ipv4`'s template parameter becomes `DeviceT` with `using Device = DeviceT`. `queue()` is exposed in mutable and const form, and `next_hop(destination) const -> std::optional<wire::Ipv4Address>` answers the route question with no ARP or packet side effect. TCP calls `next_hop` before taking a slot.
4. **Control retry counting.** `retries = 5` means five retransmissions after the initial control send, at 1, 3, 7, 15 and 31 seconds, then a timeout at 63 seconds when a sixth would be due. An `Unresolved` send polls at 10 ms without spending that budget. A successful `connect` owns a `SynSent` slot even if its first SYN is still waiting for ARP; it does not promise the SYN is already in the ring.
5. **Table hashing.** The logical key is remote address, remote port, local port; equality compares fields. The software base hash is FNV-1a over those eight network-order bytes; the hardware base hash is the card's `rss_hash`, or `device::flow_hash` of the received tuple when a packet lacks one, used only when the effective description hashes IPv4 TCP tuples. Every bucket decision applies the spec's finalizer then masks. Queue prediction always uses the received orientation and the unmixed RSS hash.
6. **Runtime handle naming.** `Stream<Stack>::send(bytes)` and `close()` are senders; the brick's synchronous `send` and `close` are reached through `connection()`. `writable()` is the byte query and `writable(n)` the sender. The brick connection models `IsStream`; the runtime owner does not, by design.
7. **Typed ARP delivery through device-free contexts.** `ShardContext` gains a cold-path `ArpSink` of `wire::Ipv4Address`, `wire::MacAddress` and the stamp, which names no device, no `net` type and adds no link dependency; `runtime::TcpStack<Device>` registers a trampoline to its own `ip.learn`. Siblings post guarded control work through `post_control`; Tasks 10 and 12 define admission and closure.
8. **Ring integration correction.** A scripted peer at another IP address drives the brick over real ring-driver mbufs, as `test_net_ring.cpp` does for UDP. Martian filtering stays. The two-real-brick echo is covered on the fabric.

Stream vocabulary, TCP and the runtime adapter depend on each other's contracts, so this is one plan with explicit intermediate gates.

## File Structure

| Area | Files to create | Existing files to extend |
|---|---|---|
| TCP wire values | `common/wire/tcp/tcp_sequence.hpp`, `tcp_header.hpp`, `tcp_options.hpp` | `common/wire/CMakeLists.txt`, `common/wire/export/aloe/wire`, `docs/architecture/wire.md` |
| Stream vocabulary | `common/stream/CMakeLists.txt`, `export/aloe/stream`, `stream/stream.hpp`, `stream/stream_events.hpp`, `docs/architecture/stream.md` | `common/CMakeLists.txt` |
| TCP state, table, storage | `common/tcp/CMakeLists.txt`, `export/aloe/tcp`, `state/tcp_config.hpp`, `state/tcp_counters.hpp`, `table/flow_table.hpp`, `table/flow_table.cpp`, `buffer/tcp_node_pool.hpp`, `buffer/read_view.hpp` | `common/net/arp/arp_cache.hpp` (the TODO goes) |
| TCP brick | `common/tcp/stack/tcp_fwd.hpp`, `tcp_connection.hpp`, `tcp_stack.hpp`, `tcp_receive.hpp`, `tcp_transmit.hpp`, `tcp_control.hpp`, `docs/architecture/tcp.md` | `common/net/stack/ipv4.hpp` |
| Public fixtures | `fixtures/frames/frames/tcp_frames.hpp`, `tcp_peer.hpp` | `fixtures/frames/frames/net_frames.hpp`, `fixtures/frames/CMakeLists.txt`, `fixtures/frames/export/aloe/frames`, `docs/architecture/fixtures.md` |
| Private test fixtures | `tests/shared/tcp/tcp_fixture.hpp`, `tcp_test_device.hpp` | `tests/shared/CMakeLists.txt` |
| Runtime tick and control | | `common/runtime/context/shard_context.hpp`, `.cpp`, `shard/shard.hpp`, `runtime/runtime.hpp`, `docs/architecture/runtime.md` |
| Runtime streams | `common/runtime/stream/streams.hpp`, `stream_handle.hpp`, `stream_senders.hpp`, `stream_wait.hpp` | `common/runtime/CMakeLists.txt`, `common/runtime/export/aloe/runtime` |
| Runtime TCP composition | `common/runtime/stack/tcp_runtime_stack.hpp`, `arp_forward.hpp` | as above |
| Tests | `tests/unit_tests/common/wire/test_wire_tcp.cpp`; `tests/unit_tests/common/stream/CMakeLists.txt`, `test_stream.cpp`; `tests/unit_tests/fixtures/frames/CMakeLists.txt`, `test_tcp_frames.cpp`; `tests/unit_tests/common/tcp/CMakeLists.txt`, `test_tcp_table.cpp`, `test_tcp_buffers.cpp`, `test_tcp_handshake.cpp`, `test_tcp_events.cpp`, `test_tcp_data.cpp`, `test_tcp_window.cpp`, `test_tcp_acks.cpp`, `test_tcp_close.cpp`, `test_tcp_two_stacks.cpp`, `test_tcp_placement.cpp`; `tests/unit_tests/common/runtime/test_streams.cpp`, `test_tcp_runtime.cpp`, `test_tcp_steering.cpp`; `tests/integration_tests/tcp/CMakeLists.txt`, `test_tcp_ring.cpp`; `tests/manual_tests/tcp/CMakeLists.txt`, `test_tcp_tap.cpp` | `tests/unit_tests/common/net/test_net_send.cpp`, `runtime/test_shard.cpp`, `test_shard_context.cpp`, `test_runtime.cpp`, `test_cross_shard.cpp`, the parent CMake files |
| Examples and docs | `examples/tcp_echo/CMakeLists.txt`, `tcp_echo.cpp`, `examples/tcp_echo_tasks/CMakeLists.txt`, `tcp_echo_tasks.cpp` | `examples/CMakeLists.txt`, `README.md`, `AGENTS.md`, `codestyle.md`, `docs/roadmap.md`, `docs/architecture/overview.md`, `loop.md`, `net.md`, `docs/guides/getting-started.md`, the spec |

Every helper not named in the spec lives in the module's `detail` namespace. `tcp_stack.hpp` declares the template and includes `tcp_receive.hpp`, `tcp_transmit.hpp` and `tcp_control.hpp` at its end for the out-of-class definitions; consumers include `<aloe/tcp>`. Packet ownership stays in connections and nodes, never in the backend-independent table.

Unqualified TCP test files live in `tests/unit_tests/common/tcp/`, unqualified runtime test files in `tests/unit_tests/common/runtime/`. `common/CMakeLists.txt` lists `net`, `stream`, `tcp` and then `runtime`, since runtime links all three. The TCP module combines a static `.Table` target and an interface `.Stack` target through `add_combined_library`. The shared TCP fixture target is `${SHARED_TESTING_TARGET}.Tcp`.

## Execution and validation conventions

For each task: add its test and build registration first, run the focused command and observe a missing declaration or a failed assertion attributable to that task, then implement and run again. A missing dependency or a broken toolchain is not a useful red result. Commit only after the green result and the format check; scope each commit to the task's files. The test code in a task is the test to write, not a summary; every named case is required.

## Task 1: TCP wire formats

**Files:**
- Create: `common/wire/tcp/tcp_sequence.hpp`, `common/wire/tcp/tcp_header.hpp`, `common/wire/tcp/tcp_options.hpp`
- Create: `tests/unit_tests/common/wire/test_wire_tcp.cpp`
- Modify: `common/wire/CMakeLists.txt`, `common/wire/export/aloe/wire`, `tests/unit_tests/common/wire/CMakeLists.txt`, `docs/architecture/wire.md`

**Interfaces:**
- Consumes: `wire::load_be16`, `load_be32`, `store_be16`, `store_be32`.
- Produces: `wire::TcpSequence{value}` with `operator+(std::uint32_t)`, `operator-(TcpSequence) -> std::int32_t`, `before`, `after`, `==`; `wire::TcpFlag`; `wire::TcpFlags` with `TcpFlags{}`, `TcpFlags{TcpFlag::Syn, TcpFlag::Ack}`, `has`, `raw`, `from_raw`, `==`; `wire::TcpHeader` with `size`, `checksum_offset`, the fields of the spec, `parse` and `write`; `wire::TcpOptions` with `mss`, `mss_size`, `parse` and `write_mss`. Data offset is stored in bytes and encoded in four-byte words.

- [ ] **Step 1: Write the failing tests**

`tests/unit_tests/common/wire/test_wire_tcp.cpp`:

```cpp
#include <aloe/wire>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;
    using aloe::wire::TcpHeader;
    using aloe::wire::TcpOptions;
    using aloe::wire::TcpSequence;

    [[nodiscard]] std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
        std::vector<std::byte> out;
        for (const unsigned value : values) {
            out.push_back(std::byte{static_cast<std::uint8_t>(value)});
        }
        return out;
    }

    // Ports 80 -> 40000, sequence 1, acknowledgement 2, offset 20, ACK, window 65535, checksum 0, urgent 0.
    const std::vector<std::byte> ack_header =
        bytes({0x00, 0x50, 0x9c, 0x40, 0, 0, 0, 1, 0, 0, 0, 2, 0x50, 0x10, 0xff, 0xff, 0, 0, 0, 0});

}  // namespace

TEST(TcpSequence, WrapsAndCompares) {
    EXPECT_EQ((TcpSequence{0xfffffff0U} + 32U).value, 16U);
    EXPECT_EQ(TcpSequence{16U} - TcpSequence{0xfffffff0U}, 32);
    EXPECT_EQ(TcpSequence{0xfffffff0U} - TcpSequence{16U}, -32);
    EXPECT_TRUE(TcpSequence{0xfffffff0U}.before(TcpSequence{16U}));
    EXPECT_TRUE(TcpSequence{16U}.after(TcpSequence{0xfffffff0U}));
    EXPECT_FALSE(TcpSequence{5U}.before(TcpSequence{5U}));
    EXPECT_FALSE(TcpSequence{5U}.after(TcpSequence{5U}));
    EXPECT_EQ(TcpSequence{5U}, TcpSequence{5U});
    EXPECT_TRUE(TcpSequence{0U}.after(TcpSequence{0x80000001U}));  // half the space away: serial order
}

TEST(TcpFlags, SetQueryAndRaw) {
    const TcpFlags flags{TcpFlag::Syn, TcpFlag::Ack};
    EXPECT_TRUE(flags.has(TcpFlag::Syn));
    EXPECT_TRUE(flags.has(TcpFlag::Ack));
    EXPECT_FALSE(flags.has(TcpFlag::Fin));
    EXPECT_EQ(flags.raw(), 0x12U);
    EXPECT_EQ(TcpFlags::from_raw(0xffU).raw(), 0xffU);
    EXPECT_TRUE(TcpFlags::from_raw(0x80U).has(TcpFlag::Cwr));
    EXPECT_EQ(TcpFlags{}.raw(), 0U);
    EXPECT_EQ(TcpFlags::from_raw(0x12U), flags);
}

TEST(TcpHeader, SizeAndChecksumOffset) {
    EXPECT_EQ(TcpHeader::size, 20U);
    EXPECT_EQ(TcpHeader::checksum_offset, 16U);
}

TEST(TcpHeader, ParsesKnownBytes) {
    const auto header = TcpHeader::parse(ack_header);
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->source_port, 80U);
    EXPECT_EQ(header->destination_port, 40000U);
    EXPECT_EQ(header->sequence, TcpSequence{1U});
    EXPECT_EQ(header->acknowledgement, TcpSequence{2U});
    EXPECT_EQ(header->data_offset, 20U);
    EXPECT_EQ(header->flags, (TcpFlags{TcpFlag::Ack}));
    EXPECT_EQ(header->window, 65535U);
    EXPECT_EQ(header->checksum, 0U);
    EXPECT_EQ(header->urgent_pointer, 0U);
}

TEST(TcpHeader, WritesKnownBytes) {
    std::array<std::byte, 20> out{};
    TcpHeader{.source_port      = 80,
              .destination_port = 40000,
              .sequence         = TcpSequence{1U},
              .acknowledgement  = TcpSequence{2U},
              .data_offset      = 20,
              .flags            = TcpFlags{TcpFlag::Ack},
              .window           = 65535}
        .write(out);
    EXPECT_EQ(std::vector<std::byte>(out.begin(), out.end()), ack_header);
}

TEST(TcpHeader, RoundTripWithOptionsAndEveryFlag) {
    std::array<std::byte, 24> out{};
    const TcpHeader written{.source_port      = 7,
                            .destination_port = 32768,
                            .sequence         = TcpSequence{0xdeadbeefU},
                            .acknowledgement  = TcpSequence{0x01020304U},
                            .data_offset      = 24,
                            .flags            = TcpFlags::from_raw(0xff),
                            .window           = 1,
                            .checksum         = 0xabcd,
                            .urgent_pointer   = 9};
    written.write(out);
    EXPECT_EQ(out[12], std::byte{0x60}) << "24 bytes is six words in the high nibble";
    const auto parsed = TcpHeader::parse(out);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, written);
}

TEST(TcpHeader, RejectsShortSegments) {
    for (std::size_t length = 0; length < 20; ++length) {
        EXPECT_FALSE(TcpHeader::parse(std::span<const std::byte>{ack_header}.first(length))) << length;
    }
}

TEST(TcpHeader, RejectsInvalidDataOffset) {
    auto under = ack_header;
    under[12]  = std::byte{0x40};  // four words: 16 bytes
    EXPECT_FALSE(TcpHeader::parse(under));
    auto beyond = ack_header;
    beyond[12]  = std::byte{0x60};  // 24 bytes, but the segment is 20
    EXPECT_FALSE(TcpHeader::parse(beyond));
    auto exact = ack_header;
    exact.resize(24);
    exact[12] = std::byte{0x60};
    EXPECT_TRUE(TcpHeader::parse(exact)) << "an offset equal to the segment length is a header without data";
}

TEST(TcpOptions, ParsesMssAndSkipsUnknown) {
    EXPECT_EQ(TcpOptions::mss_size, 4U);
    const auto mss = TcpOptions::parse(bytes({2, 4, 0x05, 0xb4}));
    ASSERT_TRUE(mss.has_value());
    EXPECT_EQ(mss->mss, 1460U);
    const auto small = TcpOptions::parse(bytes({2, 4, 0x02, 0x18}));
    ASSERT_TRUE(small.has_value());
    EXPECT_EQ(small->mss, 536U);
    // A Linux SYN: MSS, SACK permitted, timestamps, NOP, window scale, then EOL padding.
    const auto linux = TcpOptions::parse(bytes({2, 4, 0x05, 0xb4, 4, 2, 8, 10, 1, 2, 3, 4, 0, 0, 0, 0, 1, 3, 3, 7, 0}));
    ASSERT_TRUE(linux.has_value());
    EXPECT_EQ(linux->mss, 1460U);
    const auto none = TcpOptions::parse(bytes({1, 1, 0}));
    ASSERT_TRUE(none.has_value());
    EXPECT_FALSE(none->mss.has_value());
    const auto empty = TcpOptions::parse({});
    ASSERT_TRUE(empty.has_value());
    EXPECT_FALSE(empty->mss.has_value());
    const auto after_eol = TcpOptions::parse(bytes({0, 2, 4, 0x05, 0xb4}));
    ASSERT_TRUE(after_eol.has_value());
    EXPECT_FALSE(after_eol->mss.has_value()) << "nothing after EOL is read";
}

TEST(TcpOptions, RejectsBadLengths) {
    EXPECT_FALSE(TcpOptions::parse(bytes({2, 4, 0x05})));          // MSS truncated
    EXPECT_FALSE(TcpOptions::parse(bytes({2, 3, 0x05, 0xb4})));    // MSS with the wrong length
    EXPECT_FALSE(TcpOptions::parse(bytes({8, 0, 1, 2})));          // length 0
    EXPECT_FALSE(TcpOptions::parse(bytes({8, 1, 1, 2})));          // length 1
    EXPECT_FALSE(TcpOptions::parse(bytes({8, 10, 1, 2})));         // length past the option area
    EXPECT_FALSE(TcpOptions::parse(bytes({8})));                   // a kind with no length byte
}

TEST(TcpOptions, WritesMss) {
    std::array<std::byte, 4> out{};
    TcpOptions::write_mss(out, 1460);
    EXPECT_EQ(std::vector<std::byte>(out.begin(), out.end()), bytes({2, 4, 0x05, 0xb4}));
    const auto back = TcpOptions::parse(out);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->mss, 1460U);
}
```

Add `test_wire_tcp.cpp` to `Aloe.Tests.Unit.Wire` in `tests/unit_tests/common/wire/CMakeLists.txt`.

- [ ] **Step 2: Run the test to see it fail**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Wire`
Expected: compile errors, `no member named 'TcpSequence' in namespace 'aloe::wire'`.

- [ ] **Step 3: Write the three headers**

`common/wire/tcp/tcp_sequence.hpp`:

```cpp
#pragma once

#include <cstdint>

namespace aloe::wire {

    /**
     * @brief A TCP sequence number with RFC 793 serial arithmetic.
     *
     * `a - b` is the wrapped signed distance, so `before` and `after` compare across the wrap: a
     * number half the space away or less counts as later. Unsigned addition wraps by definition;
     * the conversion to `std::int32_t` is modular since C++20, so nothing here overflows.
     */
    struct TcpSequence {
        std::uint32_t value = 0;

        [[nodiscard]] constexpr TcpSequence operator+(const std::uint32_t count) const noexcept {
            return TcpSequence{value + count};
        }

        /// This minus `other`, wrapped into the signed range.
        [[nodiscard]] constexpr std::int32_t operator-(const TcpSequence other) const noexcept {
            return static_cast<std::int32_t>(value - other.value);
        }

        [[nodiscard]] constexpr bool before(const TcpSequence other) const noexcept {
            return (*this - other) < 0;
        }

        [[nodiscard]] constexpr bool after(const TcpSequence other) const noexcept {
            return (*this - other) > 0;
        }

        friend constexpr bool operator==(TcpSequence, TcpSequence) noexcept = default;
    };

}  // namespace aloe::wire
```

`common/wire/tcp/tcp_header.hpp`:

```cpp
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <utility>

#include <bytes.hpp>
#include <tcp_sequence.hpp>

namespace aloe::wire {

    enum class TcpFlag : std::uint8_t {
        Fin = 0x01,
        Syn = 0x02,
        Rst = 0x04,
        Psh = 0x08,
        Ack = 0x10,
        Urg = 0x20,
        Ece = 0x40,
        Cwr = 0x80,
    };

    /// A set of TcpFlag; `raw()` is the wire byte.
    class TcpFlags {
    public:
        constexpr TcpFlags() noexcept = default;

        constexpr TcpFlags(const std::initializer_list<TcpFlag> flags) noexcept {
            for (const TcpFlag flag : flags) {
                raw_ = static_cast<std::uint8_t>(raw_ | std::to_underlying(flag));
            }
        }

        [[nodiscard]] static constexpr TcpFlags from_raw(const std::uint8_t byte) noexcept {
            TcpFlags flags;
            flags.raw_ = byte;
            return flags;
        }

        [[nodiscard]] constexpr bool has(const TcpFlag flag) const noexcept {
            return (raw_ & std::to_underlying(flag)) != 0;
        }

        [[nodiscard]] constexpr std::uint8_t raw() const noexcept {
            return raw_;
        }

        friend constexpr bool operator==(const TcpFlags&, const TcpFlags&) noexcept = default;

    private:
        std::uint8_t raw_ = 0;
    };

    /**
     * @brief A TCP header as the brick reads and writes it.
     *
     * `data_offset` is in bytes, 20 to 60; the wire encodes it in four-byte words. Options are
     * parsed separately by `TcpOptions` from the bytes between the fixed header and the offset.
     * `checksum` is the field as found on parse and as given on write: zero when the IP brick
     * fills it.
     */
    struct TcpHeader {
        static constexpr std::size_t size            = 20;
        static constexpr std::size_t checksum_offset = 16;
        static constexpr std::size_t max_size        = 60;

        std::uint16_t source_port      = 0;
        std::uint16_t destination_port = 0;
        TcpSequence sequence{};
        TcpSequence acknowledgement{};
        std::uint8_t data_offset = 20;  ///< Bytes, 20 to 60.
        TcpFlags flags{};
        std::uint16_t window         = 0;
        std::uint16_t checksum       = 0;
        std::uint16_t urgent_pointer = 0;

        /// Nothing under 20 bytes, for a data offset under 20 or past the segment.
        [[nodiscard]] static constexpr std::optional<TcpHeader>
        parse(const std::span<const std::byte> segment) noexcept {
            if (segment.size() < size) {
                return std::nullopt;
            }
            const std::size_t offset = (std::to_integer<unsigned>(segment[12]) >> 4U) * 4U;
            if (offset < size || offset > segment.size()) {
                return std::nullopt;
            }
            return TcpHeader{
                .source_port      = load_be16(segment.subspan(0, 2)),
                .destination_port = load_be16(segment.subspan(2, 2)),
                .sequence         = TcpSequence{load_be32(segment.subspan(4, 4))},
                .acknowledgement  = TcpSequence{load_be32(segment.subspan(8, 4))},
                .data_offset      = static_cast<std::uint8_t>(offset),
                .flags            = TcpFlags::from_raw(std::to_integer<std::uint8_t>(segment[13])),
                .window           = load_be16(segment.subspan(14, 2)),
                .checksum         = load_be16(segment.subspan(checksum_offset, 2)),
                .urgent_pointer   = load_be16(segment.subspan(18, 2)),
            };
        }

        /// Writes the 20 fixed bytes into the front of `out`; options are written separately after them.
        constexpr void write(const std::span<std::byte> out) const noexcept {
            assert(out.size() >= size);
            assert(data_offset >= size && data_offset <= max_size && data_offset % 4 == 0);
            store_be16(out.subspan(0, 2), source_port);
            store_be16(out.subspan(2, 2), destination_port);
            store_be32(out.subspan(4, 4), sequence.value);
            store_be32(out.subspan(8, 4), acknowledgement.value);
            out[12] = std::byte{static_cast<std::uint8_t>((data_offset / 4U) << 4U)};
            out[13] = std::byte{flags.raw()};
            store_be16(out.subspan(14, 2), window);
            store_be16(out.subspan(checksum_offset, 2), checksum);
            store_be16(out.subspan(18, 2), urgent_pointer);
        }

        friend constexpr bool operator==(const TcpHeader&, const TcpHeader&) noexcept = default;
    };

}  // namespace aloe::wire
```

`common/wire/tcp/tcp_options.hpp`:

```cpp
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <bytes.hpp>

namespace aloe::wire {

    /**
     * @brief The TCP options the brick reads and writes: the maximum segment size, and nothing else.
     *
     * `parse` walks the option list between the fixed header and the data offset: end-of-list
     * stops it, no-operation is one byte, every other kind is skipped by its length byte. A length
     * of 0 or 1, a length reaching past the list, a kind without a length byte, or an MSS whose
     * length is not 4, is malformed and the brick drops the segment.
     */
    struct TcpOptions {
        static constexpr std::size_t mss_size = 4;  ///< Kind 2, length 4, two bytes of value.

        std::optional<std::uint16_t> mss;

        [[nodiscard]] static constexpr std::optional<TcpOptions>
        parse(const std::span<const std::byte> options) noexcept {
            TcpOptions parsed;
            std::size_t at = 0;
            while (at < options.size()) {
                const auto kind = std::to_integer<std::uint8_t>(options[at]);
                if (kind == 0) {
                    break;  // end of list
                }
                if (kind == 1) {
                    ++at;  // no-operation
                    continue;
                }
                if (at + 1 >= options.size()) {
                    return std::nullopt;  // a kind with no length byte
                }
                const auto length = std::to_integer<std::size_t>(options[at + 1]);
                if (length < 2 || at + length > options.size()) {
                    return std::nullopt;
                }
                if (kind == 2) {
                    if (length != mss_size) {
                        return std::nullopt;
                    }
                    parsed.mss = load_be16(options.subspan(at + 2, 2));
                }
                at += length;
            }
            return parsed;
        }

        /// Writes the four-byte MSS option into the front of `out`.
        static constexpr void write_mss(const std::span<std::byte> out, const std::uint16_t mss) noexcept {
            assert(out.size() >= mss_size);
            out[0] = std::byte{2};
            out[1] = std::byte{mss_size};
            store_be16(out.subspan(2, 2), mss);
        }
    };

}  // namespace aloe::wire
```

Add to `common/wire/CMakeLists.txt`, after the ICMP block and before the exported library, and list `${WIRE}.Tcp` in the `add_combined_library` call:

```cmake
##############################################################################
# TCP: sequence arithmetic, the header and the options the brick reads
##############################################################################
add_library(${WIRE}.Tcp INTERFACE
        tcp/tcp_sequence.hpp
        tcp/tcp_header.hpp
        tcp/tcp_options.hpp
)
target_include_directories(${WIRE}.Tcp INTERFACE
        tcp/
)
target_link_libraries(${WIRE}.Tcp INTERFACE
        ${WIRE}.Bytes
)
##############################################################################
```

Add to `common/wire/export/aloe/wire`, keeping the includes sorted:

```cpp
#include <tcp_header.hpp>
#include <tcp_options.hpp>
#include <tcp_sequence.hpp>
```

- [ ] **Step 4: Run the test to see it pass**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Wire && ctest --preset debug -R '^Aloe[.]Tests[.]Unit[.]Wire$' --output-on-failure`
Expected: PASS, all eleven new cases listed.

- [ ] **Step 5: Document and format**

In `docs/architecture/wire.md`, add `tcp/` to the module description and a row under "Key types": `TcpSequence` (serial arithmetic), `TcpFlag`/`TcpFlags`, `TcpHeader` (offset in bytes, options parsed separately), `TcpOptions` (MSS only; malformed lists are dropped by the brick). Run `./scripts/check-format.sh`.

- [ ] **Step 6: Commit**

```bash
git add common/wire tests/unit_tests/common/wire docs/architecture/wire.md
git commit -m "feat(wire): add TCP sequence arithmetic and formats"
```

## Task 2: Stream vocabulary and the convenience send helper

**Files:**
- Create: `common/stream/CMakeLists.txt`, `common/stream/export/aloe/stream`, `common/stream/stream/stream_events.hpp`, `common/stream/stream/stream.hpp`
- Create: `tests/unit_tests/common/stream/CMakeLists.txt`, `tests/unit_tests/common/stream/test_stream.cpp`
- Create: `docs/architecture/stream.md`
- Modify: `common/CMakeLists.txt`, `tests/unit_tests/common/CMakeLists.txt`

**Interfaces:**
- Consumes: `Aloe::Common::Core` only.
- Produces: `stream::Event`, `stream::Events` (default empty; `Events{Event::Readable, Event::Acked}`; `from_raw`; `has`; `connected`, `accepted`, `readable`, `writable`, `acked`, `peer_closed`, `closed`, `reset`, `timed_out`; `any`; `raw`; `operator|=` for `Events` and `Event`; `==`), `stream::Error`, `stream::IsReadView<V>`, `stream::IsStream<S>`, `template <IsStream S> std::size_t stream::send(S&, std::span<const std::byte>) noexcept`.

- [ ] **Step 1: Write the failing tests over a stub stream**

`tests/unit_tests/common/stream/test_stream.cpp`:

```cpp
#include <aloe/stream>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using aloe::stream::Error;
    using aloe::stream::Event;
    using aloe::stream::Events;

    /// A read view over a fixed set of chunks: the shape the concept wants, with the byte count separate.
    class ChunkView {
    public:
        using Chunks   = std::vector<std::span<const std::byte>>;
        using iterator = Chunks::const_iterator;

        explicit ChunkView(Chunks chunks)
            : chunks_{std::move(chunks)} {
            for (const auto chunk : chunks_) {
                bytes_ += chunk.size();
            }
        }

        [[nodiscard]] std::size_t size() const noexcept { return bytes_; }
        [[nodiscard]] bool empty() const noexcept { return bytes_ == 0; }
        [[nodiscard]] std::span<const std::byte> front() const noexcept { return chunks_.front(); }
        [[nodiscard]] iterator begin() const noexcept { return chunks_.begin(); }
        [[nodiscard]] iterator end() const noexcept { return chunks_.end(); }

    private:
        Chunks chunks_;
        std::size_t bytes_ = 0;
    };

    /// A stream whose prepare gives at most `segment` bytes and whose commit can be told to refuse.
    class StubStream {
    public:
        std::size_t segment              = 3;
        std::size_t refuse_commit_number = 0;  ///< 1-based; 0 never refuses.
        std::size_t null_prepare_after   = 0;  ///< Prepares after this many succeed return nothing; 0 means never.
        std::vector<std::size_t> prepared_sizes;
        std::vector<std::size_t> committed_sizes;
        std::vector<std::byte> wire;

        [[nodiscard]] std::uint32_t index() const noexcept { return 0; }
        [[nodiscard]] Events events() const noexcept { return {}; }
        [[nodiscard]] ChunkView unread() const noexcept { return ChunkView{{}}; }
        void consume(std::size_t) noexcept {}
        [[nodiscard]] bool peer_closed() const noexcept { return false; }
        [[nodiscard]] std::size_t writable() const noexcept { return segment; }

        [[nodiscard]] std::optional<std::span<std::byte>> prepare(const std::size_t count) noexcept {
            if (count == 0 || (null_prepare_after != 0 && prepared_sizes.size() >= null_prepare_after)) {
                return std::nullopt;
            }
            const std::size_t size = std::min(count, segment);
            prepared_sizes.push_back(size);
            buffer_.assign(size, std::byte{0});
            return std::span<std::byte>{buffer_};
        }

        [[nodiscard]] bool commit(const std::size_t count) noexcept {
            ++commits_;
            if (count == 0) {
                return true;
            }
            if (commits_ == refuse_commit_number) {
                return false;
            }
            committed_sizes.push_back(count);
            wire.insert(wire.end(), buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(count));
            return true;
        }

        void close() noexcept {}
        void abort() noexcept {}
        void release() noexcept {}

    private:
        std::vector<std::byte> buffer_;
        std::size_t commits_ = 0;
    };

    static_assert(aloe::stream::IsReadView<ChunkView>);
    static_assert(aloe::stream::IsStream<StubStream>);
    static_assert(!aloe::stream::IsReadView<std::vector<std::byte>>, "a byte range is not a view of chunks");
    static_assert(!aloe::stream::IsStream<ChunkView>);

    [[nodiscard]] std::vector<std::byte> five() {
        return {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
    }

}  // namespace

TEST(StreamEvents, DefaultEmptyNamedQueriesAndRaw) {
    const Events none;
    EXPECT_FALSE(none.any());
    EXPECT_EQ(none.raw(), 0U);
    Events some{Event::Readable, Event::Acked};
    EXPECT_TRUE(some.any());
    EXPECT_TRUE(some.readable());
    EXPECT_TRUE(some.acked());
    EXPECT_FALSE(some.writable());
    EXPECT_TRUE(some.has(Event::Readable));
    EXPECT_EQ(some.raw(), (1U << 2U) | (1U << 4U));
    some |= Event::PeerClosed;
    EXPECT_TRUE(some.peer_closed());
    some |= Events{Event::Closed, Event::Reset};
    EXPECT_TRUE(some.closed());
    EXPECT_TRUE(some.reset());
    EXPECT_FALSE(some.timed_out());
    EXPECT_EQ(Events::from_raw(1U << 8U), Events{Event::TimedOut});
    EXPECT_TRUE(Events{Event::Connected}.connected());
    EXPECT_TRUE(Events{Event::Accepted}.accepted());
}

TEST(StreamSend, SplitsAtWritable) {
    StubStream stub;
    const auto bytes = five();
    EXPECT_EQ(aloe::stream::send(stub, bytes), 5U);
    EXPECT_EQ(stub.committed_sizes, (std::vector<std::size_t>{3, 2}));
    EXPECT_EQ(stub.wire, bytes);
}

TEST(StreamSend, StopsOnRefusedCommit) {
    StubStream stub;
    stub.refuse_commit_number = 2;
    EXPECT_EQ(aloe::stream::send(stub, five()), 3U);
    EXPECT_EQ(stub.committed_sizes, (std::vector<std::size_t>{3}));
    EXPECT_EQ(stub.prepared_sizes.size(), 2U) << "no third attempt after a refusal";
}

TEST(StreamSend, StopsOnNullPrepare) {
    StubStream stub;
    stub.null_prepare_after = 1;
    EXPECT_EQ(aloe::stream::send(stub, five()), 3U);
    EXPECT_EQ(stub.prepared_sizes.size(), 1U);
}

TEST(StreamSend, EmptyInputMakesNoPrepare) {
    StubStream stub;
    EXPECT_EQ(aloe::stream::send(stub, {}), 0U);
    EXPECT_TRUE(stub.prepared_sizes.empty());
}

TEST(StreamSend, DoesNotCoalesceCalls) {
    StubStream stub;
    stub.segment = 10;
    const std::array<std::byte, 2> a{std::byte{1}, std::byte{2}};
    const std::array<std::byte, 2> b{std::byte{3}, std::byte{4}};
    EXPECT_EQ(aloe::stream::send(stub, a), 2U);
    EXPECT_EQ(aloe::stream::send(stub, b), 2U);
    EXPECT_EQ(stub.committed_sizes, (std::vector<std::size_t>{2, 2})) << "two calls, two segments";
}

TEST(StreamError, HasEveryValue) {
    EXPECT_NE(Error::Reset, Error::Closed);
    EXPECT_EQ(static_cast<int>(Error::Closed), 8) << "Reset, TimedOut, PeerClosed, Refused, Unplaceable, TableFull, NoPort, NoRoute, Closed";
}
```

`tests/unit_tests/common/stream/CMakeLists.txt`:

```cmake
##############################################################################
# Test Stream: the zero-copy stream contract over a stub
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Stream
        test_stream.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Stream
        PRIVATE
        Aloe::Common::Stream
        ${TEST_LIBS}
)
##############################################################################
```

Add `add_subdirectory(stream)` to `tests/unit_tests/common/CMakeLists.txt` after `net`.

- [ ] **Step 2: Run the test to see it fail**

Run: `cmake --preset debug`
Expected: configure fails, `Aloe::Common::Stream` is not a target (the test registration exists, the module does not).

- [ ] **Step 3: Write the module**

`common/stream/stream/stream_events.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <initializer_list>
#include <utility>

namespace aloe::stream {

    /// What a stream reports; each is an edge, raised when the condition becomes true.
    enum class Event : std::uint16_t {
        Connected  = 1U << 0U,  ///< An active open reached ESTABLISHED.
        Accepted   = 1U << 1U,  ///< A passive open reached ESTABLISHED; the application owns it from here.
        Readable   = 1U << 2U,  ///< Bytes were added to `unread()`.
        Writable   = 1U << 3U,  ///< `writable()` increased, or a refused operation may be retried.
        Acked      = 1U << 4U,  ///< The peer acknowledged more of what was committed.
        PeerClosed = 1U << 5U,  ///< The peer's FIN arrived; `unread()` still holds what came before it.
        Closed     = 1U << 6U,  ///< Both directions are done, or `abort()` was called.
        Reset      = 1U << 7U,  ///< The peer sent RST.
        TimedOut   = 1U << 8U,  ///< A control retransmit ran out of tries.
    };

    /// A set of Event with one query per member.
    class Events {
    public:
        constexpr Events() noexcept = default;

        constexpr Events(const std::initializer_list<Event> events) noexcept {
            for (const Event event : events) {
                raw_ = static_cast<std::uint16_t>(raw_ | std::to_underlying(event));
            }
        }

        [[nodiscard]] static constexpr Events from_raw(const std::uint16_t raw) noexcept {
            Events events;
            events.raw_ = raw;
            return events;
        }

        [[nodiscard]] constexpr bool has(const Event event) const noexcept {
            return (raw_ & std::to_underlying(event)) != 0;
        }

        [[nodiscard]] constexpr bool connected() const noexcept { return has(Event::Connected); }
        [[nodiscard]] constexpr bool accepted() const noexcept { return has(Event::Accepted); }
        [[nodiscard]] constexpr bool readable() const noexcept { return has(Event::Readable); }
        [[nodiscard]] constexpr bool writable() const noexcept { return has(Event::Writable); }
        [[nodiscard]] constexpr bool acked() const noexcept { return has(Event::Acked); }
        [[nodiscard]] constexpr bool peer_closed() const noexcept { return has(Event::PeerClosed); }
        [[nodiscard]] constexpr bool closed() const noexcept { return has(Event::Closed); }
        [[nodiscard]] constexpr bool reset() const noexcept { return has(Event::Reset); }
        [[nodiscard]] constexpr bool timed_out() const noexcept { return has(Event::TimedOut); }

        [[nodiscard]] constexpr bool any() const noexcept { return raw_ != 0; }
        [[nodiscard]] constexpr std::uint16_t raw() const noexcept { return raw_; }

        constexpr Events& operator|=(const Event event) noexcept {
            raw_ = static_cast<std::uint16_t>(raw_ | std::to_underlying(event));
            return *this;
        }

        constexpr Events& operator|=(const Events events) noexcept {
            raw_ = static_cast<std::uint16_t>(raw_ | events.raw_);
            return *this;
        }

        friend constexpr bool operator==(const Events&, const Events&) noexcept = default;

    private:
        std::uint16_t raw_ = 0;
    };

    /// Why an operation on a stream did not succeed; the runtime's senders carry it in the value channel.
    enum class Error : std::uint8_t {
        Reset,        ///< The peer sent RST.
        TimedOut,     ///< A control retransmit ran out of tries.
        PeerClosed,   ///< The peer closed before the requested bytes arrived.
        Refused,      ///< An active open was reset, or a listener was in use.
        Unplaceable,  ///< No local port makes the card deliver the flow to this shard.
        TableFull,    ///< No free connection slot, or no free listener.
        NoPort,       ///< Every ephemeral port is taken or unplaceable.
        NoRoute,      ///< The peer is off the subnet and no gateway is configured.
        Closed,       ///< The connection is closed; the operation cannot complete.
    };

}  // namespace aloe::stream
```

`common/stream/stream/stream.hpp`:

```cpp
#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>

#include <stream_events.hpp>

namespace aloe::stream {

    /**
     * @brief A view over the unread bytes of a stream: a forward range of chunks, each a span into
     * the layer's own storage, with `size()` counting bytes, not chunks.
     */
    template <typename V>
    concept IsReadView = std::ranges::forward_range<V> &&
                         std::same_as<std::ranges::range_value_t<V>, std::span<const std::byte>> &&
                         requires(const V& view) {
                             { view.size() } -> std::same_as<std::size_t>;
                             { view.empty() } -> std::same_as<bool>;
                             { view.front() } -> std::same_as<std::span<const std::byte>>;
                         };

    /**
     * @brief The zero-copy stream every layer offers: TCP now, TLS and WebSocket later.
     *
     * Reading is a view and `consume`; writing is `prepare` into the layer's next unit of
     * transmission and `commit`. Events are edges inspected through `events()` and drained by the
     * owning stack's `poll_event()`; `unread()`, `writable()` and `peer_closed()` are levels.
     */
    template <typename S>
    concept IsStream = requires(S& stream, const S& const_stream, std::size_t count) {
        { const_stream.index() } -> std::same_as<std::uint32_t>;
        { const_stream.events() } -> std::same_as<Events>;
        { const_stream.unread() } -> IsReadView;
        { stream.consume(count) } -> std::same_as<void>;
        { const_stream.peer_closed() } -> std::same_as<bool>;
        { const_stream.writable() } -> std::same_as<std::size_t>;
        { stream.prepare(count) } -> std::same_as<std::optional<std::span<std::byte>>>;
        { stream.commit(count) } -> std::same_as<bool>;
        { stream.close() } -> std::same_as<void>;
        { stream.abort() } -> std::same_as<void>;
        { stream.release() } -> std::same_as<void>;
    };

    /**
     * @brief Prepare, copy, commit, per segment, until `bytes` is accepted or the stream stops taking.
     *
     * Returns the prefix accepted; the caller keeps the suffix. Stops at the first null `prepare`
     * or false `commit`, never retries inline, and never merges two calls into one segment. A
     * convenience for a caller that already holds bytes: encoding into `prepare` avoids the copy.
     */
    template <IsStream S>
    std::size_t send(S& stream, const std::span<const std::byte> bytes) noexcept {
        std::size_t accepted = 0;
        while (accepted < bytes.size()) {
            const std::optional<std::span<std::byte>> room = stream.prepare(bytes.size() - accepted);
            if (!room) {
                break;
            }
            if (room->empty()) {
                std::ignore = stream.commit(0);  // nothing to write there: discard the preparation
                break;
            }
            const std::size_t count = room->size();
            std::ranges::copy(bytes.subspan(accepted, count), room->begin());
            if (!stream.commit(count)) {
                break;
            }
            accepted += count;
        }
        return accepted;
    }

}  // namespace aloe::stream
```

`common/stream/export/aloe/stream`:

```cpp
#pragma once

/**
 * Public umbrella for the Aloe stream module: the zero-copy stream contract every protocol
 * layer offers, as concepts, the event and error vocabulary, and the convenience `send`.
 * Names no execution model. Consumers link Aloe::Common::Stream and write `#include <aloe/stream>`.
 */

#include <stream.hpp>
#include <stream_events.hpp>
```

`common/stream/CMakeLists.txt`:

```cmake
set(STREAM ${COMMON}.Stream)

##############################################################################
# The stream contract: concepts, events, errors and the convenience send
##############################################################################
add_library(${STREAM}.Contract INTERFACE
        stream/stream.hpp
        stream/stream_events.hpp
)
target_include_directories(${STREAM}.Contract INTERFACE
        stream/
)
target_link_libraries(${STREAM}.Contract INTERFACE
        Aloe::Common::Core
)
##############################################################################

##############################################################################
# Stream exported library
##############################################################################
add_combined_library(${STREAM}
        DIRECTORIES
        export
        SOURCES
        export/aloe/stream
        LIBRARIES
        ${STREAM}.Contract
)

add_library(Aloe::Common::Stream ALIAS ${STREAM})
##############################################################################
```

In `common/CMakeLists.txt`, reorder to `core`, `execution`, `log`, `wire`, `device`, `ethdev`, `loop`, `net`, `stream`, `runtime` (Task 5 inserts `tcp` between `stream` and `runtime`). `<tuple>` is needed for `std::ignore` in `stream.hpp`; add the include.

- [ ] **Step 4: Run the test to see it pass**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Stream && ctest --preset debug -R '^Aloe[.]Tests[.]Unit[.]Stream$' --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Write `docs/architecture/stream.md`**

Follow the module page pattern (what it is, key types, usage, design notes). The key types are the two concepts, `Event`, `Events`, `Error` and `send`. The usage section shows a decoder loop over `unread()` with `consume`, and an encoder using `prepare`/`commit`. The design notes state the contract from the spec's "The stream concept" section in these words: `unread()` is a view invalidated by `consume` and the next `process`, a chunk's bytes stay put until consumed or released; `prepare`/`commit` write in place, one prepare at a time, a false `commit` means the bytes were not accepted and ownership of them stays with the caller, a `Writable` hint follows; events are edges and state is level, only `poll_event()` and `release()` clear flags; `send(bytes)` copies, reports the accepted prefix, never queues, never blocks and is not POSIX `send(2)`; `close` is half a close, `abort` tears down, `release` is the owner's last call; nothing blocks, allocates, logs or names a clock. Add a short paragraph on how a runtime maps the senders onto the events, to be completed in Task 11.

- [ ] **Step 6: Format and commit**

```bash
./scripts/check-format.sh
git add common/CMakeLists.txt common/stream tests/unit_tests/common/CMakeLists.txt tests/unit_tests/common/stream docs/architecture/stream.md
git commit -m "feat(stream): define the zero-copy stream contract"
```

## Task 3: TCP frame fixtures and the scripted peer

**Files:**
- Create: `fixtures/frames/frames/tcp_frames.hpp`, `fixtures/frames/frames/tcp_peer.hpp`
- Create: `tests/unit_tests/fixtures/frames/CMakeLists.txt`, `tests/unit_tests/fixtures/frames/test_tcp_frames.cpp`
- Modify: `fixtures/frames/frames/net_frames.hpp`, `fixtures/frames/CMakeLists.txt`, `fixtures/frames/export/aloe/frames`, `tests/unit_tests/fixtures/CMakeLists.txt`, `docs/architecture/fixtures.md`

**Interfaces:**
- Consumes: Task 1's formats; `frames::ethernet_frame`, `Checksums`, `ParsedFrame`, `parse_frame`, `l4_checksum_residue`; `wire::Ipv4Header`, `ipv4_header_checksum`, `ipv4_l4_checksum`, `ipv4_pseudo_header_sum`.
- Produces: `frames::TcpSpec`; `std::vector<std::byte> frames::tcp_frame(const TcpSpec&, std::span<const std::byte> payload)`; `ParsedFrame::tcp` (`std::optional<wire::TcpHeader>`); `std::span<const std::byte> frames::tcp_payload(const ParsedFrame&)`; `frames::TcpPeer(TcpSpec, wire::TcpSequence initial)` with `syn()`, `syn_ack(const ParsedFrame&)`, `ack(const ParsedFrame&)`, `ack()`, `data(std::span<const std::byte>)`, `fin()`, `rst()`, `segment(flags, payload, sequence, acknowledgement)`, `void see(const ParsedFrame&)`, and the queries `iss()`, `snd_nxt()`, `rcv_nxt()`, `snd_una()`, `stack_window()`, `stack_mss()`, `spec()`. The peer always describes its own outbound orientation; `see` observes without replying.

- [ ] **Step 1: Write the failing tests**

`tests/unit_tests/fixtures/frames/test_tcp_frames.cpp`:

```cpp
#include <aloe/frames>
#include <aloe/wire>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using aloe::frames::Checksums;
    using aloe::frames::TcpPeer;
    using aloe::frames::TcpSpec;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;
    using aloe::wire::TcpSequence;

    constexpr aloe::wire::MacAddress stack_mac{0x02, 0, 0, 0, 0, 0x01};
    constexpr aloe::wire::MacAddress peer_mac{0x02, 0, 0, 0, 0, 0x02};
    constexpr aloe::wire::Ipv4Address stack_ip{10, 0, 0, 2};
    constexpr aloe::wire::Ipv4Address peer_ip{10, 0, 0, 1};

    [[nodiscard]] TcpSpec peer_to_stack() {
        return {.destination_mac  = stack_mac,
                .source_mac       = peer_mac,
                .source           = peer_ip,
                .destination      = stack_ip,
                .source_port      = 40000,
                .destination_port = 7,
                .mss              = 1460};
    }

    [[nodiscard]] std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
        std::vector<std::byte> out;
        for (const unsigned value : values) {
            out.push_back(std::byte{static_cast<std::uint8_t>(value)});
        }
        return out;
    }

    [[nodiscard]] std::vector<std::byte> text(const char* s) {
        std::vector<std::byte> out;
        for (; *s != '\0'; ++s) {
            out.push_back(static_cast<std::byte>(*s));
        }
        return out;
    }

    /// The stack's SYN-ACK as the peer would see it: sequence 5000, acknowledging the peer's SYN.
    [[nodiscard]] aloe::frames::ParsedFrame stack_syn_ack(const TcpSequence acknowledgement) {
        TcpSpec spec             = peer_to_stack();
        spec.destination_mac     = peer_mac;
        spec.source_mac          = stack_mac;
        spec.source              = stack_ip;
        spec.destination         = peer_ip;
        spec.source_port         = 7;
        spec.destination_port    = 40000;
        spec.sequence            = TcpSequence{5000U};
        spec.acknowledgement     = acknowledgement;
        spec.flags               = TcpFlags{TcpFlag::Syn, TcpFlag::Ack};
        spec.window              = 46720;
        return aloe::frames::parse_frame(aloe::frames::tcp_frame(spec, {})).value();
    }

}  // namespace

TEST(TcpFrames, HeaderOptionsPayloadAndChecksum) {
    TcpSpec spec     = peer_to_stack();
    spec.sequence    = TcpSequence{1000U};
    spec.flags       = TcpFlags{TcpFlag::Syn};
    const auto frame = aloe::frames::tcp_frame(spec, {});
    EXPECT_EQ(frame.size(), 14U + 20U + 24U);
    const auto parsed = aloe::frames::parse_frame(frame).value();
    ASSERT_TRUE(parsed.ipv4.has_value());
    EXPECT_EQ(parsed.ipv4->protocol, aloe::wire::Ipv4Protocol::Tcp);
    EXPECT_EQ(parsed.ipv4->total_length, 44U);
    EXPECT_EQ(aloe::wire::internet_checksum(parsed.ipv4_header), 0U);
    ASSERT_TRUE(parsed.tcp.has_value());
    EXPECT_EQ(parsed.tcp->data_offset, 24U) << "the MSS option";
    EXPECT_EQ(parsed.tcp->flags, TcpFlags{TcpFlag::Syn});
    EXPECT_EQ(parsed.tcp->checksum, 0xe3f4U) << "the vector computed for this exact segment";
    EXPECT_EQ(parsed.l4,
              bytes({0x9c, 0x40, 0x00, 0x07, 0x00, 0x00, 0x03, 0xe8, 0x00, 0x00, 0x00, 0x00,
                     0x60, 0x02, 0xff, 0xff, 0xe3, 0xf4, 0x00, 0x00, 0x02, 0x04, 0x05, 0xb4}));
    EXPECT_EQ(aloe::frames::l4_checksum_residue(*parsed.ipv4, parsed.l4), 0U);
    EXPECT_TRUE(aloe::frames::tcp_payload(parsed).empty());
    const auto options = aloe::wire::TcpOptions::parse(std::span<const std::byte>{parsed.l4}.subspan(20, 4));
    ASSERT_TRUE(options.has_value());
    EXPECT_EQ(options->mss, 1460U);
}

TEST(TcpFrames, PayloadAndKnownChecksum) {
    TcpSpec spec            = peer_to_stack();
    spec.mss                = std::nullopt;
    spec.sequence           = TcpSequence{1001U};
    spec.acknowledgement    = TcpSequence{5001U};
    spec.flags              = TcpFlags{TcpFlag::Psh, TcpFlag::Ack};
    const auto payload      = text("abc");
    const auto parsed       = aloe::frames::parse_frame(aloe::frames::tcp_frame(spec, payload)).value();
    ASSERT_TRUE(parsed.tcp.has_value());
    EXPECT_EQ(parsed.tcp->data_offset, 20U);
    EXPECT_EQ(parsed.tcp->checksum, 0x23abU);
    EXPECT_EQ(aloe::frames::l4_checksum_residue(*parsed.ipv4, parsed.l4), 0U);
    const auto seen = aloe::frames::tcp_payload(parsed);
    EXPECT_EQ(std::vector<std::byte>(seen.begin(), seen.end()), payload);
}

TEST(TcpFrames, ChecksumModes) {
    TcpSpec spec   = peer_to_stack();
    spec.flags     = TcpFlags{TcpFlag::Ack};
    spec.mss       = std::nullopt;
    spec.checksums = Checksums::Zero;
    auto zero      = aloe::frames::parse_frame(aloe::frames::tcp_frame(spec, {})).value();
    EXPECT_EQ(zero.tcp->checksum, 0U);
    EXPECT_NE(aloe::frames::l4_checksum_residue(*zero.ipv4, zero.l4), 0U);
    EXPECT_EQ(zero.ipv4->checksum, 0U);
    spec.checksums = Checksums::Seeded;
    auto seeded    = aloe::frames::parse_frame(aloe::frames::tcp_frame(spec, {})).value();
    EXPECT_EQ(seeded.tcp->checksum,
              aloe::wire::ipv4_pseudo_header_sum(peer_ip, stack_ip, aloe::wire::Ipv4Protocol::Tcp, 20));
    spec.checksums = Checksums::Wrong;
    auto wrong     = aloe::frames::parse_frame(aloe::frames::tcp_frame(spec, {})).value();
    EXPECT_NE(aloe::frames::l4_checksum_residue(*wrong.ipv4, wrong.l4), 0U);
    EXPECT_EQ(aloe::wire::internet_checksum(wrong.ipv4_header), 0U) << "Wrong corrupts only the TCP checksum here";
}

TEST(TcpFrames, UnknownOptionBytesCanBeInsertedByHand) {
    TcpSpec spec  = peer_to_stack();
    spec.flags    = TcpFlags{TcpFlag::Syn};
    auto frame    = aloe::frames::tcp_frame(spec, {});
    // Replace the MSS option with an unknown kind of the same length: the parser skips it, the brick keeps the default.
    frame[14 + 20 + 20] = std::byte{30};
    const auto parsed   = aloe::frames::parse_frame(frame).value();
    ASSERT_TRUE(parsed.tcp.has_value());
    const auto options = aloe::wire::TcpOptions::parse(std::span<const std::byte>{parsed.l4}.subspan(20, 4));
    ASSERT_TRUE(options.has_value());
    EXPECT_FALSE(options->mss.has_value());
}

TEST(Parser, ExcludesEthernetPadding) {
    TcpSpec spec = peer_to_stack();
    spec.flags   = TcpFlags{TcpFlag::Ack};
    spec.mss     = std::nullopt;
    auto frame   = aloe::frames::tcp_frame(spec, {});
    frame.resize(60, std::byte{0xee});  // a card pads to the minimum frame
    const auto parsed = aloe::frames::parse_frame(frame).value();
    ASSERT_TRUE(parsed.tcp.has_value());
    EXPECT_EQ(parsed.l4.size(), 20U);
    EXPECT_TRUE(aloe::frames::tcp_payload(parsed).empty());
    EXPECT_EQ(aloe::frames::l4_checksum_residue(*parsed.ipv4, parsed.l4), 0U);
}

TEST(TcpPeer, TracksSynDataFinSequenceSpace) {
    TcpPeer peer{peer_to_stack(), TcpSequence{1000U}};
    EXPECT_EQ(peer.iss(), TcpSequence{1000U});
    const auto syn = aloe::frames::parse_frame(peer.syn()).value();
    EXPECT_EQ(syn.tcp->sequence, TcpSequence{1000U});
    EXPECT_TRUE(syn.tcp->flags.has(TcpFlag::Syn));
    EXPECT_FALSE(syn.tcp->flags.has(TcpFlag::Ack));
    EXPECT_EQ(syn.tcp->data_offset, 24U);
    EXPECT_EQ(peer.snd_nxt(), TcpSequence{1001U});

    peer.see(stack_syn_ack(TcpSequence{1001U}));
    EXPECT_EQ(peer.rcv_nxt(), TcpSequence{5001U});
    EXPECT_EQ(peer.snd_una(), TcpSequence{1001U});
    EXPECT_EQ(peer.stack_window(), 46720U);
    EXPECT_EQ(peer.stack_mss(), 1460U);

    const auto ack = aloe::frames::parse_frame(peer.ack()).value();
    EXPECT_EQ(ack.tcp->sequence, TcpSequence{1001U});
    EXPECT_EQ(ack.tcp->acknowledgement, TcpSequence{5001U});
    EXPECT_EQ(ack.tcp->flags, TcpFlags{TcpFlag::Ack});
    EXPECT_EQ(ack.tcp->data_offset, 20U) << "no MSS after the SYN";
    EXPECT_EQ(peer.snd_nxt(), TcpSequence{1001U}) << "a pure ACK takes no sequence space";

    const auto data = aloe::frames::parse_frame(peer.data(text("abc"))).value();
    EXPECT_EQ(data.tcp->sequence, TcpSequence{1001U});
    EXPECT_TRUE(data.tcp->flags.has(TcpFlag::Psh));
    EXPECT_EQ(peer.snd_nxt(), TcpSequence{1004U});

    const auto fin = aloe::frames::parse_frame(peer.fin()).value();
    EXPECT_EQ(fin.tcp->sequence, TcpSequence{1004U});
    EXPECT_EQ(fin.tcp->flags, (TcpFlags{TcpFlag::Fin, TcpFlag::Ack}));
    EXPECT_EQ(peer.snd_nxt(), TcpSequence{1005U});

    const auto rst = aloe::frames::parse_frame(peer.rst()).value();
    EXPECT_TRUE(rst.tcp->flags.has(TcpFlag::Rst));
    EXPECT_EQ(rst.tcp->sequence, TcpSequence{1005U});
}

TEST(TcpPeer, SynAckAdoptsThePortsOfTheSynItAnswers) {
    TcpPeer peer{peer_to_stack(), TcpSequence{1000U}};
    // The stack opened from port 33000 to the peer's 7: the peer answers from 7 to 33000.
    TcpSpec stack_spec         = peer_to_stack();
    stack_spec.destination_mac = peer_mac;
    stack_spec.source_mac      = stack_mac;
    stack_spec.source          = stack_ip;
    stack_spec.destination     = peer_ip;
    stack_spec.source_port     = 33000;
    stack_spec.destination_port = 7;
    stack_spec.sequence        = TcpSequence{9000U};
    stack_spec.flags           = TcpFlags{TcpFlag::Syn};
    const auto seen            = aloe::frames::parse_frame(aloe::frames::tcp_frame(stack_spec, {})).value();
    const auto reply           = aloe::frames::parse_frame(peer.syn_ack(seen)).value();
    EXPECT_EQ(reply.tcp->source_port, 7U);
    EXPECT_EQ(reply.tcp->destination_port, 33000U);
    EXPECT_EQ(reply.tcp->acknowledgement, TcpSequence{9001U});
    EXPECT_EQ(reply.tcp->flags, (TcpFlags{TcpFlag::Syn, TcpFlag::Ack}));
    EXPECT_EQ(peer.snd_nxt(), TcpSequence{1001U});
    EXPECT_EQ(peer.spec().source_port, 7U);
}

TEST(TcpPeer, SeeNeverRetreats) {
    TcpPeer peer{peer_to_stack(), TcpSequence{1000U}};
    peer.see(stack_syn_ack(TcpSequence{1001U}));
    peer.see(stack_syn_ack(TcpSequence{1001U}));  // a retransmitted SYN-ACK
    EXPECT_EQ(peer.rcv_nxt(), TcpSequence{5001U});
}
```

`tests/unit_tests/fixtures/frames/CMakeLists.txt`:

```cmake
##############################################################################
# Test Frames: the TCP builder, the parser field and the scripted peer
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Frames
        test_tcp_frames.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Frames
        PRIVATE
        Aloe::Fixtures::Frames
        ${TEST_LIBS}
)
##############################################################################
```

Add `add_subdirectory(frames)` to `tests/unit_tests/fixtures/CMakeLists.txt`.

- [ ] **Step 2: Run the test to see it fail**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Frames`
Expected: compile errors, `no member named 'TcpSpec' in namespace 'aloe::frames'`.

- [ ] **Step 3: Write the builder, extend the parser, write the peer**

`fixtures/frames/frames/tcp_frames.hpp`:

```cpp
#pragma once

#include <algorithm>
#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <frames.hpp>
#include <net_frames.hpp>

/**
 * A TCP segment over IPv4 over Ethernet, built the way a peer would send it, and the payload of a
 * parsed one. Fixture allocation is fine here; nothing in `common` links this.
 */
namespace aloe::frames {

    struct TcpSpec {
        wire::MacAddress destination_mac{};
        wire::MacAddress source_mac{};
        wire::Ipv4Address source{};
        wire::Ipv4Address destination{};
        std::uint16_t source_port      = 0;
        std::uint16_t destination_port = 0;
        wire::TcpSequence sequence{};
        wire::TcpSequence acknowledgement{};
        wire::TcpFlags flags{};
        std::uint16_t window = 65535;
        std::optional<std::uint16_t> mss;  ///< Written as the one option when present; set it on SYNs.
        Checksums checksums = Checksums::Correct;  ///< `Wrong` corrupts the TCP checksum only; the IPv4 header stays right.
    };

    /// The segment's bytes after the IPv4 header: header, the MSS option when given, then `payload`.
    [[nodiscard]] inline std::vector<std::byte> tcp_segment(const TcpSpec& spec, const std::span<const std::byte> payload) {
        const std::size_t header = wire::TcpHeader::size + (spec.mss ? wire::TcpOptions::mss_size : 0);
        std::vector<std::byte> segment(header + payload.size());
        wire::TcpHeader{.source_port      = spec.source_port,
                        .destination_port = spec.destination_port,
                        .sequence         = spec.sequence,
                        .acknowledgement  = spec.acknowledgement,
                        .data_offset      = static_cast<std::uint8_t>(header),
                        .flags            = spec.flags,
                        .window           = spec.window}
            .write(segment);
        if (spec.mss) {
            wire::TcpOptions::write_mss(std::span<std::byte>{segment}.subspan(wire::TcpHeader::size), *spec.mss);
        }
        std::ranges::copy(payload, segment.begin() + static_cast<std::ptrdiff_t>(header));
        if (spec.checksums != Checksums::Zero) {
            const std::uint16_t value =
                spec.checksums == Checksums::Seeded
                    ? wire::ipv4_pseudo_header_sum(
                          spec.source, spec.destination, wire::Ipv4Protocol::Tcp, static_cast<std::uint16_t>(segment.size()))
                    : wire::ipv4_l4_checksum(spec.source, spec.destination, wire::Ipv4Protocol::Tcp, segment);
            wire::store_be16(std::span<std::byte>{segment}.subspan(wire::TcpHeader::checksum_offset, 2), value);
            if (spec.checksums == Checksums::Wrong) {
                segment[wire::TcpHeader::checksum_offset] ^= std::byte{0x01};
            }
        }
        return segment;
    }

    /// A TCP segment over IPv4 over Ethernet, as a peer puts it on the wire.
    [[nodiscard]] inline std::vector<std::byte> tcp_frame(const TcpSpec& spec, const std::span<const std::byte> payload) {
        const std::vector<std::byte> segment = tcp_segment(spec, payload);
        std::vector<std::byte> ip(wire::Ipv4Header::size + segment.size());
        const std::span<std::byte> header = std::span<std::byte>{ip}.first(wire::Ipv4Header::size);
        wire::Ipv4Header{.total_length   = static_cast<std::uint16_t>(ip.size()),
                         .identification = 0x0102,
                         .ttl            = 64,
                         .protocol       = wire::Ipv4Protocol::Tcp,
                         .source         = spec.source,
                         .destination    = spec.destination}
            .write(header);
        if (spec.checksums == Checksums::Correct || spec.checksums == Checksums::Wrong) {
            // Wrong corrupts the TCP checksum only: the IPv4 header stays right, so the segment reaches TCP.
            wire::store_be16(header.subspan(wire::ipv4_checksum_offset, 2), wire::ipv4_header_checksum(header));
        }
        std::ranges::copy(segment, ip.begin() + static_cast<std::ptrdiff_t>(wire::Ipv4Header::size));
        return ethernet_frame(spec.destination_mac, spec.source_mac, ethertype_ipv4, ip);
    }

    /// The bytes after the TCP header of a parsed frame; empty when the frame is not TCP.
    [[nodiscard]] inline std::span<const std::byte> tcp_payload(const ParsedFrame& frame) {
        if (!frame.tcp) {
            return {};
        }
        return std::span<const std::byte>{frame.l4}.subspan(frame.tcp->data_offset);
    }

}  // namespace aloe::frames
```

In `fixtures/frames/frames/net_frames.hpp`, add `std::optional<wire::TcpHeader> tcp;` to `ParsedFrame` after `icmp`, and in `parse_frame` after the ICMP branch:

```cpp
        if (parsed.ipv4->protocol == wire::Ipv4Protocol::Tcp) {
            parsed.tcp = wire::TcpHeader::parse(l4);
        }
```

`fixtures/frames/frames/tcp_peer.hpp`:

```cpp
#pragma once

#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <net_frames.hpp>
#include <tcp_frames.hpp>

namespace aloe::frames {

    /**
     * @brief The other end of a connection, scripted by the test: builds each segment with the
     * right numbers from what it has seen, and never replies on its own.
     *
     * `spec` holds the peer's outbound orientation: its MACs, addresses and ports as the source,
     * the stack's as the destination. `see` is fed every frame the stack transmits and advances
     * `rcv_nxt`, `snd_una`, the stack's window and MSS. Every other member returns a frame.
     */
    class TcpPeer {
    public:
        TcpPeer(TcpSpec spec, const wire::TcpSequence initial) noexcept
            : spec_{std::move(spec)},
              iss_{initial},
              snd_nxt_{initial} {
        }

        /// SYN with the spec's MSS; takes one sequence number.
        [[nodiscard]] std::vector<std::byte> syn() {
            const std::vector<std::byte> frame =
                segment(wire::TcpFlags{wire::TcpFlag::Syn}, {}, snd_nxt_, wire::TcpSequence{}, true);
            snd_nxt_ = snd_nxt_ + 1;
            return frame;
        }

        /// Answers the stack's SYN: `see` adopts its ports, then SYN-ACK acknowledges it and takes one sequence number.
        [[nodiscard]] std::vector<std::byte> syn_ack(const ParsedFrame& seen) {
            see(seen);
            const std::vector<std::byte> frame =
                segment(wire::TcpFlags{wire::TcpFlag::Syn, wire::TcpFlag::Ack}, {}, snd_nxt_, rcv_nxt_, true);
            snd_nxt_ = snd_nxt_ + 1;
            return frame;
        }

        /// Sees `seen`, then a pure ACK of everything seen so far.
        [[nodiscard]] std::vector<std::byte> ack(const ParsedFrame& seen) {
            see(seen);
            return ack();
        }

        [[nodiscard]] std::vector<std::byte> ack() {
            return segment(wire::TcpFlags{wire::TcpFlag::Ack}, {}, snd_nxt_, rcv_nxt_, false);
        }

        /// PSH+ACK carrying `bytes`; takes their sequence space.
        [[nodiscard]] std::vector<std::byte> data(const std::span<const std::byte> bytes) {
            const std::vector<std::byte> frame =
                segment(wire::TcpFlags{wire::TcpFlag::Psh, wire::TcpFlag::Ack}, bytes, snd_nxt_, rcv_nxt_, false);
            snd_nxt_ = snd_nxt_ + static_cast<std::uint32_t>(bytes.size());
            return frame;
        }

        /// FIN+ACK; takes one sequence number.
        [[nodiscard]] std::vector<std::byte> fin() {
            const std::vector<std::byte> frame =
                segment(wire::TcpFlags{wire::TcpFlag::Fin, wire::TcpFlag::Ack}, {}, snd_nxt_, rcv_nxt_, false);
            snd_nxt_ = snd_nxt_ + 1;
            return frame;
        }

        /// RST+ACK at the current numbers.
        [[nodiscard]] std::vector<std::byte> rst() {
            return segment(wire::TcpFlags{wire::TcpFlag::Rst, wire::TcpFlag::Ack}, {}, snd_nxt_, rcv_nxt_, false);
        }

        /// Any segment at explicit numbers, with no change to the peer's state: for duplicates and gaps.
        [[nodiscard]] std::vector<std::byte> segment(const wire::TcpFlags flags,
                                                     const std::span<const std::byte> payload,
                                                     const wire::TcpSequence sequence,
                                                     const wire::TcpSequence acknowledgement,
                                                     const bool with_mss = false) const {
            TcpSpec spec        = spec_;
            spec.sequence       = sequence;
            spec.acknowledgement = acknowledgement;
            spec.flags          = flags;
            if (!with_mss) {
                spec.mss = std::nullopt;
            }
            return tcp_frame(spec, payload);
        }

        /// Observes a frame the stack transmitted. Numbers never retreat, so a retransmission changes nothing;
        /// a SYN without ACK is an active open by the stack, and the peer adopts its ports to answer it.
        void see(const ParsedFrame& frame) noexcept {
            if (!frame.tcp) {
                return;
            }
            const wire::TcpHeader& tcp = *frame.tcp;
            std::uint32_t length       = static_cast<std::uint32_t>(frame.l4.size() - tcp.data_offset);
            if (tcp.flags.has(wire::TcpFlag::Syn)) {
                ++length;
                if (!tcp.flags.has(wire::TcpFlag::Ack)) {  // the stack opened: the peer answers from the SYN's destination port
                    spec_.source_port      = tcp.destination_port;
                    spec_.destination_port = tcp.source_port;
                }
                if (const auto options = wire::TcpOptions::parse(
                        std::span<const std::byte>{frame.l4}.subspan(wire::TcpHeader::size,
                                                                     tcp.data_offset - wire::TcpHeader::size))) {
                    stack_mss_ = options->mss;
                }
                synchronised_ = true;
                rcv_nxt_      = tcp.sequence + length;
            } else if (tcp.flags.has(wire::TcpFlag::Fin)) {
                ++length;
            }
            const wire::TcpSequence end = tcp.sequence + length;
            if (synchronised_ && end.after(rcv_nxt_)) {
                rcv_nxt_ = end;
            }
            if (tcp.flags.has(wire::TcpFlag::Ack) && tcp.acknowledgement.after(snd_una_)) {
                snd_una_ = tcp.acknowledgement;
            }
            stack_window_ = tcp.window;
        }

        [[nodiscard]] wire::TcpSequence iss() const noexcept { return iss_; }
        [[nodiscard]] wire::TcpSequence snd_nxt() const noexcept { return snd_nxt_; }
        [[nodiscard]] wire::TcpSequence rcv_nxt() const noexcept { return rcv_nxt_; }
        [[nodiscard]] wire::TcpSequence snd_una() const noexcept { return snd_una_; }
        [[nodiscard]] std::uint16_t stack_window() const noexcept { return stack_window_; }
        [[nodiscard]] std::optional<std::uint16_t> stack_mss() const noexcept { return stack_mss_; }
        [[nodiscard]] TcpSpec& spec() noexcept { return spec_; }
        [[nodiscard]] const TcpSpec& spec() const noexcept { return spec_; }

        /// Rewinds the peer's own sequence by `count` bytes, for scripting a retransmission.
        void rewind(const std::uint32_t count) noexcept { snd_nxt_ = wire::TcpSequence{snd_nxt_.value - count}; }

    private:
        TcpSpec spec_;
        wire::TcpSequence iss_;
        wire::TcpSequence snd_nxt_;
        wire::TcpSequence rcv_nxt_{};
        wire::TcpSequence snd_una_{};
        std::uint16_t stack_window_ = 0;
        std::optional<std::uint16_t> stack_mss_;
        bool synchronised_ = false;
    };

}  // namespace aloe::frames
```

Add `frames/tcp_frames.hpp` and `frames/tcp_peer.hpp` to `${FRAMES}.Builders` in `fixtures/frames/CMakeLists.txt`, and to the umbrella `fixtures/frames/export/aloe/frames`:

```cpp
#include <tcp_frames.hpp>
#include <tcp_peer.hpp>
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Frames Aloe.Tests.Unit.Wire Aloe.Tests.Unit.Net && ctest --preset debug -R '^Aloe[.]Tests[.]Unit[.](Frames|Wire|Net)$' --output-on-failure`
Expected: PASS; the net tests still pass with the extra parser field.

- [ ] **Step 5: Document and format**

In `docs/architecture/fixtures.md`, add `TcpSpec`, `tcp_frame`, `tcp_segment`, `tcp_payload`, `ParsedFrame::tcp` and `TcpPeer` to the key types, and a usage paragraph: a test reads like a packetdrill script, one segment at a time, feeding `see` what the stack transmits and injecting what the peer sends; the peer never replies on its own. Run `./scripts/check-format.sh`.

- [ ] **Step 6: Commit**

```bash
git add fixtures/frames tests/unit_tests/fixtures docs/architecture/fixtures.md
git commit -m "feat(frames): add TCP frames and a scripted peer"
```

## Task 4: Expose the IP seams TCP needs

**Files:**
- Modify: `common/net/stack/ipv4.hpp`, `tests/unit_tests/common/net/test_net_send.cpp`, `docs/architecture/net.md`

**Interfaces:**
- Produces: `net::Ipv4<DeviceT>::Device` (alias of the template parameter), `loop::ShardQueue<Device>& queue() noexcept`, `const loop::ShardQueue<Device>& queue() const noexcept`, `std::optional<wire::Ipv4Address> next_hop(wire::Ipv4Address destination) const noexcept`. For a unicast destination the result is the destination itself on the subnet, the configured gateway off it, or nothing with no gateway. The query sends nothing and touches no cache. `send` uses the same decision.

- [ ] **Step 1: Write the failing tests**

Add to `tests/unit_tests/common/net/test_net_send.cpp`, inside the anonymous namespace after the fixture:

```cpp
    static_assert(std::same_as<aloe::net::Ipv4<aloe::fabric::Port>::Device, aloe::fabric::Port>);
```

and after the existing tests:

```cpp
TEST_P(NetSend, QueueIdentityAndDeviceAlias) {
    EXPECT_EQ(&ip_.queue(), &queue_);
    const Ipv4& read_only = ip_;
    EXPECT_EQ(&read_only.queue(), &queue_);
    EXPECT_EQ(ip_.queue().index(), 0U);
}

TEST_P(NetSend, RouteQueryHasNoSideEffects) {
    EXPECT_EQ(ip_.next_hop(harness_ip), harness_ip) << "on the subnet: the destination itself";
    EXPECT_EQ(ip_.next_hop(far_ip), gateway_ip) << "off the subnet: the gateway";
    const Ipv4 without_gateway{queue_, {.address = stack_ip, .prefix = 24}};
    EXPECT_FALSE(without_gateway.next_hop(far_ip).has_value());
    EXPECT_EQ(without_gateway.next_hop(harness_ip), harness_ip);
    EXPECT_EQ(queue_.pending(), 0U) << "querying is not resolving: no ARP request was queued";
    EXPECT_EQ(ip_.counters().arp_requests_sent, 0U);
    EXPECT_EQ(ip_.counters().send_no_route, 0U);
}
```

`Ipv4` is the fixture's alias for `aloe::net::Ipv4<aloe::fabric::Port>`; `<concepts>` is needed for `std::same_as`.

- [ ] **Step 2: Run the test to see it fail**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Net`
Expected: compile errors, `no type named 'Device'` and `no member named 'queue'`.

- [ ] **Step 3: Make the change**

In `common/net/stack/ipv4.hpp`, rename the template parameter and add the alias:

```cpp
    template <device::IsDevice DeviceT>
    class Ipv4 {
    public:
        using Device = DeviceT;
        using Packet = typename Device::Packet;
```

Replace every other `Device` in the class body that referred to the parameter with `Device` (the alias has the same spelling, so the constructor's `loop::ShardQueue<Device>& owner` and the `queue_` member need no edit; check the out-of-class helpers at the bottom of the file).

Add after `learn`:

```cpp
        /// The shard queue the brick was built over: a transport reads its index, steering, ring and device there.
        [[nodiscard]] loop::ShardQueue<Device>& queue() noexcept {
            return *queue_;
        }

        [[nodiscard]] const loop::ShardQueue<Device>& queue() const noexcept {
            return *queue_;
        }

        /**
         * @brief Where a unicast datagram to `destination` would be sent: the destination itself on the
         * subnet, the gateway off it, or nothing when there is none.
         *
         * A route question with no side effect: no ARP lookup, no request, no counter. `send` decides
         * the same way. Broadcast and multicast destinations are not routed and answer with themselves.
         */
        [[nodiscard]] std::optional<wire::Ipv4Address> next_hop(const wire::Ipv4Address destination) const noexcept {
            if (is_broadcast(destination) || destination.is_multicast() || subnet_.contains(destination)) {
                return destination;
            }
            return config_.gateway;
        }
```

In `send`, replace the unicast next-hop block with the query:

```cpp
            } else {
                const std::optional<wire::Ipv4Address> hop = next_hop(request.destination);
                if (!hop) {
                    ++counters_.send_no_route;
                    return std::unexpected{SendError::NoRoute};
                }
                const std::optional<wire::MacAddress> mac = resolve(*hop, now);
                if (!mac) {
                    ++counters_.send_unresolved;
                    return std::unexpected{SendError::Unresolved};
                }
                next_mac = *mac;
            }
```

`is_broadcast` and `subnet_` already exist in the class; `next_hop` must be declared after them or they must be visible, which inside a class body they are.

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Net Aloe.Tests.Integration.Net.Ring && ctest --preset debug -R '^Aloe[.]Tests[.](Unit[.]Net|Integration[.]Net[.]Ring)$' --output-on-failure`
Expected: PASS; every existing send test still passes, including the no-route and unresolved cases.

- [ ] **Step 5: Document and format**

In `docs/architecture/net.md`, under "Key types", add a "Transport access" paragraph: `Device`, `queue()`, `next_hop()`, and that a transport asks `next_hop` before committing resources so a connect to an unroutable peer takes no slot. Run `./scripts/check-format.sh`.

- [ ] **Step 6: Commit**

```bash
git add common/net/stack/ipv4.hpp tests/unit_tests/common/net/test_net_send.cpp docs/architecture/net.md
git commit -m "feat(net): expose queue and route information to transports"
```

## Task 5: Fixed flow table and retained-packet storage

**Files:**
- Create: `common/tcp/CMakeLists.txt`, `common/tcp/export/aloe/tcp`, `common/tcp/state/tcp_config.hpp`, `common/tcp/state/tcp_counters.hpp`, `common/tcp/table/flow_table.hpp`, `common/tcp/table/flow_table.cpp`, `common/tcp/buffer/tcp_node_pool.hpp`, `common/tcp/buffer/read_view.hpp`
- Create: `tests/unit_tests/common/tcp/CMakeLists.txt`, `tests/unit_tests/common/tcp/test_tcp_table.cpp`, `tests/unit_tests/common/tcp/test_tcp_buffers.cpp`
- Modify: `common/CMakeLists.txt`, `tests/unit_tests/common/CMakeLists.txt`, `common/net/arp/arp_cache.hpp`

**Interfaces:**
- Consumes: `wire::Ipv4Address`, `device::IsPacket`, `stream::IsReadView`, `core::Duration`.
- Produces: `tcp::Endpoint`, `tcp::State`, `tcp::ConnectError`, `tcp::ListenError`, `tcp::TcpConfig`, `tcp::default_peer_mss`; `tcp::TcpCounters`; `tcp::FlowKey{remote, remote_port, local_port}`; `tcp::detail::mix_flow_hash(std::uint32_t)`, `tcp::detail::software_flow_hash(FlowKey)`; `tcp::FlowTable(std::size_t connections)` with `find(key, base_hash) -> std::optional<std::uint32_t>`, `insert(key, index, base_hash) -> bool`, `erase(key, base_hash) -> bool`, `size()`, `capacity()`; `tcp::detail::no_node`; `tcp::detail::TcpNodePool<Packet>(std::size_t)` with `Node{packet, offset, length, next}`, `acquire(Packet&&, offset, length) -> std::optional<std::uint32_t>` (moves only on success), `release(index)`, `node(index)` in both constnesses, `available()`, `capacity()`; `tcp::ReadView<Packet>(TcpNodePool<Packet>&, head, bytes)` modelling `stream::IsReadView`.

- [ ] **Step 1: Write the failing tests**

`tests/unit_tests/common/tcp/test_tcp_table.cpp`:

```cpp
#include <aloe/device>
#include <aloe/tcp>
#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using aloe::tcp::FlowKey;
    using aloe::tcp::FlowTable;
    using aloe::tcp::detail::mix_flow_hash;
    using aloe::tcp::detail::software_flow_hash;

    constexpr aloe::wire::Ipv4Address peer{10, 0, 0, 1};
    constexpr aloe::wire::Ipv4Address other{10, 0, 0, 2};

    [[nodiscard]] FlowKey key(const std::uint16_t remote_port, const std::uint16_t local_port = 7) {
        return {.remote = peer, .remote_port = remote_port, .local_port = local_port};
    }

}  // namespace

TEST(TcpTable, CapacityIsTheNextPowerOfTwoAtLeastTwiceTheConnections) {
    EXPECT_EQ(FlowTable{4}.capacity(), 8U);
    EXPECT_EQ(FlowTable{5}.capacity(), 16U);
    EXPECT_EQ(FlowTable{1024}.capacity(), 2048U);
    EXPECT_EQ(FlowTable{1}.capacity(), 2U);
    EXPECT_THROW((std::ignore = FlowTable{0}), std::invalid_argument);
    EXPECT_THROW((std::ignore = FlowTable{std::size_t{1} << 31U}), std::invalid_argument);
}

TEST(TcpTable, FixedVectorsForTheMixerAndTheSoftwareHash) {
    EXPECT_EQ(mix_flow_hash(0U), 0U);
    EXPECT_EQ(mix_flow_hash(1U), 0x514e28b7U);
    EXPECT_EQ(mix_flow_hash(0xdeadbeefU), 0x0de5c6a9U);
    EXPECT_EQ(mix_flow_hash(0x80000000U), 0x6d3c65a0U);
    EXPECT_EQ(software_flow_hash(key(40000)), 0x12854375U);
    EXPECT_EQ(software_flow_hash(key(40001)), 0x4f6bf39aU);
    EXPECT_EQ(software_flow_hash({.remote = other, .remote_port = 40000, .local_port = 7}), 0x43a64bdcU);
    EXPECT_EQ(software_flow_hash({}), 0x9be17165U);
    EXPECT_NE(software_flow_hash(key(40000, 7)), software_flow_hash(key(7, 40000))) << "ports are not symmetric";
}

// Base hashes 0x1, 0x3 and 0x12 have home bucket 7 of 8 after mixing; 0x6 has home 0. The chain from 7
// wraps to 0 and 1, and the entry homed at 0 is pushed to 2. Deleting the head shifts the three behind it.
TEST(TcpTable, BackwardShiftAcrossWrap) {
    FlowTable table{4};
    ASSERT_EQ(table.capacity(), 8U);
    ASSERT_TRUE(table.insert(key(1), 10, 0x1));
    ASSERT_TRUE(table.insert(key(2), 11, 0x3));
    ASSERT_TRUE(table.insert(key(3), 12, 0x12));
    ASSERT_TRUE(table.insert(key(4), 13, 0x6));
    EXPECT_EQ(table.size(), 4U);
    EXPECT_EQ(table.find(key(1), 0x1), 10U);
    EXPECT_EQ(table.find(key(4), 0x6), 13U);

    EXPECT_TRUE(table.erase(key(1), 0x1));  // the head of the chain
    EXPECT_FALSE(table.find(key(1), 0x1));
    EXPECT_EQ(table.find(key(2), 0x3), 11U);
    EXPECT_EQ(table.find(key(3), 0x12), 12U);
    EXPECT_EQ(table.find(key(4), 0x6), 13U);

    EXPECT_TRUE(table.erase(key(3), 0x12));  // the middle
    EXPECT_EQ(table.find(key(2), 0x3), 11U);
    EXPECT_EQ(table.find(key(4), 0x6), 13U);
    EXPECT_FALSE(table.find(key(3), 0x12));

    ASSERT_TRUE(table.insert(key(5), 14, 0x6)) << "insert after deletion reuses the shifted holes";
    EXPECT_EQ(table.find(key(5), 0x6), 14U);
    EXPECT_TRUE(table.erase(key(5), 0x6));  // the tail
    EXPECT_TRUE(table.erase(key(4), 0x6));
    EXPECT_TRUE(table.erase(key(2), 0x3));
    EXPECT_EQ(table.size(), 0U);
    EXPECT_FALSE(table.erase(key(2), 0x3));
}

TEST(TcpTable, DuplicateAndFullTable) {
    FlowTable table{1};
    ASSERT_EQ(table.capacity(), 2U);
    EXPECT_TRUE(table.insert(key(1), 0, 0x1));
    EXPECT_FALSE(table.insert(key(1), 5, 0x1)) << "a duplicate key is refused";
    EXPECT_EQ(table.find(key(1), 0x1), 0U);
    EXPECT_TRUE(table.insert(key(2), 1, 0x1)) << "a second key with the same hash probes to the next slot";
    EXPECT_FALSE(table.insert(key(3), 2, 0x1)) << "full: two entries in two slots";
    EXPECT_EQ(table.size(), 2U);
    EXPECT_FALSE(table.find(key(3), 0x1));
    EXPECT_TRUE(table.erase(key(1), 0x1));
    EXPECT_EQ(table.find(key(2), 0x1), 1U);
}

TEST(TcpTable, FieldEqualityNotHashEquality) {
    FlowTable table{4};
    ASSERT_TRUE(table.insert(key(40000, 7), 1, 0x1));
    EXPECT_FALSE(table.find(key(40000, 8), 0x1)) << "same hash, different local port";
    EXPECT_FALSE(table.find({.remote = other, .remote_port = 40000, .local_port = 7}, 0x1));
    EXPECT_FALSE(table.find(key(40000, 7), 0x2)) << "the same key under another base hash is a different bucket";
}

TEST(TcpTable, HardwareSoftwareHashAgreement) {
    // Received orientation: the peer is the source, we are the destination; `connect` hashes the same tuple.
    const aloe::device::RssDescription rss = aloe::device::round_robin_rss(4);
    const aloe::device::FlowTuple received{.source           = peer,
                                           .destination      = other,
                                           .source_port      = 40000,
                                           .destination_port = 33000,
                                           .protocol         = aloe::wire::Ipv4Protocol::Tcp};
    const std::uint32_t hardware = aloe::device::flow_hash(rss, received);
    const FlowKey logical{.remote = peer, .remote_port = 40000, .local_port = 33000};
    FlowTable table{16};
    ASSERT_TRUE(table.insert(logical, 3, hardware));
    EXPECT_EQ(table.find(logical, aloe::device::flow_hash(rss, received)), 3U) << "a packet without rss_hash recomputes it";
    EXPECT_TRUE(table.erase(logical, hardware));
    ASSERT_TRUE(table.insert(logical, 4, software_flow_hash(logical)));
    EXPECT_EQ(table.find(logical, software_flow_hash(logical)), 4U);
    EXPECT_FALSE(table.find(logical, hardware)) << "one base hash function per stack, chosen at construction";
}

TEST(TcpTable, ShardConditionedHashes) {
    // With 16 queues and a round-robin table every hash reaching one shard shares its low four bits.
    FlowTable table{1024};
    ASSERT_EQ(table.capacity(), 2048U);
    std::set<std::uint32_t> homes;
    std::set<std::uint32_t> unmixed;
    for (std::uint32_t flow = 0; flow < 64; ++flow) {
        const std::uint32_t base = (flow << 4U) | 0x5U;
        homes.insert(mix_flow_hash(base) & 2047U);
        unmixed.insert(base & 2047U & 0xfU);
        ASSERT_TRUE(table.insert(key(static_cast<std::uint16_t>(40000 + flow)), flow, base));
    }
    EXPECT_EQ(homes.size(), 64U) << "mixing spreads the conditioned hashes over distinct buckets";
    EXPECT_EQ(unmixed.size(), 1U) << "without mixing they would all share one low nibble";
    for (std::uint32_t flow = 0; flow < 64; ++flow) {
        const std::uint32_t base = (flow << 4U) | 0x5U;
        EXPECT_EQ(table.find(key(static_cast<std::uint16_t>(40000 + flow)), base), flow);
    }
    for (std::uint32_t flow = 0; flow < 64; flow += 2) {
        EXPECT_TRUE(table.erase(key(static_cast<std::uint16_t>(40000 + flow)), (flow << 4U) | 0x5U));
    }
    for (std::uint32_t flow = 1; flow < 64; flow += 2) {
        EXPECT_EQ(table.find(key(static_cast<std::uint16_t>(40000 + flow)), (flow << 4U) | 0x5U), flow);
    }
    // Four queues: the low two bits are fixed; the same property holds.
    std::set<std::uint32_t> homes4;
    for (std::uint32_t flow = 0; flow < 64; ++flow) {
        homes4.insert(mix_flow_hash((flow << 2U) | 0x1U) & 2047U);
    }
    EXPECT_GE(homes4.size(), 60U);
}
```

`tests/unit_tests/common/tcp/test_tcp_buffers.cpp`:

```cpp
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/stream>
#include <aloe/tcp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using Packet = aloe::fabric::Packet;
    using Pool   = aloe::tcp::detail::TcpNodePool<Packet>;
    using View   = aloe::tcp::ReadView<Packet>;

    static_assert(aloe::stream::IsReadView<View>);
    static_assert(std::ranges::forward_range<View>);

    class TcpBuffers : public ::testing::Test {
    protected:
        aloe::fabric::Fabric fabric_;
        aloe::fabric::Port& port_ = fabric_.add_port({.mac = {0x02, 0, 0, 0, 0, 1}, .pool_size = 8});

        /// A packet holding `length` pattern bytes after `offset` bytes of header.
        [[nodiscard]] Packet packet(const std::uint16_t offset, const std::uint16_t length) {
            auto out = port_.allocate(0);
            EXPECT_TRUE(out.has_value());
            EXPECT_TRUE(aloe::frames::fill(*out, aloe::frames::pattern(offset + length, offset)));
            return std::move(*out);
        }
    };

}  // namespace

TEST_F(TcpBuffers, RetainsPacketsAndIteratesSpans) {
    Pool pool{4};
    EXPECT_EQ(pool.capacity(), 4U);
    EXPECT_EQ(pool.available(), 4U);
    const auto a = pool.acquire(packet(54, 3), 54, 3);
    const auto b = pool.acquire(packet(54, 5), 54, 5);
    const auto c = pool.acquire(packet(60, 2), 60, 2);
    ASSERT_TRUE(a && b && c);
    EXPECT_EQ(pool.available(), 1U);
    pool.node(*a).next = *b;
    pool.node(*b).next = *c;
    const View view{pool, *a, 10};
    EXPECT_EQ(view.size(), 10U);
    EXPECT_FALSE(view.empty());
    EXPECT_EQ(view.front().size(), 3U);
    std::vector<std::size_t> sizes;
    std::vector<const std::byte*> starts;
    for (const std::span<const std::byte> chunk : view) {
        sizes.push_back(chunk.size());
        starts.push_back(chunk.data());
    }
    EXPECT_EQ(sizes, (std::vector<std::size_t>{3, 5, 2}));
    EXPECT_EQ(starts[0], pool.node(*a).packet.data().data() + 54) << "a span into the packet, not a copy";
    EXPECT_EQ(starts[1], pool.node(*b).packet.data().data() + 54);
    EXPECT_EQ(starts[2], pool.node(*c).packet.data().data() + 60);
    EXPECT_EQ(std::to_integer<int>(view.front()[0]), 54) << "the pattern starts at the offset";
    EXPECT_EQ(std::ranges::distance(view), 3);
    auto it = view.begin();
    ++it;
    EXPECT_EQ((*it).size(), 5U);
    EXPECT_EQ((*it++).size(), 5U);
    EXPECT_EQ((*it).size(), 2U);
    ++it;
    EXPECT_EQ(it, view.end());
}

TEST_F(TcpBuffers, PartialFrontOffsetAndEmptyView) {
    Pool pool{2};
    const auto a = pool.acquire(packet(54, 6), 54, 6);
    ASSERT_TRUE(a);
    pool.node(*a).offset += 4;  // four bytes consumed
    pool.node(*a).length -= 4;
    const View view{pool, *a, 2};
    EXPECT_EQ(view.size(), 2U);
    EXPECT_EQ(view.front().size(), 2U);
    EXPECT_EQ(std::to_integer<int>(view.front()[0]), 58);
    const View empty;
    EXPECT_TRUE(empty.empty());
    EXPECT_EQ(empty.size(), 0U);
    EXPECT_EQ(empty.begin(), empty.end());
    const View none{pool, aloe::tcp::detail::no_node, 0};
    EXPECT_TRUE(none.empty());
}

TEST_F(TcpBuffers, ExhaustionKeepsThePacketAndReleaseRestoresTheCount) {
    Pool pool{1};
    const auto a = pool.acquire(packet(54, 1), 54, 1);
    ASSERT_TRUE(a);
    EXPECT_EQ(pool.available(), 0U);
    Packet kept    = packet(54, 1);
    const auto* at = kept.data().data();
    const auto b   = pool.acquire(std::move(kept), 54, 1);
    EXPECT_FALSE(b);
    EXPECT_FALSE(kept.empty()) << "a failed acquire leaves the packet with the caller";  // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(kept.data().data(), at);                                                   // NOLINT(bugprone-use-after-move)
    pool.release(*a);
    EXPECT_EQ(pool.available(), 1U);
    EXPECT_TRUE(pool.node(*a).packet.empty()) << "release returns the packet to its pool";
    EXPECT_EQ(port_.counters(0).received, 0U);
    const auto c = pool.acquire(std::move(kept), 54, 1);
    EXPECT_TRUE(c);
    EXPECT_EQ(pool.available(), 0U);
}

TEST_F(TcpBuffers, PoolRejectsZeroAndUnrepresentableCounts) {
    EXPECT_THROW((std::ignore = Pool{0}), std::invalid_argument);
    EXPECT_THROW((std::ignore = Pool{static_cast<std::size_t>(aloe::tcp::detail::no_node)}), std::invalid_argument);
}
```

`tests/unit_tests/common/tcp/CMakeLists.txt`:

```cmake
##############################################################################
# Test Tcp: the table, the buffers, and the brick over a fabric
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Tcp
        test_tcp_table.cpp
        test_tcp_buffers.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Tcp
        PRIVATE
        Aloe::Common::Tcp
        Aloe::Fixtures::Fabric
        Aloe::Fixtures::Frames
        ${TEST_LIBS}
)
##############################################################################
```

Add `add_subdirectory(tcp)` to `tests/unit_tests/common/CMakeLists.txt` after `stream`.

- [ ] **Step 2: Run the test to see it fail**

Run: `cmake --preset debug`
Expected: configure fails on `Aloe::Common::Tcp`.

- [ ] **Step 3: Write the vocabulary, the table, the pool and the view**

`common/tcp/state/tcp_config.hpp`:

```cpp
#pragma once

#include <aloe/core>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include <ipv4_address.hpp>

namespace aloe::tcp {

    struct Endpoint {
        wire::Ipv4Address address{};
        std::uint16_t port = 0;

        friend constexpr bool operator==(const Endpoint&, const Endpoint&) noexcept = default;
    };

    /// The RFC 793 states minimal TCP has; no TIME_WAIT until phase 2.
    enum class State : std::uint8_t {
        Closed,
        SynSent,
        SynReceived,
        Established,
        FinWait1,
        FinWait2,
        CloseWait,
        Closing,
        LastAck,
    };

    enum class ConnectError : std::uint8_t {
        TableFull,    ///< No free connection slot.
        NoPort,       ///< The ephemeral range has no free port that lands on this queue.
        Unplaceable,  ///< The card hashes addresses only, or steers nothing, and this is not the queue it picks.
        NoRoute,      ///< Off the subnet with no gateway.
    };

    enum class ListenError : std::uint8_t {
        InUse,      ///< Already listening on that port.
        TableFull,  ///< Every listener slot is taken.
    };

    /// The peer's MSS when its SYN carried no option: RFC 1122's default.
    inline constexpr std::uint16_t default_peer_mss = 536;

    struct TcpConfig {
        std::size_t connections      = 1024;  ///< Slots per shard.
        std::size_t listeners        = 8;
        std::size_t receive_segments = 32;    ///< Hard cap on held packets per connection; sizes the byte budget too.
        std::size_t receive_pool     = 2048;  ///< Shared retained-packet nodes per shard.
        std::uint16_t ephemeral_first = 32768;  ///< `connect`'s local ports, inclusive.
        std::uint16_t ephemeral_last  = 60999;
        core::Duration retry_initial    = std::chrono::seconds{1};        ///< SYN, SYN-ACK, FIN: doubles per try.
        std::uint8_t retries            = 5;                              ///< Retransmissions before TimedOut.
        core::Duration unresolved_retry = std::chrono::milliseconds{10};  ///< A SYN waiting for ARP; not a try.
    };

}  // namespace aloe::tcp
```

`common/tcp/state/tcp_counters.hpp`:

```cpp
#pragma once

#include <cstdint>

namespace aloe::tcp {

    /// One per stack, monotonic, read on the owning thread or after it has stopped.
    struct TcpCounters {
        std::uint64_t segments_received     = 0;
        std::uint64_t data_segments_sent    = 0;
        std::uint64_t pure_acks_sent        = 0;
        std::uint64_t control_segments_sent = 0;  ///< SYN, SYN-ACK, FIN, first sends and retransmits alike.
        std::uint64_t resets_sent           = 0;
        std::uint64_t retransmits           = 0;  ///< Timer-driven control retransmits.
        std::uint64_t window_updates        = 0;  ///< Segments that extended the advertised edge.

        std::uint64_t connections_opened    = 0;  ///< Active opens that reached ESTABLISHED.
        std::uint64_t connections_accepted  = 0;
        std::uint64_t connections_closed    = 0;  ///< Normal closes, both FINs acknowledged.
        std::uint64_t connections_reset     = 0;  ///< RST received on a connection we own.
        std::uint64_t connections_timed_out = 0;
        std::uint64_t handshakes_failed     = 0;  ///< A passive open nobody owned yet was reset or timed out.

        std::uint64_t dropped_bad_header     = 0;
        std::uint64_t dropped_bad_checksum   = 0;
        std::uint64_t dropped_no_connection  = 0;
        std::uint64_t dropped_closed         = 0;
        std::uint64_t dropped_unexpected     = 0;
        std::uint64_t dropped_duplicate      = 0;
        std::uint64_t dropped_out_of_order   = 0;
        std::uint64_t dropped_out_of_window  = 0;
        std::uint64_t dropped_no_slot        = 0;
        std::uint64_t dropped_no_node        = 0;
        std::uint64_t dropped_table_full     = 0;

        std::uint64_t send_refused    = 0;
        std::uint64_t send_unresolved = 0;

        friend constexpr bool operator==(const TcpCounters&, const TcpCounters&) noexcept = default;
    };

}  // namespace aloe::tcp
```

`common/tcp/table/flow_table.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include <ipv4_address.hpp>

namespace aloe::tcp {

    /// A connection as the table keys it: the received orientation, local address implied by the stack.
    struct FlowKey {
        wire::Ipv4Address remote{};
        std::uint16_t remote_port = 0;
        std::uint16_t local_port  = 0;

        friend constexpr bool operator==(const FlowKey&, const FlowKey&) noexcept = default;
    };

    namespace detail {

        /// The spec's 32-bit avalanche finalizer: applied to every base hash before a bucket is chosen.
        [[nodiscard]] constexpr std::uint32_t mix_flow_hash(std::uint32_t hash) noexcept {
            hash ^= hash >> 16U;
            hash *= 0x85ebca6bU;
            hash ^= hash >> 13U;
            hash *= 0xc2b2ae35U;
            hash ^= hash >> 16U;
            return hash;
        }

        /// FNV-1a over the key's eight network-order bytes: the base hash when the card's RSS hash is not usable.
        [[nodiscard]] constexpr std::uint32_t software_flow_hash(const FlowKey key) noexcept {
            std::uint32_t hash = 0x811c9dc5U;
            const auto mix     = [&hash](const std::uint8_t byte) {
                hash ^= byte;
                hash *= 0x01000193U;
            };
            for (const std::byte byte : key.remote.bytes()) {
                mix(std::to_integer<std::uint8_t>(byte));
            }
            mix(static_cast<std::uint8_t>(key.remote_port >> 8U));
            mix(static_cast<std::uint8_t>(key.remote_port & 0xffU));
            mix(static_cast<std::uint8_t>(key.local_port >> 8U));
            mix(static_cast<std::uint8_t>(key.local_port & 0xffU));
            return hash;
        }

    }  // namespace detail

    /**
     * @brief A fixed open-addressing index from a flow key to a connection slot.
     *
     * Capacity is the next power of two at or above twice `connections`, so the load never passes
     * one half. Linear probing, backward-shift deletion, no tombstones, no allocation after
     * construction. The caller supplies one base hash per key, computed the same way on insert,
     * find and erase; the table mixes it and masks it for the home bucket and stores the mixed
     * value so deletion can recover the home. Equality is on the key's fields, never on the hash.
     */
    class FlowTable {
    public:
        static constexpr std::uint32_t no_index = std::numeric_limits<std::uint32_t>::max();

        /// Throws std::invalid_argument for zero connections or a capacity that does not fit 32 bits.
        explicit FlowTable(std::size_t connections);

        [[nodiscard]] std::optional<std::uint32_t> find(FlowKey key, std::uint32_t base_hash) const noexcept;
        /// False when the key is present already or the table is full.
        [[nodiscard]] bool insert(FlowKey key, std::uint32_t index, std::uint32_t base_hash) noexcept;
        /// False when the key is absent.
        [[nodiscard]] bool erase(FlowKey key, std::uint32_t base_hash) noexcept;

        [[nodiscard]] std::size_t size() const noexcept {
            return size_;
        }

        [[nodiscard]] std::size_t capacity() const noexcept {
            return entries_.size();
        }

    private:
        struct Entry {
            FlowKey key{};
            std::uint32_t index = no_index;
            std::uint32_t mixed = 0;
            bool used           = false;
        };

        [[nodiscard]] std::uint32_t home(const std::uint32_t mixed) const noexcept {
            return mixed & mask_;
        }

        /// How far `slot` is past `from`, cyclically.
        [[nodiscard]] std::uint32_t distance(const std::uint32_t from, const std::uint32_t slot) const noexcept {
            return (slot - from) & mask_;
        }

        [[nodiscard]] std::optional<std::uint32_t> locate(FlowKey key, std::uint32_t mixed) const noexcept;

        std::vector<Entry> entries_;
        std::uint32_t mask_ = 0;
        std::size_t size_   = 0;
    };

}  // namespace aloe::tcp
```

`common/tcp/table/flow_table.cpp`:

```cpp
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>

#include <flow_table.hpp>

namespace aloe::tcp {

    namespace {

        [[nodiscard]] std::size_t capacity_for(const std::size_t connections) {
            if (connections == 0) {
                throw std::invalid_argument{"FlowTable needs at least one connection"};
            }
            if (connections > (std::size_t{1} << 30U)) {
                throw std::invalid_argument{"FlowTable: twice the connections must fit a 32-bit index"};
            }
            return std::bit_ceil(2 * connections);
        }

    }  // namespace

    FlowTable::FlowTable(const std::size_t connections)
        : entries_(capacity_for(connections)),
          mask_{static_cast<std::uint32_t>(entries_.size() - 1)} {
    }

    std::optional<std::uint32_t> FlowTable::locate(const FlowKey key, const std::uint32_t mixed) const noexcept {
        std::uint32_t slot = home(mixed);
        for (std::size_t probed = 0; probed < entries_.size(); ++probed) {
            const Entry& entry = entries_[slot];
            if (!entry.used) {
                return std::nullopt;
            }
            if (entry.mixed == mixed && entry.key == key) {
                return slot;
            }
            slot = (slot + 1) & mask_;
        }
        return std::nullopt;
    }

    std::optional<std::uint32_t> FlowTable::find(const FlowKey key, const std::uint32_t base_hash) const noexcept {
        const std::optional<std::uint32_t> slot = locate(key, detail::mix_flow_hash(base_hash));
        if (!slot) {
            return std::nullopt;
        }
        return entries_[*slot].index;
    }

    bool FlowTable::insert(const FlowKey key, const std::uint32_t index, const std::uint32_t base_hash) noexcept {
        if (size_ == entries_.size()) {
            return false;
        }
        const std::uint32_t mixed = detail::mix_flow_hash(base_hash);
        std::uint32_t slot        = home(mixed);
        while (entries_[slot].used) {
            if (entries_[slot].mixed == mixed && entries_[slot].key == key) {
                return false;
            }
            slot = (slot + 1) & mask_;
        }
        entries_[slot] = Entry{.key = key, .index = index, .mixed = mixed, .used = true};
        ++size_;
        return true;
    }

    bool FlowTable::erase(const FlowKey key, const std::uint32_t base_hash) noexcept {
        const std::optional<std::uint32_t> found = locate(key, detail::mix_flow_hash(base_hash));
        if (!found) {
            return false;
        }
        std::uint32_t hole = *found;
        std::uint32_t next = (hole + 1) & mask_;
        while (entries_[next].used) {
            // An entry moves back into the hole only if the hole is not before its home on the probe path.
            const std::uint32_t its_home = home(entries_[next].mixed);
            if (distance(its_home, hole) < distance(its_home, next)) {
                entries_[hole] = entries_[next];
                hole           = next;
            }
            next = (next + 1) & mask_;
        }
        entries_[hole] = Entry{};
        --size_;
        return true;
    }

}  // namespace aloe::tcp
```

`common/tcp/buffer/tcp_node_pool.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <packet.hpp>

namespace aloe::tcp::detail {

    /// The index that is no node: the end of a chain, an empty free list.
    inline constexpr std::uint32_t no_node = std::numeric_limits<std::uint32_t>::max();

    /**
     * @brief The per-shard pool of nodes a connection chains its held segments on.
     *
     * A node is a packet, the offset of its unconsumed payload, that payload's length and the next
     * node. Fixed at construction, never grown; `acquire` moves the packet in only when a node is
     * free, so a refused packet stays with the caller.
     */
    template <device::IsPacket Packet>
    class TcpNodePool {
    public:
        struct Node {
            Packet packet{};
            std::uint16_t offset = 0;  ///< Of the unconsumed payload inside the packet's data.
            std::uint16_t length = 0;  ///< Unconsumed payload bytes.
            std::uint32_t next   = no_node;
        };

        /// Throws std::invalid_argument for zero nodes or more than the index can name.
        explicit TcpNodePool(const std::size_t count)
            : nodes_(validated(count)),
              available_{count} {
            for (std::size_t index = 0; index + 1 < count; ++index) {
                nodes_[index].next = static_cast<std::uint32_t>(index + 1);
            }
            free_head_ = 0;
        }

        [[nodiscard]] std::optional<std::uint32_t>
        acquire(Packet&& packet, const std::uint16_t offset, const std::uint16_t length) noexcept {
            if (free_head_ == no_node) {
                return std::nullopt;
            }
            const std::uint32_t index = free_head_;
            Node& node                = nodes_[index];
            free_head_                = node.next;
            node.packet               = std::move(packet);
            node.offset               = offset;
            node.length               = length;
            node.next                 = no_node;
            --available_;
            return index;
        }

        /// Returns the packet to its pool and the node to the free list.
        void release(const std::uint32_t index) noexcept {
            Node& node  = nodes_[index];
            node.packet = Packet{};
            node.offset = 0;
            node.length = 0;
            node.next   = free_head_;
            free_head_  = index;
            ++available_;
        }

        [[nodiscard]] Node& node(const std::uint32_t index) noexcept {
            return nodes_[index];
        }

        [[nodiscard]] const Node& node(const std::uint32_t index) const noexcept {
            return nodes_[index];
        }

        [[nodiscard]] std::size_t available() const noexcept {
            return available_;
        }

        [[nodiscard]] std::size_t capacity() const noexcept {
            return nodes_.size();
        }

    private:
        [[nodiscard]] static std::size_t validated(const std::size_t count) {
            if (count == 0 || count >= no_node) {
                throw std::invalid_argument{"TcpNodePool needs between one node and 2^32 - 2"};
            }
            return count;
        }

        std::vector<Node> nodes_;
        std::uint32_t free_head_ = no_node;
        std::size_t available_   = 0;
    };

}  // namespace aloe::tcp::detail
```

`common/tcp/buffer/read_view.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>

#include <packet.hpp>
#include <tcp_node_pool.hpp>

namespace aloe::tcp {

    /**
     * @brief The unread bytes of a connection: one span per held packet, over the pool's storage.
     *
     * A forward range whose value type is `std::span<const std::byte>`; `size()` counts bytes. The
     * view is a pair of indices and a count, copied freely, and is invalidated by `consume` and by
     * the next `process`. The pool pointer is mutable because the packet concept offers `data()`
     * only on a non-const packet; the bytes handed out are const.
     */
    template <device::IsPacket Packet>
    class ReadView {
    public:
        using Pool = detail::TcpNodePool<Packet>;

        class Iterator {
        public:
            using iterator_concept  = std::forward_iterator_tag;
            using iterator_category = std::input_iterator_tag;  // the reference is a prvalue span
            using value_type        = std::span<const std::byte>;
            using difference_type   = std::ptrdiff_t;

            Iterator() noexcept = default;

            Iterator(Pool* pool, const std::uint32_t index) noexcept
                : pool_{pool},
                  index_{index} {
            }

            [[nodiscard]] std::span<const std::byte> operator*() const noexcept {
                typename Pool::Node& node = pool_->node(index_);
                return std::span<const std::byte>{node.packet.data()}.subspan(node.offset, node.length);
            }

            Iterator& operator++() noexcept {
                index_ = pool_->node(index_).next;
                return *this;
            }

            Iterator operator++(int) noexcept {
                const Iterator before = *this;
                ++*this;
                return before;
            }

            friend bool operator==(const Iterator&, const Iterator&) noexcept = default;

        private:
            Pool* pool_          = nullptr;
            std::uint32_t index_ = detail::no_node;
        };

        ReadView() noexcept = default;

        ReadView(Pool& pool, const std::uint32_t head, const std::size_t bytes) noexcept
            : pool_{&pool},
              head_{head},
              bytes_{bytes} {
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return bytes_;
        }

        [[nodiscard]] bool empty() const noexcept {
            return bytes_ == 0;
        }

        [[nodiscard]] std::span<const std::byte> front() const noexcept {
            return *begin();
        }

        [[nodiscard]] Iterator begin() const noexcept {
            return Iterator{pool_, bytes_ == 0 ? detail::no_node : head_};
        }

        [[nodiscard]] Iterator end() const noexcept {
            return Iterator{pool_, detail::no_node};
        }

    private:
        Pool* pool_         = nullptr;
        std::uint32_t head_ = detail::no_node;
        std::size_t bytes_  = 0;
    };

}  // namespace aloe::tcp
```

`common/tcp/CMakeLists.txt` (the `.Stack` target lists the five brick headers Task 6 creates; add them in Task 6, or create them empty here and fill them there, whichever keeps the configure green):

```cmake
set(TCP ${COMMON}.Tcp)

##############################################################################
# The connection table: a fixed open-addressing index, backend-independent
##############################################################################
add_library(${TCP}.Table STATIC
        table/flow_table.hpp
        table/flow_table.cpp
)
target_include_directories(${TCP}.Table PUBLIC
        table/
)
target_link_libraries(${TCP}.Table PUBLIC
        Aloe::Common::Wire
        Aloe::Common::Core
)
##############################################################################

##############################################################################
# The brick: configuration, counters, retained-packet storage and the stack over IP
##############################################################################
add_library(${TCP}.Stack INTERFACE
        state/tcp_config.hpp
        state/tcp_counters.hpp
        buffer/tcp_node_pool.hpp
        buffer/read_view.hpp
)
target_include_directories(${TCP}.Stack INTERFACE
        state/
        buffer/
        stack/
)
target_link_libraries(${TCP}.Stack INTERFACE
        ${TCP}.Table
        Aloe::Common::Net
        Aloe::Common::Stream
        Aloe::Common::Loop
        Aloe::Common::Device
        Aloe::Common::Wire
        Aloe::Common::Core
)
##############################################################################

##############################################################################
# Tcp exported library
##############################################################################
add_combined_library(${TCP}
        DIRECTORIES
        export
        SOURCES
        export/aloe/tcp
        LIBRARIES
        ${TCP}.Table
        ${TCP}.Stack
)

add_library(Aloe::Common::Tcp ALIAS ${TCP})
##############################################################################
```

`common/tcp/export/aloe/tcp`:

```cpp
#pragma once

/**
 * Public umbrella for the Aloe tcp module: the minimal TCP brick over the IP brick and a timer
 * wheel, its connection table, its retained-packet storage and the connection as a zero-copy
 * stream. Names no execution model and no logger. Consumers link Aloe::Common::Tcp and write
 * `#include <aloe/tcp>`.
 */

#include <flow_table.hpp>
#include <read_view.hpp>
#include <tcp_config.hpp>
#include <tcp_counters.hpp>
#include <tcp_node_pool.hpp>
```

Add `add_subdirectory(tcp)` to `common/CMakeLists.txt` between `stream` and `runtime`. Delete the two-line `TODO: Issue#5` comment in `common/net/arp/arp_cache.hpp`; the ARP table itself does not change.

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Tcp Aloe.Tests.Unit.Stream Aloe.Tests.Unit.Net && ctest --preset debug -R '^Aloe[.]Tests[.]Unit[.](Tcp|Stream|Net)$' --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Check the link interface and format**

Run: `grep -n 'Execution\|Log\|Dpdk\|Fixtures' common/tcp/CMakeLists.txt` and expect no output. Run `./scripts/check-format.sh`.

- [ ] **Step 6: Commit**

```bash
git add common/CMakeLists.txt common/tcp common/net/arp/arp_cache.hpp tests/unit_tests/common/CMakeLists.txt tests/unit_tests/common/tcp
git commit -m "feat(tcp): add bounded flow and receive storage"
```

## Task 6: Connection lifecycle, handshake and control timers

**Files:**
- Create: `common/tcp/stack/tcp_fwd.hpp`, `tcp_connection.hpp`, `tcp_stack.hpp`, `tcp_receive.hpp`, `tcp_transmit.hpp`, `tcp_control.hpp`
- Create: `tests/shared/tcp/tcp_test_device.hpp`, `tests/shared/tcp/tcp_fixture.hpp`
- Create: `tests/unit_tests/common/tcp/test_tcp_handshake.cpp`, `test_tcp_events.cpp`
- Modify: `common/tcp/CMakeLists.txt`, `common/tcp/export/aloe/tcp`, `tests/shared/CMakeLists.txt`, `tests/unit_tests/common/tcp/CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 1 to 5; `net::Ipv4<Device>` with `allocate`, `send`, `received`, `address`, `max_l4_size`, `next_hop`, `queue`; `net::Datagram`; `loop::TimerWheel`, `loop::Timer`; `device::flow_hash`, `queue_for`.
- Produces: `tcp::IsIp<Ip>`; `tcp::Connection<Ip>` with the complete public interface of the spec (`index`, `state`, `local`, `remote`, `events`, `mss`, `unread`, `consume`, `peer_closed`, `writable`, `prepare`, `commit`, `send`, `committed`, `acknowledged`, `unacknowledged`, `close`, `abort`, `release`); `tcp::ConnectionEvent<Ip>{connection, events}`; `tcp::Stack<Ip>` with `ConnectionType`, `EventType`, the constructor, `process`, `poll_event`, `flush`, `listen`, `unlisten`, `connect`, `connection(index)`, `capacity`, `mss`, `byte_budget`, `counters`, `config`, `listening(port)`, `pending_events`, `now`, `nodes_available`, `table_size`. The whole receive dispatcher is written here, because every state's handling is reached from `process` and a template member without a definition fails to link once `process` is instantiated. `unread`, `consume`, `prepare`, `commit`, `send`, `close` are declared here and defined in Tasks 7 to 9; nothing in this task's tests reaches them.
- Fixtures: `testing::TcpTestDevice` (a `fabric::Port` wrapper that can refuse transmits, fail allocations and override steering); `testing::TcpHarness<Device>` with `configure_tcp`, `receive`, `process_pending`, `advance`, `collect`, `collect_queued`, `poll`, `open_passive`, `open_active`, `burst_empty`; `testing::TcpFixture` (over `fabric::Port`) and `testing::TcpRefusingFixture` (over `TcpTestDevice`), both parameterised over the emulated offloads.

- [ ] **Step 1: Write the fixtures**

`tests/shared/tcp/tcp_test_device.hpp`:

```cpp
#pragma once

#include <aloe/device>
#include <aloe/fabric>
#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace aloe::testing {

    /**
     * @brief A fabric port seen through a device that can refuse transmits, fail allocations and
     * report another steering description: for the refusals the fabric never produces.
     *
     * Models IsDevice; every call delegates to the port unless a knob says otherwise. Used from the
     * port's queue thread only.
     */
    class TcpTestDevice {
    public:
        using Packet = fabric::Packet;

        explicit TcpTestDevice(fabric::Port& port) noexcept
            : port_{&port} {
        }

        bool refuse_transmit             = false;  ///< `transmit` accepts nothing while set.
        std::size_t fail_allocations     = 0;      ///< That many `allocate` calls return nothing, then normal.
        std::optional<device::RssDescription> steering_override;

        [[nodiscard]] std::uint16_t queue_count() const noexcept { return port_->queue_count(); }
        [[nodiscard]] wire::MacAddress mac() const noexcept { return port_->mac(); }
        [[nodiscard]] std::uint16_t mtu() const noexcept { return port_->mtu(); }
        [[nodiscard]] bool link_up() const noexcept { return port_->link_up(); }
        [[nodiscard]] const device::Capabilities& capabilities() const noexcept { return port_->capabilities(); }

        [[nodiscard]] const device::RssDescription& steering() const noexcept {
            return steering_override ? *steering_override : port_->steering();
        }

        [[nodiscard]] std::optional<Packet> allocate(const std::uint16_t queue) noexcept {
            if (fail_allocations > 0) {
                --fail_allocations;
                return std::nullopt;
            }
            return port_->allocate(queue);
        }

        [[nodiscard]] std::size_t receive(const std::uint16_t queue, std::span<Packet> out) const noexcept {
            return port_->receive(queue, out);
        }

        [[nodiscard]] std::size_t transmit(const std::uint16_t queue, std::span<Packet> in) const noexcept {
            return refuse_transmit ? 0 : port_->transmit(queue, in);
        }

        [[nodiscard]] device::QueueCounters counters(const std::uint16_t queue) const noexcept {
            return port_->counters(queue);
        }

        [[nodiscard]] fabric::Port& port() noexcept { return *port_; }

    private:
        fabric::Port* port_;
    };

    static_assert(device::IsDevice<TcpTestDevice>);

}  // namespace aloe::testing
```

`tests/shared/tcp/tcp_fixture.hpp`:

```cpp
#pragma once

#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/tcp>
#include <aloe/wire>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <net_fixture.hpp>
#include <tcp_test_device.hpp>

namespace aloe::testing {

    inline constexpr std::uint16_t tcp_listen_port = 7;
    inline constexpr std::uint16_t tcp_peer_port   = 40000;
    inline constexpr std::uint32_t tcp_peer_isn    = 1000;
    inline constexpr std::uint16_t tcp_fabric_mss  = 1460;  ///< MTU 1500 less both headers.
    inline constexpr std::uint32_t tcp_fabric_budget = 32U * tcp_fabric_mss;  ///< 46720: the default byte budget.

    /// The peer's outbound orientation towards the stack's listener.
    [[nodiscard]] inline frames::TcpSpec peer_spec(const std::uint16_t peer_port = tcp_peer_port) {
        return {.destination_mac  = stack_mac,
                .source_mac       = harness_mac,
                .source           = harness_ip,
                .destination      = stack_ip,
                .source_port      = peer_port,
                .destination_port = tcp_listen_port,
                .mss              = tcp_fabric_mss};
    }

    [[nodiscard]] inline bool is_flags(const frames::ParsedFrame& frame, const wire::TcpFlags flags) {
        return frame.tcp.has_value() && frame.tcp->flags == flags;
    }

    namespace detail {

        /// A fabric with the harness port and the stack port, constructed before the harness that uses them.
        struct FabricPair {
            explicit FabricPair(const fabric::EmulatedOffloads offloads,
                                const std::uint16_t stack_queues = 1,
                                const std::size_t pool_size      = 64)
                : harness{fabric.add_port({.mac = harness_mac, .pool_size = 256, .queue_depth = 4096, .offloads = offloads})},
                  port{fabric.add_port({.mac = stack_mac, .queues = stack_queues, .pool_size = pool_size, .offloads = offloads})} {
            }

            fabric::Fabric fabric;
            fabric::Port& harness;
            fabric::Port& port;
        };

        struct TestDeviceHolder {
            explicit TestDeviceHolder(fabric::Port& port) noexcept
                : device{port} {
            }

            TcpTestDevice device;
        };

    }  // namespace detail

    /**
     * @brief The brick over one queue of a device, a peer scripted by the test, and the helpers
     * to inject, tick and collect. Time is `now_`: the epoch plus what the test adds.
     *
     * `receive` injects through IP and TCP without flushing; `advance` moves the stamp, runs an
     * empty `process` and advances the wheel; `collect` flushes TCP and the queue and returns what
     * the harness port received. No fatal assertion lives in a helper that returns a value.
     */
    template <device::IsDevice Device>
    class TcpHarness {
    public:
        using Packet     = typename Device::Packet;
        using Ipv4       = net::Ipv4<Device>;
        using Tcp        = tcp::Stack<Ipv4>;
        using Connection = typename Tcp::ConnectionType;
        using Event      = typename Tcp::EventType;
        using TimePoint  = core::TimePoint;

        /// `ring_capacity` 1 makes the first refused device transmit refuse the send itself: the refusing fixture's choice.
        TcpHarness(fabric::Port& harness,
                   Device& device,
                   const tcp::TcpConfig& config     = {},
                   const net::Ipv4Config& ip_config = stack_config(),
                   const std::size_t ring_capacity  = 16)
            : harness_{&harness},
              queue_{device, 0, ring_capacity, counters_},
              ip_{queue_, ip_config},
              tcp_{std::make_unique<Tcp>(ip_, wheel_, config)} {
            ip_.learn(harness_ip, harness_mac, now_);
        }

    protected:
        fabric::Port* harness_;
        loop::ShardCounters counters_;
        loop::ShardQueue<Device> queue_;
        Ipv4 ip_;
        loop::TimerWheel wheel_{std::chrono::milliseconds{1}, TimePoint{}};
        std::unique_ptr<Tcp> tcp_;
        TimePoint now_{};
        std::vector<Packet> burst_ = std::vector<Packet>(64);
        std::size_t last_burst_    = 0;
        frames::TcpPeer peer_{peer_spec(), wire::TcpSequence{tcp_peer_isn}};

        /// Replaces the stack: setup only, before any connection exists.
        void configure_tcp(const tcp::TcpConfig& config) {
            tcp_.reset();
            tcp_ = std::make_unique<Tcp>(ip_, wheel_, config);
        }

        /// Sends `frame` from the harness, then runs IP and TCP over what the stack port received. No flush.
        void receive(const std::span<const std::byte> frame) {
            auto packet = harness_->allocate(0);
            ASSERT_TRUE(packet.has_value());
            ASSERT_TRUE(frames::fill(*packet, frame));
            std::array<Packet, 1> out{std::move(*packet)};
            ASSERT_EQ(harness_->transmit(0, out), 1U);
            process_pending();
        }

        /// One receive pass over whatever the stack port holds, which may be nothing.
        void process_pending() {
            last_burst_ = queue_.receive(burst_);
            ip_.process(std::span<Packet>{burst_}.first(last_burst_), now_);
            tcp_->process(ip_.received(wire::Ipv4Protocol::Tcp), now_);
        }

        /// Moves time, runs an empty process at the new stamp, then fires the timers due.
        void advance(const core::Duration by) {
            now_ += by;
            tcp_->process({}, now_);
            std::ignore = wheel_.advance(now_);
        }

        /// Flushes TCP and the queue, then returns every frame the harness received, oldest first.
        [[nodiscard]] std::vector<frames::ParsedFrame> collect() {
            tcp_->flush(now_);
            return collect_queued();
        }

        /// Flushes only the queue: what was already queued, without TCP's pending ACKs.
        [[nodiscard]] std::vector<frames::ParsedFrame> collect_queued() {
            std::ignore = queue_.flush();
            std::vector<frames::ParsedFrame> frames;
            std::array<Packet, 16> out;
            for (std::size_t count = harness_->receive(0, out); count > 0; count = harness_->receive(0, out)) {
                for (std::size_t index = 0; index < count; ++index) {
                    if (auto parsed = frames::parse_frame(frames::bytes_of(out[index]))) {
                        frames.push_back(std::move(*parsed));
                    }
                    out[index] = Packet{};
                }
            }
            return frames;
        }

        [[nodiscard]] std::optional<Event> poll() {
            return tcp_->poll_event();
        }

        /// A scripted passive open: SYN in, SYN-ACK out, ACK in, `Accepted` drained. Null on failure.
        [[nodiscard]] Connection* open_passive(frames::TcpPeer& peer) {
            if (!tcp_->listening(tcp_listen_port)) {
                std::ignore = tcp_->listen(tcp_listen_port);
            }
            receive(peer.syn());
            const auto frames = collect();
            if (frames.size() != 1 || !is_flags(frames[0], {wire::TcpFlag::Syn, wire::TcpFlag::Ack})) {
                ADD_FAILURE() << "expected one SYN-ACK, got " << frames.size() << " frames";
                return nullptr;
            }
            peer.see(frames[0]);
            receive(peer.ack());
            const auto event = poll();
            if (!event || !event->events.accepted()) {
                ADD_FAILURE() << "expected the Accepted event";
                return nullptr;
            }
            return event->connection;
        }

        /// A scripted active open: SYN out, SYN-ACK in, `Connected` drained, the deferred ACK flushed. Null on failure.
        [[nodiscard]] Connection* open_active(frames::TcpPeer& peer) {
            const auto connected = tcp_->connect({.address = harness_ip, .port = peer.spec().source_port}, now_);
            if (!connected) {
                ADD_FAILURE() << "connect failed: " << static_cast<int>(connected.error());
                return nullptr;
            }
            const auto frames = collect();
            if (frames.size() != 1 || !is_flags(frames[0], {wire::TcpFlag::Syn})) {
                ADD_FAILURE() << "expected one SYN, got " << frames.size() << " frames";
                return nullptr;
            }
            receive(peer.syn_ack(frames[0]));
            const auto event = poll();
            if (!event || !event->events.connected()) {
                ADD_FAILURE() << "expected the Connected event";
                return nullptr;
            }
            const auto acks = collect();
            if (acks.size() != 1 || !is_flags(acks[0], {wire::TcpFlag::Ack})) {
                ADD_FAILURE() << "expected the handshake ACK, got " << acks.size() << " frames";
                return nullptr;
            }
            peer.see(acks[0]);
            return *connected;
        }

        /// Every slot the last process was handed is empty afterwards.
        [[nodiscard]] bool burst_empty() const {
            return std::ranges::all_of(std::span<const Packet>{burst_}.first(last_burst_),
                                       [](const Packet& packet) { return packet.empty(); });
        }
    };

    /// The brick over a fabric port: the fixture most TCP tests use.
    class TcpFixture : public ::testing::TestWithParam<fabric::EmulatedOffloads>,
                       protected detail::FabricPair,
                       protected TcpHarness<fabric::Port> {
    protected:
        TcpFixture()
            : FabricPair{GetParam()},
              TcpHarness{harness, port} {
        }

        [[nodiscard]] static bool offloads() {
            return GetParam() == fabric::EmulatedOffloads::Checksums;
        }
    };

    /// The brick over a device that can refuse: for refused sends and failed allocations.
    class TcpRefusingFixture : public ::testing::TestWithParam<fabric::EmulatedOffloads>,
                               protected detail::FabricPair,
                               protected detail::TestDeviceHolder,
                               protected TcpHarness<TcpTestDevice> {
    protected:
        TcpRefusingFixture()
            : FabricPair{GetParam()},
              TestDeviceHolder{port},
              TcpHarness{harness, device, {}, stack_config(), 1} {
        }

        /// From now on every send is refused: the device takes nothing, and an empty packet fills the one-slot ring
        /// so the very next transmit meets a full ring and a refusing device. The fabric drops the empty packet later.
        void refuse() {
            device.refuse_transmit = true;
            auto filler            = port.allocate(0);
            ASSERT_TRUE(filler.has_value());
            std::ignore = queue_.transmit(std::move(*filler));
        }

        void allow() {
            device.refuse_transmit = false;
        }

        [[nodiscard]] static bool offloads() {
            return GetParam() == fabric::EmulatedOffloads::Checksums;
        }
    };

}  // namespace aloe::testing
```

Add to `tests/shared/CMakeLists.txt`:

```cmake
##############################################################################
# The TCP brick over a fabric, with a scripted peer: the harness fixture
##############################################################################
add_library(${SHARED_TESTING_TARGET}.Tcp INTERFACE
        tcp/tcp_fixture.hpp
        tcp/tcp_test_device.hpp
)
target_include_directories(${SHARED_TESTING_TARGET}.Tcp INTERFACE
        tcp/
)
target_link_libraries(${SHARED_TESTING_TARGET}.Tcp INTERFACE
        Aloe::Common::Tcp
        Aloe::Common::Net
        Aloe::Common::Loop
        Aloe::Fixtures::Fabric
        Aloe::Fixtures::Frames
        ${SHARED_TESTING_TARGET}.Net
        ${TEST_LIBS}
)
##############################################################################
```

- [ ] **Step 2: Write the failing tests**

`tests/unit_tests/common/tcp/test_tcp_handshake.cpp`:

```cpp
#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <tuple>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>

namespace {

    using namespace std::chrono_literals;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;
    using aloe::wire::TcpSequence;
    using TcpHandshake = aloe::testing::TcpFixture;

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpHandshake,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

    [[nodiscard]] std::uint16_t mss_option(const aloe::frames::ParsedFrame& frame) {
        const auto options = aloe::wire::TcpOptions::parse(
            std::span<const std::byte>{frame.l4}.subspan(20, frame.tcp->data_offset - 20U));
        return options && options->mss ? *options->mss : 0;
    }

}  // namespace

TEST_P(TcpHandshake, PassiveOpen) {
    ASSERT_TRUE(tcp_->listen(aloe::testing::tcp_listen_port).has_value());
    receive(peer_.syn());
    EXPECT_TRUE(burst_empty());
    EXPECT_EQ(tcp_->pending_events(), 0U) << "nobody owns the connection before the handshake completes";
    EXPECT_EQ(tcp_->table_size(), 1U);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    const auto& syn_ack = frames[0];
    ASSERT_TRUE(syn_ack.tcp.has_value());
    EXPECT_EQ(syn_ack.tcp->flags, (TcpFlags{TcpFlag::Syn, TcpFlag::Ack}));
    EXPECT_EQ(syn_ack.tcp->source_port, aloe::testing::tcp_listen_port);
    EXPECT_EQ(syn_ack.tcp->destination_port, aloe::testing::tcp_peer_port);
    EXPECT_EQ(syn_ack.tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 1});
    EXPECT_EQ(syn_ack.tcp->data_offset, 24U);
    EXPECT_EQ(mss_option(syn_ack), aloe::testing::tcp_fabric_mss);
    EXPECT_EQ(syn_ack.tcp->window, aloe::testing::tcp_fabric_budget);
    EXPECT_EQ(syn_ack.ethernet.destination, aloe::testing::harness_mac);
    EXPECT_EQ(aloe::frames::l4_checksum_residue(*syn_ack.ipv4, syn_ack.l4), 0U);
    EXPECT_EQ(wheel_.pending(), 1U) << "the SYN-ACK retransmit timer";
    const TcpSequence iss = syn_ack.tcp->sequence;

    peer_.see(syn_ack);
    receive(peer_.ack());
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.accepted());
    EXPECT_EQ(event->events, aloe::stream::Events{aloe::stream::Event::Accepted});
    auto& c = *event->connection;
    EXPECT_EQ(c.state(), aloe::tcp::State::Established);
    EXPECT_EQ(c.index(), 0U);
    EXPECT_EQ(&tcp_->connection(0), &c);
    EXPECT_EQ(c.local(), (aloe::tcp::Endpoint{aloe::testing::stack_ip, aloe::testing::tcp_listen_port}));
    EXPECT_EQ(c.remote(), (aloe::tcp::Endpoint{aloe::testing::harness_ip, aloe::testing::tcp_peer_port}));
    EXPECT_EQ(c.mss(), aloe::testing::tcp_fabric_mss);
    EXPECT_EQ(c.committed(), iss + 1U);
    EXPECT_EQ(c.acknowledged(), iss + 1U);
    EXPECT_EQ(c.unacknowledged(), 0U);
    EXPECT_FALSE(c.events().any()) << "poll_event cleared the snapshot";
    EXPECT_EQ(wheel_.pending(), 0U) << "the handshake ACK cancelled the timer";
    EXPECT_FALSE(poll().has_value());
    EXPECT_TRUE(collect().empty()) << "a pure ACK is not acknowledged";
    EXPECT_EQ(tcp_->counters().connections_accepted, 1U);
    EXPECT_EQ(tcp_->counters().control_segments_sent, 1U);
    EXPECT_EQ(tcp_->counters().segments_received, 2U);
}

TEST_P(TcpHandshake, ActiveOpen) {
    const auto connected = tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    auto& c = **connected;
    EXPECT_EQ(c.state(), aloe::tcp::State::SynSent);
    EXPECT_EQ(c.index(), 0U);
    EXPECT_EQ(wheel_.pending(), 1U);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    const auto& syn = frames[0];
    ASSERT_TRUE(syn.tcp.has_value());
    EXPECT_EQ(syn.tcp->flags, TcpFlags{TcpFlag::Syn});
    EXPECT_EQ(syn.tcp->destination_port, aloe::testing::tcp_peer_port);
    EXPECT_GE(syn.tcp->source_port, 32768U);
    EXPECT_LE(syn.tcp->source_port, 60999U);
    EXPECT_EQ(c.local().port, syn.tcp->source_port);
    EXPECT_EQ(syn.tcp->data_offset, 24U);
    EXPECT_EQ(mss_option(syn), aloe::testing::tcp_fabric_mss);
    EXPECT_EQ(syn.tcp->window, aloe::testing::tcp_fabric_budget);
    EXPECT_EQ(syn.tcp->acknowledgement, TcpSequence{});
    EXPECT_EQ(aloe::frames::l4_checksum_residue(*syn.ipv4, syn.l4), 0U);
    const TcpSequence iss = syn.tcp->sequence;
    EXPECT_EQ(c.committed(), iss + 1U);

    receive(peer_.syn_ack(syn));
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.connected());
    EXPECT_EQ(event->connection, &c);
    EXPECT_EQ(c.state(), aloe::tcp::State::Established);
    EXPECT_EQ(c.remote(), (aloe::tcp::Endpoint{aloe::testing::harness_ip, aloe::testing::tcp_peer_port}));
    EXPECT_EQ(c.acknowledged(), iss + 1U);
    EXPECT_EQ(wheel_.pending(), 0U);
    EXPECT_TRUE(collect_queued().empty()) << "the handshake ACK waits for flush, so application data can carry it";
    const auto acks = collect();
    ASSERT_EQ(acks.size(), 1U);
    EXPECT_EQ(acks[0].tcp->flags, TcpFlags{TcpFlag::Ack});
    EXPECT_EQ(acks[0].tcp->sequence, iss + 1U);
    EXPECT_EQ(acks[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 1});
    EXPECT_EQ(acks[0].tcp->window, aloe::testing::tcp_fabric_budget);
    EXPECT_EQ(acks[0].tcp->data_offset, 20U);
    EXPECT_EQ(tcp_->counters().connections_opened, 1U);
    EXPECT_EQ(tcp_->counters().pure_acks_sent, 1U);
    EXPECT_FALSE(poll().has_value());
}

TEST_P(TcpHandshake, DuplicateSyn) {
    ASSERT_TRUE(tcp_->listen(aloe::testing::tcp_listen_port).has_value());
    receive(peer_.syn());
    const auto first = collect();
    ASSERT_EQ(first.size(), 1U);
    aloe::frames::TcpPeer again{aloe::testing::peer_spec(), TcpSequence{aloe::testing::tcp_peer_isn}};
    receive(again.syn());
    const auto second = collect();
    ASSERT_EQ(second.size(), 1U);
    EXPECT_EQ(second[0].tcp->flags, (TcpFlags{TcpFlag::Syn, TcpFlag::Ack}));
    EXPECT_EQ(second[0].tcp->sequence, first[0].tcp->sequence) << "the same ISN, re-sent";
    EXPECT_EQ(tcp_->table_size(), 1U);
    EXPECT_EQ(tcp_->counters().control_segments_sent, 2U);
    EXPECT_EQ(tcp_->counters().dropped_table_full, 0U);
    EXPECT_FALSE(poll().has_value());
}

TEST_P(TcpHandshake, RefusedOpen) {
    receive(peer_.syn());  // nobody listens on 7
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->flags, (TcpFlags{TcpFlag::Rst, TcpFlag::Ack}));
    EXPECT_EQ(frames[0].tcp->sequence, TcpSequence{});
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 1}) << "SYN counts one";
    EXPECT_EQ(frames[0].tcp->source_port, aloe::testing::tcp_listen_port);
    EXPECT_EQ(frames[0].tcp->destination_port, aloe::testing::tcp_peer_port);
    EXPECT_EQ(tcp_->counters().dropped_no_connection, 1U);
    EXPECT_EQ(tcp_->counters().resets_sent, 1U);
    EXPECT_EQ(tcp_->table_size(), 0U);
}

TEST_P(TcpHandshake, StrayAckGetsRstWithRfc793Numbers) {
    receive(peer_.segment({TcpFlag::Ack}, {}, TcpSequence{1001U}, TcpSequence{777U}));
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->flags, TcpFlags{TcpFlag::Rst}) << "with ACK: a bare RST";
    EXPECT_EQ(frames[0].tcp->sequence, TcpSequence{777U}) << "at the stray segment's acknowledgement";
    EXPECT_EQ(tcp_->counters().dropped_no_connection, 1U);
    receive(peer_.rst());
    EXPECT_TRUE(collect().empty()) << "a RST is never answered with a RST";
    EXPECT_EQ(tcp_->counters().dropped_no_connection, 2U);
    EXPECT_EQ(tcp_->counters().resets_sent, 1U);
}

TEST_P(TcpHandshake, ResetInSynSentIsRefused) {
    const auto connected = tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    peer_.see(frames[0]);
    receive(peer_.rst());
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.reset());
    EXPECT_EQ((*connected)->state(), aloe::tcp::State::Closed);
    EXPECT_EQ(wheel_.pending(), 0U);
    EXPECT_EQ(tcp_->counters().connections_reset, 1U);
    EXPECT_EQ(tcp_->table_size(), 1U) << "a failed active open keeps its slot until release";
    (*connected)->release();
    EXPECT_EQ(tcp_->table_size(), 0U);
    EXPECT_TRUE(collect().empty()) << "release of a closed connection sends nothing";
}

TEST_P(TcpHandshake, ControlRetrySchedule) {
    const auto connected = tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    const auto first = collect();
    ASSERT_EQ(first.size(), 1U);
    const TcpSequence iss = first[0].tcp->sequence;
    advance(999ms);
    EXPECT_TRUE(collect().empty());
    std::uint64_t retransmits = 0;
    for (const auto at : {1000ms, 3000ms, 7000ms, 15000ms, 31000ms}) {
        advance(at - now_.time_since_epoch());  // the fixture's time starts at the epoch
        const auto frames = collect();
        ASSERT_EQ(frames.size(), 1U) << "a retransmission at " << at.count() << " ms";
        EXPECT_EQ(frames[0].tcp->flags, TcpFlags{TcpFlag::Syn});
        EXPECT_EQ(frames[0].tcp->sequence, iss) << "the original sequence";
        EXPECT_EQ(tcp_->counters().retransmits, ++retransmits);
        EXPECT_FALSE(poll().has_value());
    }
    advance(31999ms - 31000ms);
    EXPECT_TRUE(collect().empty());
    advance(63000ms - 31999ms);
    EXPECT_TRUE(collect().empty()) << "no sixth retransmission";
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.timed_out());
    EXPECT_EQ((*connected)->state(), aloe::tcp::State::Closed);
    EXPECT_EQ(wheel_.pending(), 0U);
    EXPECT_EQ(tcp_->counters().connections_timed_out, 1U);
    EXPECT_EQ(tcp_->counters().control_segments_sent, 6U);
}

TEST_P(TcpHandshake, PassiveRetryAndUnownedTimeout) {
    ASSERT_TRUE(tcp_->listen(aloe::testing::tcp_listen_port).has_value());
    receive(peer_.syn());
    const auto first = collect();
    ASSERT_EQ(first.size(), 1U);
    advance(1000ms);
    const auto second = collect();
    ASSERT_EQ(second.size(), 1U);
    EXPECT_EQ(second[0].tcp->flags, (TcpFlags{TcpFlag::Syn, TcpFlag::Ack}));
    EXPECT_EQ(second[0].tcp->sequence, first[0].tcp->sequence);
    advance(62000ms);
    EXPECT_FALSE(poll().has_value()) << "nobody owned it: no event";
    EXPECT_EQ(tcp_->counters().handshakes_failed, 1U);
    EXPECT_EQ(tcp_->counters().connections_timed_out, 0U);
    EXPECT_EQ(tcp_->table_size(), 0U) << "the slot was freed";
    EXPECT_EQ(wheel_.pending(), 0U);
    std::ignore = collect();
    receive(peer_.syn());
    EXPECT_EQ(collect().size(), 1U) << "a fresh SYN gets a fresh SYN-ACK from the freed slot";
}

TEST_P(TcpHandshake, UnresolvedDoesNotSpendRetries) {
    // far_ip is behind the gateway, whose MAC nobody has learned: the SYN waits for ARP.
    const auto connected = tcp_->connect({.address = aloe::testing::far_ip, .port = 7}, now_);
    ASSERT_TRUE(connected.has_value());
    EXPECT_EQ((*connected)->state(), aloe::tcp::State::SynSent);
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_TRUE(frames[0].arp.has_value()) << "an ARP request for the gateway, no SYN";
    EXPECT_EQ(tcp_->counters().send_unresolved, 1U);
    EXPECT_EQ(wheel_.pending(), 1U);
    advance(10ms);
    EXPECT_TRUE(collect().empty()) << "still unresolved; the IP brick rate-limits its ARP requests";
    EXPECT_EQ(tcp_->counters().send_unresolved, 2U);
    EXPECT_EQ(tcp_->counters().retransmits, 0U);
    receive(aloe::frames::arp_frame(
        aloe::frames::arp_reply(aloe::testing::gateway_mac, aloe::testing::gateway_ip, aloe::testing::stack_mac, aloe::testing::stack_ip),
        aloe::testing::stack_mac));
    advance(10ms);
    frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    ASSERT_TRUE(frames[0].tcp.has_value());
    EXPECT_EQ(frames[0].tcp->flags, TcpFlags{TcpFlag::Syn});
    EXPECT_EQ(frames[0].ethernet.destination, aloe::testing::gateway_mac);
    EXPECT_EQ(tcp_->counters().retransmits, 0U) << "the first successful send is not a retry";
    EXPECT_EQ(tcp_->counters().control_segments_sent, 1U);
    advance(999ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);
    EXPECT_EQ(collect().size(), 1U) << "normal retry timing starts from the successful send";
    EXPECT_EQ(tcp_->counters().retransmits, 1U);
}

TEST_P(TcpHandshake, NoRouteTakesNoSlot) {
    Ipv4 bare{queue_, {.address = aloe::testing::stack_ip, .prefix = 24}};
    Tcp tcp{bare, wheel_, {.connections = 1}};
    const auto failed = tcp.connect({.address = aloe::testing::far_ip, .port = 7}, now_);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error(), aloe::tcp::ConnectError::NoRoute);
    EXPECT_EQ(tcp.table_size(), 0U);
    EXPECT_EQ(wheel_.pending(), 0U);
    const auto ok = tcp.connect({.address = aloe::testing::harness_ip, .port = 7}, now_);
    ASSERT_TRUE(ok.has_value()) << "the one slot is still free";
    EXPECT_EQ((*ok)->index(), 0U);
    (*ok)->release();
    std::ignore = collect();
}

TEST_P(TcpHandshake, TableFullAndUnlisten) {
    configure_tcp({.connections = 1});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    aloe::frames::TcpPeer second{aloe::testing::peer_spec(40001), TcpSequence{2000U}};
    receive(second.syn());
    EXPECT_TRUE(collect().empty()) << "a full table drops the SYN silently";
    EXPECT_EQ(tcp_->counters().dropped_table_full, 1U);
    tcp_->unlisten(aloe::testing::tcp_listen_port);
    EXPECT_FALSE(tcp_->listening(aloe::testing::tcp_listen_port));
    EXPECT_EQ(c->state(), aloe::tcp::State::Established) << "unlisten touches no open connection";
    c->release();
    std::ignore = collect();
    receive(second.syn());
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_TRUE(frames[0].tcp->flags.has(TcpFlag::Rst)) << "nobody listens any more";
    ASSERT_TRUE(tcp_->listen(aloe::testing::tcp_listen_port).has_value());
    aloe::frames::TcpPeer third{aloe::testing::peer_spec(40002), TcpSequence{3000U}};
    EXPECT_NE(open_passive(third), nullptr) << "the released slot serves the next open";
}

TEST_P(TcpHandshake, ListenErrors) {
    ASSERT_TRUE(tcp_->listen(7).has_value());
    const auto again = tcp_->listen(7);
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error(), aloe::tcp::ListenError::InUse);
    configure_tcp({.listeners = 1});
    ASSERT_TRUE(tcp_->listen(7).has_value());
    const auto full = tcp_->listen(8);
    ASSERT_FALSE(full.has_value());
    EXPECT_EQ(full.error(), aloe::tcp::ListenError::TableFull);
    tcp_->unlisten(7);
    EXPECT_TRUE(tcp_->listen(8).has_value());
}

TEST_P(TcpHandshake, ConfigValidation) {
    using Config = aloe::tcp::TcpConfig;
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.connections = 0}}), std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.listeners = 0}}), std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.receive_segments = 0}}), std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.receive_pool = 0}}), std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.ephemeral_first = 50000, .ephemeral_last = 40000}}),
                 std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.ephemeral_first = 0}}), std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.retry_initial = std::chrono::seconds{0}}}),
                 std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.retries = 0}}), std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.unresolved_retry = std::chrono::milliseconds{-1}}}),
                 std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.connections = std::size_t{1} << 31U}}),
                 std::invalid_argument);
    const Tcp capped{ip_, wheel_, Config{.receive_segments = 1000}};
    EXPECT_EQ(capped.byte_budget(), 65535U) << "the byte budget never passes the wire window";
    EXPECT_EQ(capped.mss(), aloe::testing::tcp_fabric_mss);
    EXPECT_EQ(capped.capacity(), 1024U);
    EXPECT_EQ(wheel_.pending(), 0U) << "construction arms nothing";
}
```

`tests/unit_tests/common/tcp/test_tcp_events.cpp`:

```cpp
#include <aloe/frames>
#include <aloe/stream>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>
#include <tuple>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>

namespace {

    using namespace std::chrono_literals;
    using aloe::stream::Event;
    using aloe::stream::Events;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpSequence;
    using TcpEvents = aloe::testing::TcpFixture;

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpEvents,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

}  // namespace

TEST_P(TcpEvents, PollConsumesSnapshot) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_FALSE(c->events().any());
    receive(peer_.fin());
    EXPECT_TRUE(c->events().peer_closed()) << "inspection sees the flag";
    EXPECT_TRUE(c->events().peer_closed()) << "and does not consume it";
    EXPECT_EQ(tcp_->pending_events(), 1U);
    receive(peer_.rst());
    EXPECT_EQ(tcp_->pending_events(), 1U) << "two raises, one entry";
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->connection, c);
    EXPECT_EQ(event->events, (Events{Event::PeerClosed, Event::Reset})) << "coalesced flags in one snapshot";
    EXPECT_FALSE(c->events().any());
    EXPECT_FALSE(poll().has_value());
    EXPECT_EQ(tcp_->pending_events(), 0U);
}

TEST_P(TcpEvents, EventsSurviveProcessingAndFlush) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.fin());
    advance(5ms);                                        // an empty process and a wheel advance
    std::ignore = collect();                             // a flush
    receive(peer_.segment({TcpFlag::Ack}, {}, TcpSequence{5U}, TcpSequence{5U}));  // an unrelated stray segment
    std::ignore = collect();
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.peer_closed());
}

TEST_P(TcpEvents, TimerSurvivesNextProcess) {
    const auto connected = tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    std::ignore = collect();
    advance(63000ms);  // every retry, then the timeout, raised from inside the wheel
    EXPECT_EQ(tcp_->pending_events(), 1U);
    receive(peer_.segment({TcpFlag::Ack}, {}, TcpSequence{5U}, TcpSequence{5U}));  // a later receive pass
    std::ignore = collect();
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.timed_out());
    EXPECT_EQ(event->connection, *connected);
}

TEST_P(TcpEvents, RaiseAfterPollQueuesAgain) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.fin());
    ASSERT_TRUE(poll().has_value());
    receive(peer_.rst());
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->events, Events{Event::Reset}) << "a fresh notification with only the new flag";
    EXPECT_EQ(event->connection, c);
}

TEST_P(TcpEvents, ReleaseRemovesPendingEntryAndReuseIsClean) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.fin());
    EXPECT_EQ(tcp_->pending_events(), 1U);
    c->release();
    EXPECT_EQ(tcp_->pending_events(), 0U);
    EXPECT_FALSE(poll().has_value());
    std::ignore = collect();  // the RST release sent
    aloe::frames::TcpPeer next{aloe::testing::peer_spec(40001), TcpSequence{2000U}};
    auto* reused = open_passive(next);
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(reused, c) << "the same slot";
    EXPECT_EQ(reused->remote().port, 40001U);
    EXPECT_FALSE(reused->peer_closed()) << "nothing stale survived the release";
    EXPECT_FALSE(reused->events().any());
    EXPECT_FALSE(poll().has_value());
}

TEST_P(TcpEvents, ReleasingAnotherPendingConnectionLeavesTheRest) {
    auto* first = open_passive(peer_);
    ASSERT_NE(first, nullptr);
    aloe::frames::TcpPeer other{aloe::testing::peer_spec(40001), TcpSequence{2000U}};
    auto* second = open_passive(other);
    ASSERT_NE(second, nullptr);
    receive(peer_.fin());
    receive(other.fin());
    EXPECT_EQ(tcp_->pending_events(), 2U);
    first->release();
    EXPECT_EQ(tcp_->pending_events(), 1U);
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->connection, second);
    EXPECT_FALSE(poll().has_value());
}
```

Add both files and the fixture library to `tests/unit_tests/common/tcp/CMakeLists.txt`:

```cmake
add_unit_test(${UNIT_TESTING_TARGET}.Tcp
        test_tcp_table.cpp
        test_tcp_buffers.cpp
        test_tcp_handshake.cpp
        test_tcp_events.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Tcp
        PRIVATE
        Aloe::Common::Tcp
        Aloe::Fixtures::Fabric
        Aloe::Fixtures::Frames
        ${SHARED_TESTING_TARGET}.Tcp
        ${TEST_LIBS}
)
```

- [ ] **Step 3: Run the tests to see them fail**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Tcp`
Expected: compile errors, `no member named 'Stack' in namespace 'aloe::tcp'`.

- [ ] **Step 4: Write the forward declarations, the connection and the stack declaration**

`common/tcp/stack/tcp_fwd.hpp`:

```cpp
#pragma once

#include <aloe/core>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <utility>

#include <device.hpp>
#include <ipv4.hpp>
#include <ipv4_address.hpp>
#include <shard_queue.hpp>

namespace aloe::tcp {

    template <typename Ip>
    class Connection;

    template <typename Ip>
    struct ConnectionEvent;

    template <typename Ip>
    class Stack;

    /// What the brick asks of the IP layer below it: `net::Ipv4<Device>`, or anything with these members.
    template <typename Ip>
    concept IsIp = device::IsDevice<typename Ip::Device> &&
                   std::same_as<typename Ip::Packet, typename Ip::Device::Packet> &&
                   requires(Ip& ip,
                            const Ip& const_ip,
                            typename Ip::Packet packet,
                            const net::SendRequest& request,
                            core::TimePoint now,
                            wire::Ipv4Address address) {
                       { ip.allocate() } -> std::same_as<std::optional<typename Ip::Packet>>;
                       { ip.send(std::move(packet), request, now) } -> std::same_as<std::expected<void, net::SendError>>;
                       { const_ip.address() } -> std::same_as<wire::Ipv4Address>;
                       { const_ip.max_l4_size() } -> std::same_as<std::uint16_t>;
                       { const_ip.next_hop(address) } -> std::same_as<std::optional<wire::Ipv4Address>>;
                       { ip.queue() } -> std::same_as<loop::ShardQueue<typename Ip::Device>&>;
                   };

    namespace detail {

        /// The links of one intrusive list membership; a connection has one per list it can be on.
        template <typename C>
        struct Link {
            C* prev     = nullptr;
            C* next     = nullptr;
            bool linked = false;
        };

        /**
         * @brief An intrusive doubly linked list of connections through one of their Link members.
         *
         * Constant-time push, remove and pop; a connection is on a list at most once, which `push_back`
         * asserts. No allocation; the links live in the connections.
         */
        template <typename C, Link<C> C::* Member>
        class ConnectionList {
        public:
            void push_back(C& item) noexcept {
                Link<C>& link = item.*Member;
                assert(!link.linked && "a connection is on a list at most once");
                link.prev   = tail_;
                link.next   = nullptr;
                link.linked = true;
                if (tail_ != nullptr) {
                    (tail_->*Member).next = &item;
                } else {
                    head_ = &item;
                }
                tail_ = &item;
                ++size_;
            }

            /// A no-op for a connection that is not on the list.
            void remove(C& item) noexcept {
                Link<C>& link = item.*Member;
                if (!link.linked) {
                    return;
                }
                if (link.prev != nullptr) {
                    (link.prev->*Member).next = link.next;
                } else {
                    head_ = link.next;
                }
                if (link.next != nullptr) {
                    (link.next->*Member).prev = link.prev;
                } else {
                    tail_ = link.prev;
                }
                link = Link<C>{};
                --size_;
            }

            [[nodiscard]] C* pop_front() noexcept {
                C* item = head_;
                if (item != nullptr) {
                    remove(*item);
                }
                return item;
            }

            [[nodiscard]] C* front() const noexcept {
                return head_;
            }

            [[nodiscard]] static C* next_of(C& item) noexcept {
                return (item.*Member).next;
            }

            [[nodiscard]] bool contains(const C& item) const noexcept {
                return (item.*Member).linked;
            }

            [[nodiscard]] bool empty() const noexcept {
                return head_ == nullptr;
            }

            [[nodiscard]] std::size_t size() const noexcept {
                return size_;
            }

        private:
            C* head_          = nullptr;
            C* tail_          = nullptr;
            std::size_t size_ = 0;
        };

    }  // namespace detail

}  // namespace aloe::tcp
```

`common/tcp/stack/tcp_connection.hpp`:

```cpp
#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <flow_table.hpp>
#include <read_view.hpp>
#include <stream.hpp>
#include <stream_events.hpp>
#include <tcp_config.hpp>
#include <tcp_fwd.hpp>
#include <tcp_header.hpp>
#include <tcp_node_pool.hpp>
#include <tcp_sequence.hpp>
#include <timer_wheel.hpp>

namespace aloe::tcp {

    /**
     * @brief One TCP connection as a zero-copy stream: a slot of its stack, stable until `release`.
     *
     * Every member runs on the stack's thread and takes its time from the stack's last stamp. The
     * state is owned by the stack; the public members are the stream contract plus the TCP
     * queries. Construction takes the stack's private tag: slots exist only inside a `Stack`.
     */
    template <typename Ip>
    class Connection {
    public:
        using Packet   = typename Ip::Packet;
        using Sequence = wire::TcpSequence;
        using View     = ReadView<Packet>;

    private:
        struct PrivateTag {};

    public:
        explicit Connection(PrivateTag) noexcept
            : timer_{this} {
        }

        Connection(const Connection&)            = delete;
        Connection& operator=(const Connection&) = delete;
        Connection(Connection&&)                 = delete;
        Connection& operator=(Connection&&)      = delete;
        ~Connection()                            = default;

        [[nodiscard]] std::uint32_t index() const noexcept { return index_; }
        [[nodiscard]] State state() const noexcept { return state_; }
        [[nodiscard]] Endpoint local() const noexcept { return local_; }
        [[nodiscard]] Endpoint remote() const noexcept { return remote_; }
        /// Undrained flags; inspection does not clear them.
        [[nodiscard]] stream::Events events() const noexcept { return events_; }
        /// What we send with: the smaller of the peer's option and ours.
        [[nodiscard]] std::uint16_t mss() const noexcept { return std::min(peer_mss_, stack_->mss_); }

        /// The unconsumed payloads of the held packets, in order. Invalidated by `consume` and the next `process`.
        [[nodiscard]] View unread() const noexcept { return View{stack_->pool_, chain_head_, unread_bytes_}; }
        /// `count` at most `unread().size()`, asserted. Frees packets as they empty; may reopen the window.
        void consume(const std::size_t count) noexcept { stack_->consume(*this, count); }
        [[nodiscard]] bool peer_closed() const noexcept { return peer_closed_; }

        /// `min(mss, peer window - in flight)` in Established or CloseWait with no prepare open, else zero.
        [[nodiscard]] std::size_t writable() const noexcept { return stack_->writable(*this); }
        /// A span of at most `writable()` bytes inside a fresh packet, or nothing. One prepare at a time.
        [[nodiscard]] std::optional<std::span<std::byte>> prepare(const std::size_t count) noexcept {
            return stack_->prepare(*this, count);
        }
        /// Seals the prepared segment with `count` bytes and queues it. False: not sent, bytes not accepted.
        [[nodiscard]] bool commit(const std::size_t count) noexcept { return stack_->commit(*this, count); }
        /// `stream::send`: prepare, copy, commit per segment; the bytes accepted.
        [[nodiscard]] std::size_t send(const std::span<const std::byte> bytes) noexcept {
            return stream::send(*this, bytes);
        }
        /// The sequence after the last committed byte.
        [[nodiscard]] Sequence committed() const noexcept { return snd_nxt_; }
        /// Everything before it reached the peer.
        [[nodiscard]] Sequence acknowledged() const noexcept { return snd_una_; }
        [[nodiscard]] std::size_t unacknowledged() const noexcept { return snd_nxt_.value - snd_una_.value; }

        /// FIN after what was committed; no more sends. Nothing outside Established and CloseWait.
        void close() noexcept { stack_->close(*this); }
        /// RST now, Closed raised.
        void abort() noexcept { stack_->abort(*this); }
        /// Frees the slot, aborting first unless Closed. The owner's last call; the pointer is invalid after.
        void release() noexcept { stack_->release(*this); }

    private:
        friend class Stack<Ip>;  // forms the pointers to the link members; using them needs no access

        /// The wheel's node, carrying the way back to its connection.
        struct TimerNode : loop::Timer {
            explicit TimerNode(Connection* owner) noexcept
                : loop::Timer{&Connection::on_timer},
                  connection{owner} {
            }

            Connection* connection;
        };

        static void on_timer(loop::Timer& timer) noexcept {
            Connection& self = *static_cast<TimerNode&>(timer).connection;
            self.stack_->on_timer(self);
        }

        /// Back to the state of a fresh slot; the stack, index and timer trampoline stay.
        void clear() noexcept {
            assert(!timer_.armed());
            state_   = State::Closed;
            owned_   = false;
            indexed_ = false;
            local_   = {};
            remote_  = {};
            key_     = {};
            base_hash_ = 0;
            events_  = {};
            assert(!event_link_.linked && !ack_link_.linked && !retry_link_.linked);
            ack_pending_    = false;
            finish_pending_ = false;
            rcv_nxt_        = {};
            rcv_adv_        = {};
            unread_bytes_   = 0;
            recovery_end_.reset();
            peer_closed_             = false;
            full_segments_since_ack_ = 0;
            chain_head_              = detail::no_node;
            chain_tail_              = detail::no_node;
            chain_count_             = 0;
            iss_                     = {};
            snd_nxt_                 = {};
            prepared_.reset();
            prepared_size_   = 0;
            fin_sent_        = false;
            fin_sequence_    = {};
            snd_una_         = {};
            snd_wnd_         = 0;
            peer_mss_        = default_peer_mss;
            wl1_             = {};
            wl2_             = {};
            tries_           = 0;
            unresolved_wait_ = false;
        }

        // Identity and ownership.
        Stack<Ip>* stack_    = nullptr;
        std::uint32_t index_ = 0;
        State state_         = State::Closed;
        bool owned_          = false;  ///< An application holds it: after Accepted, or from connect.
        bool indexed_        = false;  ///< Its key is in the flow table.
        Endpoint local_{};
        Endpoint remote_{};
        FlowKey key_{};
        std::uint32_t base_hash_ = 0;

        // Events and the stack's lists.
        stream::Events events_{};
        detail::Link<Connection> event_link_{};
        detail::Link<Connection> ack_link_{};
        detail::Link<Connection> retry_link_{};
        bool ack_pending_    = false;  ///< An ordinary ACK waits for flush.
        bool finish_pending_ = false;  ///< The protocol is done; Closed is raised once the last ACK is queued.

        // Receive: acceptance, consumption and acknowledgement state.
        Sequence rcv_nxt_{};
        Sequence rcv_adv_{};  ///< The right edge already offered to the peer; never retreats.
        std::size_t unread_bytes_ = 0;
        std::optional<Sequence> recovery_end_{};  ///< The furthest end of data dropped above a gap.
        bool peer_closed_                     = false;
        std::uint8_t full_segments_since_ack_ = 0;  ///< Saturates at two.
        std::uint32_t chain_head_             = detail::no_node;
        std::uint32_t chain_tail_             = detail::no_node;
        std::uint32_t chain_count_            = 0;

        // Transmit: preparation and outgoing sequence allocation.
        Sequence iss_{};
        Sequence snd_nxt_{};
        std::optional<Packet> prepared_{};
        std::size_t prepared_size_ = 0;
        bool fin_sent_             = false;
        Sequence fin_sequence_{};

        // Peer feedback: what the ACKs told us.
        Sequence snd_una_{};
        std::uint32_t snd_wnd_ = 0;
        std::uint16_t peer_mss_ = default_peer_mss;
        Sequence wl1_{};
        Sequence wl2_{};

        // Maintenance: the control retransmit timer.
        TimerNode timer_;
        std::uint8_t tries_   = 0;
        bool unresolved_wait_ = false;  ///< The last control send waited for ARP; the next fire is a poll, not a try.
    };

    /// One notification from `poll_event`: the connection and the flags taken from it.
    template <typename Ip>
    struct ConnectionEvent {
        Connection<Ip>* connection = nullptr;  ///< Valid until the owner releases it.
        stream::Events events{};
    };

}  // namespace aloe::tcp
```

`common/tcp/stack/tcp_stack.hpp`:

```cpp
#pragma once

#include <aloe/core>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include <datagram.hpp>
#include <flow_table.hpp>
#include <ipv4.hpp>
#include <rss.hpp>
#include <stream_events.hpp>
#include <tcp_config.hpp>
#include <tcp_connection.hpp>
#include <tcp_counters.hpp>
#include <tcp_fwd.hpp>
#include <tcp_header.hpp>
#include <tcp_node_pool.hpp>
#include <tcp_options.hpp>
#include <timer_wheel.hpp>

namespace aloe::tcp {

    /**
     * @brief The minimal TCP brick: one per shard, over the IP brick and the shard's wheel.
     *
     * Four verbs: `process` the segments IP sorted out, `poll_event` to drain notifications, the
     * connections' non-blocking operations, `flush` for the ACKs that waited. The stack keeps the
     * stamp of the last `process`, `flush` or `connect` and uses it for everything a connection
     * does in between. Everything is allocated at construction; nothing here logs, blocks or
     * names a clock.
     */
    template <typename Ip>
    class Stack {
        static_assert(IsIp<Ip>, "tcp::Stack needs net::Ipv4<Device>, or an IP brick with the same members");

    public:
        using Device         = typename Ip::Device;
        using Packet         = typename Ip::Packet;
        using ConnectionType = Connection<Ip>;
        using EventType      = ConnectionEvent<Ip>;
        using Sequence       = wire::TcpSequence;

        /// Validates and throws std::invalid_argument; allocates everything; sends nothing.
        Stack(Ip& ip, loop::TimerWheel& wheel, const TcpConfig& config);
        Stack(const Stack&)            = delete;
        Stack& operator=(const Stack&) = delete;
        Stack(Stack&&)                 = delete;
        Stack& operator=(Stack&&)      = delete;
        /// Cancels every timer and returns every held packet; the IP brick and the wheel outlive it.
        ~Stack();

        /// Records the stamp, publishes deferred retry hints, then processes each datagram. Every packet is moved
        /// out or released. An empty span is valid.
        void process(std::span<net::Datagram<Packet>> segments, core::TimePoint now) noexcept;
        /// Removes one pending notification and returns its snapshot; nothing when none is pending.
        [[nodiscard]] std::optional<EventType> poll_event() noexcept;
        /// Sends the ordinary ACKs and window updates that waited, one pure ACK per connection.
        void flush(core::TimePoint now) noexcept;

        [[nodiscard]] std::expected<void, ListenError> listen(std::uint16_t port) noexcept;
        /// Connections already open stay open.
        void unlisten(std::uint16_t port) noexcept;
        /// A SynSent connection the caller owns; its SYN is queued, or waits for ARP on the timer.
        [[nodiscard]] std::expected<ConnectionType*, ConnectError> connect(Endpoint peer, core::TimePoint now) noexcept;

        [[nodiscard]] ConnectionType& connection(const std::uint32_t index) noexcept { return connections_[index]; }
        [[nodiscard]] const ConnectionType& connection(const std::uint32_t index) const noexcept { return connections_[index]; }
        [[nodiscard]] std::size_t capacity() const noexcept { return connections_.size(); }
        /// Ours: `max_l4_size() - 20`.
        [[nodiscard]] std::uint16_t mss() const noexcept { return mss_; }
        /// `min(receive_segments * mss, 65535)`: the receive credit a connection starts with.
        [[nodiscard]] std::uint32_t byte_budget() const noexcept { return byte_budget_; }
        [[nodiscard]] const TcpCounters& counters() const noexcept { return counters_; }
        [[nodiscard]] const TcpConfig& config() const noexcept { return config_; }
        [[nodiscard]] bool listening(std::uint16_t port) const noexcept;
        [[nodiscard]] std::size_t pending_events() const noexcept { return events_.size(); }
        [[nodiscard]] core::TimePoint now() const noexcept { return now_; }
        [[nodiscard]] std::size_t nodes_available() const noexcept { return pool_.available(); }
        [[nodiscard]] std::size_t table_size() const noexcept { return table_.size(); }

    private:
        friend class Connection<Ip>;

        using Datagram  = net::Datagram<Packet>;
        using EventList = detail::ConnectionList<ConnectionType, &ConnectionType::event_link_>;
        using AckList   = detail::ConnectionList<ConnectionType, &ConnectionType::ack_link_>;
        using RetryList = detail::ConnectionList<ConnectionType, &ConnectionType::retry_link_>;

        /// What the receive path parsed from one datagram.
        struct Segment {
            wire::TcpHeader header{};
            std::uint16_t payload_offset = 0;  ///< From the start of `l4()`: the data offset.
            std::uint16_t payload_length = 0;
            std::optional<std::uint16_t> mss;  ///< From the options of a SYN.
            std::uint32_t base_hash = 0;

            /// Sequence space the segment occupies: payload, SYN and FIN.
            [[nodiscard]] std::uint32_t length() const noexcept {
                return payload_length + (header.flags.has(wire::TcpFlag::Syn) ? 1U : 0U) +
                       (header.flags.has(wire::TcpFlag::Fin) ? 1U : 0U);
            }
        };

        // tcp_receive.hpp
        void on_datagram(Datagram& datagram) noexcept;
        [[nodiscard]] bool checksum_ok(Datagram& datagram) const noexcept;
        [[nodiscard]] std::uint32_t base_hash_of(const Datagram& datagram, const Segment& segment, FlowKey key) const noexcept;
        void passive_open(Datagram& datagram, const Segment& segment, FlowKey key) noexcept;
        void receive_syn_sent(ConnectionType& c, Datagram& datagram, Segment segment) noexcept;
        void receive_synchronized(ConnectionType& c, Datagram& datagram, Segment segment) noexcept;
        [[nodiscard]] bool accept_payload(ConnectionType& c, Datagram& datagram, const Segment& segment) noexcept;
        void apply_ack_policy(ConnectionType& c, std::size_t accepted) noexcept;
        void receive_fin(ConnectionType& c) noexcept;
        void update_peer_window(ConnectionType& c, const wire::TcpHeader& header) noexcept;
        void reply_reset(const Datagram& datagram, const Segment& segment) noexcept;

        // tcp_transmit.hpp
        [[nodiscard]] std::expected<void, net::SendError>
        send_segment(ConnectionType& c, Packet&& packet, Sequence sequence, wire::TcpFlags flags, bool with_mss) noexcept;
        [[nodiscard]] std::expected<void, net::SendError>
        send_control(ConnectionType& c, wire::TcpFlags flags, Sequence sequence) noexcept;
        [[nodiscard]] bool send_pure_ack(ConnectionType& c) noexcept;
        void queue_ack(ConnectionType& c) noexcept;
        void ack_now(ConnectionType& c) noexcept;
        void mark_retry(ConnectionType& c) noexcept;
        [[nodiscard]] std::size_t writable(const ConnectionType& c) const noexcept;
        [[nodiscard]] std::optional<std::span<std::byte>> prepare(ConnectionType& c, std::size_t count) noexcept;  // Task 8
        [[nodiscard]] bool commit(ConnectionType& c, std::size_t count) noexcept;                                  // Task 8
        void consume(ConnectionType& c, std::size_t count) noexcept;                                               // Task 7

        // tcp_control.hpp
        void raise(ConnectionType& c, stream::Event event) noexcept;
        [[nodiscard]] std::optional<std::uint32_t> take_slot() noexcept;
        void free_slot(ConnectionType& c) noexcept;
        void return_chain(ConnectionType& c) noexcept;
        [[nodiscard]] Sequence draw_isn() noexcept;
        [[nodiscard]] std::expected<std::uint16_t, ConnectError> choose_port(Endpoint peer, std::uint32_t& base_hash) noexcept;
        void arm_retry(ConnectionType& c) noexcept;
        void on_timer(ConnectionType& c) noexcept;
        [[nodiscard]] std::expected<void, net::SendError> resend_control(ConnectionType& c) noexcept;
        void time_out(ConnectionType& c) noexcept;
        void set_closed(ConnectionType& c) noexcept;
        void finish(ConnectionType& c) noexcept;
        void maybe_finish(ConnectionType& c) noexcept;
        void close(ConnectionType& c) noexcept;  // Task 9
        void abort(ConnectionType& c) noexcept;
        void release(ConnectionType& c) noexcept;

        [[nodiscard]] static TcpConfig validated(const TcpConfig& config, std::uint16_t max_l4_size);

        Ip* ip_;
        loop::TimerWheel* wheel_;
        TcpConfig config_;
        std::uint16_t mss_;
        std::uint32_t byte_budget_;
        detail::TcpNodePool<Packet> pool_;
        FlowTable table_;
        std::deque<ConnectionType> connections_;
        std::vector<std::uint16_t> listeners_;
        std::vector<std::uint32_t> free_;  ///< A stack of free slot indices.
        EventList events_;
        AckList acks_;
        RetryList retries_;
        std::uint16_t cursor_;
        std::mt19937 isn_engine_;
        core::TimePoint now_{};
        TcpCounters counters_{};
        bool hardware_hash_ = false;  ///< The card hashes IPv4 TCP tuples: its hash is the table's base hash.
    };

    template <typename Ip>
    TcpConfig Stack<Ip>::validated(const TcpConfig& config, const std::uint16_t max_l4_size) {
        if (config.connections == 0 || config.listeners == 0 || config.receive_segments == 0 || config.receive_pool == 0) {
            throw std::invalid_argument{"TcpConfig: connections, listeners, receive_segments and receive_pool must be positive"};
        }
        if (config.connections > (std::size_t{1} << 30U)) {
            throw std::invalid_argument{"TcpConfig: too many connections for a 32-bit index"};
        }
        if (config.ephemeral_first == 0 || config.ephemeral_first > config.ephemeral_last) {
            throw std::invalid_argument{"TcpConfig: the ephemeral range must be non-empty and exclude port 0"};
        }
        if (config.retry_initial <= core::Duration::zero() || config.unresolved_retry <= core::Duration::zero()) {
            throw std::invalid_argument{"TcpConfig: retry_initial and unresolved_retry must be positive"};
        }
        if (config.retries == 0) {
            throw std::invalid_argument{"TcpConfig: retries must be positive"};
        }
        if (max_l4_size <= wire::TcpHeader::size) {
            throw std::invalid_argument{"TcpConfig: the MTU leaves no room for a TCP segment"};
        }
        return config;
    }

    template <typename Ip>
    Stack<Ip>::Stack(Ip& ip, loop::TimerWheel& wheel, const TcpConfig& config)
        : ip_{&ip},
          wheel_{&wheel},
          config_{validated(config, ip.max_l4_size())},
          mss_{static_cast<std::uint16_t>(ip.max_l4_size() - wire::TcpHeader::size)},
          byte_budget_{static_cast<std::uint32_t>(std::min<std::uint64_t>(
              std::uint64_t{config_.receive_segments} * mss_, std::numeric_limits<std::uint16_t>::max()))},
          pool_{config_.receive_pool},
          table_{config_.connections},
          cursor_{config_.ephemeral_first},
          isn_engine_{std::random_device{}()},
          hardware_hash_{ip.queue().steering().enabled && !ip.queue().steering().table.empty() &&
                         ip.queue().steering().types.ipv4_tcp} {
        listeners_.reserve(config_.listeners);
        free_.reserve(config_.connections);
        for (std::size_t index = 0; index < config_.connections; ++index) {
            ConnectionType& slot = connections_.emplace_back(typename ConnectionType::PrivateTag{});
            slot.stack_          = this;
            slot.index_          = static_cast<std::uint32_t>(index);
        }
        for (std::size_t index = config_.connections; index > 0; --index) {
            free_.push_back(static_cast<std::uint32_t>(index - 1));  // slot 0 is taken first
        }
    }

    template <typename Ip>
    Stack<Ip>::~Stack() {
        for (ConnectionType& c : connections_) {
            wheel_->cancel(c.timer_);
            c.prepared_.reset();
            return_chain(c);
        }
    }

    template <typename Ip>
    bool Stack<Ip>::listening(const std::uint16_t port) const noexcept {
        return std::ranges::find(listeners_, port) != listeners_.end();
    }

    template <typename Ip>
    std::expected<void, ListenError> Stack<Ip>::listen(const std::uint16_t port) noexcept {
        if (listening(port)) {
            return std::unexpected{ListenError::InUse};
        }
        if (listeners_.size() == config_.listeners) {
            return std::unexpected{ListenError::TableFull};
        }
        listeners_.push_back(port);  // within the reserved capacity: no allocation
        return {};
    }

    template <typename Ip>
    void Stack<Ip>::unlisten(const std::uint16_t port) noexcept {
        const auto found = std::ranges::find(listeners_, port);
        if (found != listeners_.end()) {
            *found = listeners_.back();
            listeners_.pop_back();
        }
    }

    template <typename Ip>
    void Stack<Ip>::process(const std::span<net::Datagram<Packet>> segments, const core::TimePoint now) noexcept {
        now_ = now;
        for (ConnectionType* c = retries_.pop_front(); c != nullptr; c = retries_.pop_front()) {
            raise(*c, stream::Event::Writable);  // the deferred retry hint, at most one per marked connection
        }
        for (Datagram& datagram : segments) {
            if (datagram.packet.empty()) {
                continue;
            }
            on_datagram(datagram);
            if (!datagram.packet.empty()) {
                datagram.packet = Packet{};  // not held: back to the pool now, not at IP's next process
            }
        }
    }

    template <typename Ip>
    std::optional<ConnectionEvent<Ip>> Stack<Ip>::poll_event() noexcept {
        ConnectionType* c = events_.pop_front();
        if (c == nullptr) {
            return std::nullopt;
        }
        const stream::Events snapshot = c->events_;
        c->events_                    = {};
        return EventType{.connection = c, .events = snapshot};
    }

    template <typename Ip>
    void Stack<Ip>::flush(const core::TimePoint now) noexcept {
        now_ = now;
        for (ConnectionType* c = acks_.front(); c != nullptr;) {
            ConnectionType* next = AckList::next_of(*c);  // a successful send unlinks c
            if (send_pure_ack(*c)) {
                maybe_finish(*c);
            }
            c = next;
        }
    }

    template <typename Ip>
    std::expected<Connection<Ip>*, ConnectError> Stack<Ip>::connect(const Endpoint peer, const core::TimePoint now) noexcept {
        now_ = now;
        if (!ip_->next_hop(peer.address)) {
            return std::unexpected{ConnectError::NoRoute};
        }
        std::uint32_t base_hash = 0;
        const auto port         = choose_port(peer, base_hash);
        if (!port) {
            return std::unexpected{port.error()};
        }
        const auto slot = take_slot();
        if (!slot) {
            return std::unexpected{ConnectError::TableFull};
        }
        ConnectionType& c = connections_[*slot];
        c.local_          = Endpoint{.address = ip_->address(), .port = *port};
        c.remote_         = peer;
        c.key_            = FlowKey{.remote = peer.address, .remote_port = peer.port, .local_port = *port};
        c.base_hash_      = base_hash;
        [[maybe_unused]] const bool inserted = table_.insert(c.key_, *slot, base_hash);
        assert(inserted && "choose_port found the tuple free");
        c.indexed_ = true;
        c.state_   = State::SynSent;
        c.owned_   = true;
        c.iss_     = draw_isn();
        c.snd_nxt_ = c.iss_ + 1;
        c.snd_una_ = c.iss_;
        const auto sent = send_control(c, wire::TcpFlags{wire::TcpFlag::Syn}, c.iss_);
        if (sent) {
            ++counters_.control_segments_sent;
        }
        c.tries_           = 0;
        c.unresolved_wait_ = !sent && sent.error() == net::SendError::Unresolved;
        arm_retry(c);
        return &c;
    }

}  // namespace aloe::tcp

#include <tcp_control.hpp>
#include <tcp_receive.hpp>
#include <tcp_transmit.hpp>
```

- [ ] **Step 5: Write the control, receive and transmit implementation headers**

Each includes `<tcp_stack.hpp>` so it compiles on its own; `tcp_stack.hpp` includes all three at its end, after the class, and `#pragma once` breaks the cycle.

`common/tcp/stack/tcp_control.hpp`:

```cpp
#pragma once

#include <cassert>
#include <cstdint>
#include <expected>
#include <optional>
#include <tuple>

#include <rss.hpp>
#include <tcp_stack.hpp>

// The control path of tcp::Stack: events, slots, the port choice, the retransmit timer, abort and release.
namespace aloe::tcp {

    template <typename Ip>
    void Stack<Ip>::raise(ConnectionType& c, const stream::Event event) noexcept {
        c.events_ |= event;
        if (!events_.contains(c)) {
            events_.push_back(c);
        }
    }

    template <typename Ip>
    std::optional<std::uint32_t> Stack<Ip>::take_slot() noexcept {
        if (free_.empty()) {
            return std::nullopt;
        }
        const std::uint32_t index = free_.back();
        free_.pop_back();
        return index;
    }

    template <typename Ip>
    void Stack<Ip>::return_chain(ConnectionType& c) noexcept {
        for (std::uint32_t index = c.chain_head_; index != detail::no_node;) {
            const std::uint32_t next = pool_.node(index).next;
            pool_.release(index);
            index = next;
        }
        c.chain_head_   = detail::no_node;
        c.chain_tail_   = detail::no_node;
        c.chain_count_  = 0;
        c.unread_bytes_ = 0;
    }

    template <typename Ip>
    void Stack<Ip>::free_slot(ConnectionType& c) noexcept {
        wheel_->cancel(c.timer_);
        if (c.indexed_) {
            std::ignore = table_.erase(c.key_, c.base_hash_);
        }
        return_chain(c);
        c.prepared_.reset();
        events_.remove(c);
        acks_.remove(c);
        retries_.remove(c);
        c.clear();
        free_.push_back(c.index_);
    }

    template <typename Ip>
    wire::TcpSequence Stack<Ip>::draw_isn() noexcept {
        return Sequence{static_cast<std::uint32_t>(isn_engine_())};
    }

    template <typename Ip>
    std::expected<std::uint16_t, ConnectError> Stack<Ip>::choose_port(const Endpoint peer, std::uint32_t& base_hash) noexcept {
        const device::RssDescription& steering = ip_->queue().steering();
        const std::uint16_t mine               = ip_->queue().index();
        device::FlowTuple tuple{.source           = peer.address,  // received orientation: the peer is the source
                                .destination      = ip_->address(),
                                .source_port      = peer.port,
                                .destination_port = 0,
                                .protocol         = wire::Ipv4Protocol::Tcp};
        if (!hardware_hash_) {
            // Steering off, or addresses only: the queue the replies land on does not depend on the port.
            if (device::queue_for(steering, tuple) != mine) {
                return std::unexpected{ConnectError::Unplaceable};
            }
        }
        const std::uint32_t range = static_cast<std::uint32_t>(config_.ephemeral_last - config_.ephemeral_first) + 1U;
        for (std::uint32_t tried = 0; tried < range; ++tried) {
            const std::uint16_t port = cursor_;
            cursor_ = cursor_ == config_.ephemeral_last ? config_.ephemeral_first : static_cast<std::uint16_t>(cursor_ + 1);
            if (listening(port)) {
                continue;
            }
            tuple.destination_port = port;
            if (hardware_hash_ && device::queue_for(steering, tuple) != mine) {
                continue;
            }
            const FlowKey key{.remote = peer.address, .remote_port = peer.port, .local_port = port};
            const std::uint32_t hash = hardware_hash_ ? device::flow_hash(steering, tuple) : detail::software_flow_hash(key);
            if (table_.find(key, hash)) {
                continue;
            }
            base_hash = hash;
            return port;
        }
        return std::unexpected{ConnectError::NoPort};
    }

    template <typename Ip>
    void Stack<Ip>::arm_retry(ConnectionType& c) noexcept {
        const core::Duration delay =
            c.unresolved_wait_ ? config_.unresolved_retry : config_.retry_initial * (std::int64_t{1} << c.tries_);
        wheel_->arm(c.timer_, now_ + delay);
    }

    template <typename Ip>
    std::expected<void, net::SendError> Stack<Ip>::resend_control(ConnectionType& c) noexcept {
        switch (c.state_) {
            case State::SynSent:
                return send_control(c, wire::TcpFlags{wire::TcpFlag::Syn}, c.iss_);
            case State::SynReceived:
                return send_control(c, wire::TcpFlags{wire::TcpFlag::Syn, wire::TcpFlag::Ack}, c.iss_);
            case State::FinWait1:
            case State::Closing:
            case State::LastAck:
                return send_control(c, wire::TcpFlags{wire::TcpFlag::Fin, wire::TcpFlag::Ack}, c.fin_sequence_);
            default:
                return {};
        }
    }

    /// Called by the wheel with the timer unarmed: a try, or a poll while the next hop is unresolved.
    template <typename Ip>
    void Stack<Ip>::on_timer(ConnectionType& c) noexcept {
        switch (c.state_) {
            case State::SynSent:
            case State::SynReceived:
            case State::FinWait1:
            case State::Closing:
            case State::LastAck:
                break;
            default:
                return;  // nothing to retransmit in this state
        }
        if (!c.unresolved_wait_) {
            if (c.tries_ >= config_.retries) {
                time_out(c);
                return;
            }
            ++c.tries_;
            ++counters_.retransmits;
        }
        const auto sent = resend_control(c);
        if (sent) {
            ++counters_.control_segments_sent;
        }
        c.unresolved_wait_ = !sent && sent.error() == net::SendError::Unresolved;
        arm_retry(c);
    }

    template <typename Ip>
    void Stack<Ip>::time_out(ConnectionType& c) noexcept {
        if (!c.owned_) {
            ++counters_.handshakes_failed;  // a passive open nobody took: freed silently
            free_slot(c);
            return;
        }
        ++counters_.connections_timed_out;
        set_closed(c);
        raise(c, stream::Event::TimedOut);
    }

    /// Closed, with nothing armed, pending or prepared. Held segments stay until release.
    template <typename Ip>
    void Stack<Ip>::set_closed(ConnectionType& c) noexcept {
        c.state_ = State::Closed;
        wheel_->cancel(c.timer_);
        c.unresolved_wait_ = false;
        acks_.remove(c);
        c.ack_pending_    = false;
        c.finish_pending_ = false;
        retries_.remove(c);
        c.prepared_.reset();
        c.prepared_size_ = 0;
    }

    template <typename Ip>
    void Stack<Ip>::finish(ConnectionType& c) noexcept {
        set_closed(c);
        ++counters_.connections_closed;
        raise(c, stream::Event::Closed);
    }

    /// The last ACK the peer is owed must be queued before Closed is published; flush retries the rest.
    template <typename Ip>
    void Stack<Ip>::maybe_finish(ConnectionType& c) noexcept {
        if (c.finish_pending_ && !c.ack_pending_) {
            finish(c);
        }
    }

    template <typename Ip>
    void Stack<Ip>::abort(ConnectionType& c) noexcept {
        if (c.state_ == State::Closed) {
            return;
        }
        c.prepared_.reset();
        c.prepared_size_ = 0;
        if (send_control(c, wire::TcpFlags{wire::TcpFlag::Rst, wire::TcpFlag::Ack}, c.snd_nxt_)) {
            ++counters_.resets_sent;
        }
        set_closed(c);
        raise(c, stream::Event::Closed);
    }

    template <typename Ip>
    void Stack<Ip>::release(ConnectionType& c) noexcept {
        if (c.state_ != State::Closed) {
            abort(c);
        }
        free_slot(c);
    }

}  // namespace aloe::tcp
```

`common/tcp/stack/tcp_receive.hpp`:

```cpp
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include <checksum.hpp>
#include <ipv4_checksum.hpp>
#include <rss.hpp>
#include <tcp_stack.hpp>

// The receive path of tcp::Stack: header, checksum, lookup, the handshake, the synchronized states.
namespace aloe::tcp {

    template <typename Ip>
    void Stack<Ip>::on_datagram(Datagram& datagram) noexcept {
        ++counters_.segments_received;
        const std::span<std::byte> l4               = datagram.l4();
        const std::optional<wire::TcpHeader> header = wire::TcpHeader::parse(l4);
        if (!header) {
            ++counters_.dropped_bad_header;
            return;
        }
        Segment segment{.header         = *header,
                        .payload_offset = header->data_offset,
                        .payload_length = static_cast<std::uint16_t>(l4.size() - header->data_offset)};
        if (header->flags.has(wire::TcpFlag::Syn)) {
            const auto options = wire::TcpOptions::parse(
                std::span<const std::byte>{l4}.subspan(wire::TcpHeader::size, header->data_offset - wire::TcpHeader::size));
            if (!options) {
                ++counters_.dropped_bad_header;
                return;
            }
            segment.mss = options->mss;
        }
        if (!checksum_ok(datagram)) {
            ++counters_.dropped_bad_checksum;
            return;
        }
        const FlowKey key{.remote = datagram.source, .remote_port = header->source_port, .local_port = header->destination_port};
        segment.base_hash                        = base_hash_of(datagram, segment, key);
        const std::optional<std::uint32_t> found = table_.find(key, segment.base_hash);
        if (!found) {
            const bool opening = header->flags.has(wire::TcpFlag::Syn) && !header->flags.has(wire::TcpFlag::Ack) &&
                                 !header->flags.has(wire::TcpFlag::Rst);
            if (opening && listening(header->destination_port)) {
                passive_open(datagram, segment, key);
                return;
            }
            ++counters_.dropped_no_connection;
            if (!header->flags.has(wire::TcpFlag::Rst)) {
                reply_reset(datagram, segment);
            }
            return;
        }
        ConnectionType& c = connections_[*found];
        switch (c.state_) {
            case State::Closed:  // still indexed because not released
                ++counters_.dropped_closed;
                if (!header->flags.has(wire::TcpFlag::Rst)) {
                    reply_reset(datagram, segment);
                }
                return;
            case State::SynSent:
                receive_syn_sent(c, datagram, segment);
                return;
            default:
                receive_synchronized(c, datagram, segment);
                return;
        }
    }

    template <typename Ip>
    bool Stack<Ip>::checksum_ok(Datagram& datagram) const noexcept {
        switch (datagram.l4_checksum) {
            case device::ChecksumVerdict::Good:
                return true;
            case device::ChecksumVerdict::Bad:
                return false;
            default:
                break;
        }
        const std::span<const std::byte> l4 = datagram.l4();
        const std::uint32_t pseudo          = wire::ipv4_pseudo_header_sum(
            datagram.source, datagram.destination, wire::Ipv4Protocol::Tcp, static_cast<std::uint16_t>(l4.size()));
        return wire::checksum_finish(wire::checksum_add(pseudo, l4)) == 0;
    }

    template <typename Ip>
    std::uint32_t Stack<Ip>::base_hash_of(const Datagram& datagram, const Segment& segment, const FlowKey key) const noexcept {
        if (!hardware_hash_) {
            return detail::software_flow_hash(key);
        }
        if (const std::optional<std::uint32_t> hash = datagram.packet.rx().rss_hash) {
            return *hash;
        }
        return device::flow_hash(ip_->queue().steering(),
                                 device::FlowTuple{.source           = datagram.source,
                                                   .destination      = datagram.destination,
                                                   .source_port      = segment.header.source_port,
                                                   .destination_port = segment.header.destination_port,
                                                   .protocol         = wire::Ipv4Protocol::Tcp});
    }

    template <typename Ip>
    void Stack<Ip>::passive_open(Datagram& datagram, const Segment& segment, const FlowKey key) noexcept {
        const std::optional<std::uint32_t> slot = take_slot();
        if (!slot) {
            ++counters_.dropped_table_full;
            return;
        }
        ConnectionType& c = connections_[*slot];
        c.local_          = Endpoint{.address = ip_->address(), .port = segment.header.destination_port};
        c.remote_         = Endpoint{.address = datagram.source, .port = segment.header.source_port};
        c.key_            = key;
        c.base_hash_      = segment.base_hash;
        [[maybe_unused]] const bool inserted = table_.insert(key, *slot, segment.base_hash);
        assert(inserted && "the lookup just missed");
        c.indexed_  = true;
        c.state_    = State::SynReceived;
        c.owned_    = false;
        c.rcv_nxt_  = segment.header.sequence + 1;
        c.rcv_adv_  = c.rcv_nxt_ + byte_budget_;  // the initial offer, carried by the SYN-ACK
        c.peer_mss_ = segment.mss.value_or(default_peer_mss);
        c.snd_wnd_  = segment.header.window;
        c.wl1_      = segment.header.sequence;
        c.wl2_      = Sequence{};
        c.iss_      = draw_isn();
        c.snd_nxt_  = c.iss_ + 1;
        c.snd_una_  = c.iss_;
        const auto sent = send_control(c, wire::TcpFlags{wire::TcpFlag::Syn, wire::TcpFlag::Ack}, c.iss_);
        if (sent) {
            ++counters_.control_segments_sent;
        }
        c.tries_           = 0;
        c.unresolved_wait_ = !sent && sent.error() == net::SendError::Unresolved;
        arm_retry(c);
    }

    template <typename Ip>
    void Stack<Ip>::receive_syn_sent(ConnectionType& c, Datagram& datagram, Segment segment) noexcept {
        const wire::TcpHeader& header = segment.header;
        if (header.flags.has(wire::TcpFlag::Ack) && header.acknowledgement != c.snd_nxt_) {
            ++counters_.dropped_unexpected;  // not an answer to our SYN
            return;
        }
        if (header.flags.has(wire::TcpFlag::Rst)) {
            if (!header.flags.has(wire::TcpFlag::Ack)) {
                ++counters_.dropped_unexpected;
                return;
            }
            ++counters_.connections_reset;
            set_closed(c);
            raise(c, stream::Event::Reset);
            return;
        }
        if (!header.flags.has(wire::TcpFlag::Syn) || !header.flags.has(wire::TcpFlag::Ack)) {
            ++counters_.dropped_unexpected;  // a simultaneous open is out of scope
            return;
        }
        c.snd_una_  = header.acknowledgement;
        c.rcv_nxt_  = header.sequence + 1;
        c.rcv_adv_  = c.rcv_nxt_ + byte_budget_;  // the initial offer, carried by our ACK or the first data segment
        c.peer_mss_ = segment.mss.value_or(default_peer_mss);
        c.snd_wnd_  = header.window;
        c.wl1_      = header.sequence;
        c.wl2_      = header.acknowledgement;
        c.state_    = State::Established;
        wheel_->cancel(c.timer_);
        c.tries_           = 0;
        c.unresolved_wait_ = false;
        ++counters_.connections_opened;
        queue_ack(c);  // deferred: a commit on the Connected event completes the handshake with data
        raise(c, stream::Event::Connected);
        if (segment.payload_length > 0 || header.flags.has(wire::TcpFlag::Fin)) {
            segment.header.sequence = c.rcv_nxt_;
            segment.header.flags    = wire::TcpFlags::from_raw(
                static_cast<std::uint8_t>(header.flags.raw() & ~std::to_underlying(wire::TcpFlag::Syn)));
            receive_synchronized(c, datagram, segment);
        }
    }

    template <typename Ip>
    void Stack<Ip>::receive_synchronized(ConnectionType& c, Datagram& datagram, Segment segment) noexcept {
        wire::TcpHeader& header = segment.header;
        const bool rst          = header.flags.has(wire::TcpFlag::Rst);

        // 1. Sequence check against [rcv_nxt, rcv_adv): trim an old prefix, drop the rest with an immediate ACK.
        const std::uint32_t window = c.rcv_adv_.value - c.rcv_nxt_.value;
        const std::uint32_t length = segment.length();
        Sequence sequence          = header.sequence;
        const Sequence end         = sequence + length;
        if (length == 0) {
            const bool acceptable = window == 0 ? sequence == c.rcv_nxt_
                                                : !sequence.before(c.rcv_nxt_) && sequence.before(c.rcv_adv_);
            if (!acceptable) {
                ++counters_.dropped_out_of_window;
                if (!rst) {
                    ack_now(c);
                }
                return;
            }
        } else {
            if (!end.after(c.rcv_nxt_)) {  // entirely before rcv_nxt
                if (c.state_ == State::SynReceived && header.flags.has(wire::TcpFlag::Syn) &&
                    !header.flags.has(wire::TcpFlag::Ack)) {
                    if (send_control(c, wire::TcpFlags{wire::TcpFlag::Syn, wire::TcpFlag::Ack}, c.iss_)) {
                        ++counters_.control_segments_sent;  // a duplicate SYN: the SYN-ACK again
                    }
                    return;
                }
                ++counters_.dropped_duplicate;
                if (!rst) {
                    ack_now(c);
                }
                return;
            }
            if (sequence.after(c.rcv_nxt_)) {  // a gap before it
                ++counters_.dropped_out_of_order;
                if (!c.recovery_end_ || end.after(*c.recovery_end_)) {
                    c.recovery_end_ = end;
                }
                if (!rst) {
                    ack_now(c);
                }
                return;
            }
            if (sequence.before(c.rcv_nxt_)) {  // overlap: the old prefix is already ours
                std::uint32_t trim = c.rcv_nxt_.value - sequence.value;
                if (header.flags.has(wire::TcpFlag::Syn)) {
                    header.flags = wire::TcpFlags::from_raw(
                        static_cast<std::uint8_t>(header.flags.raw() & ~std::to_underlying(wire::TcpFlag::Syn)));
                    --trim;
                }
                assert(trim <= segment.payload_length);
                segment.payload_offset = static_cast<std::uint16_t>(segment.payload_offset + trim);
                segment.payload_length = static_cast<std::uint16_t>(segment.payload_length - trim);
                sequence               = c.rcv_nxt_;
                header.sequence        = sequence;
            }
            if (window == 0 || end.after(c.rcv_adv_)) {
                ++counters_.dropped_out_of_window;
                if (!rst) {
                    ack_now(c);
                }
                return;
            }
        }

        // 2. RST.
        if (rst) {
            if (c.state_ == State::SynReceived && !c.owned_) {
                ++counters_.handshakes_failed;
                free_slot(c);
                return;
            }
            ++counters_.connections_reset;
            set_closed(c);
            raise(c, stream::Event::Reset);
            return;
        }
        if (header.flags.has(wire::TcpFlag::Syn)) {
            ++counters_.dropped_unexpected;  // a SYN inside the window of a synchronized connection
            return;
        }

        // 3. ACK.
        if (!header.flags.has(wire::TcpFlag::Ack)) {
            ++counters_.dropped_unexpected;
            return;
        }
        const Sequence ack = header.acknowledgement;
        if (c.state_ == State::SynReceived) {
            if (ack != c.snd_nxt_) {
                ++counters_.dropped_unexpected;
                return;
            }
            c.snd_una_ = ack;
            c.state_   = State::Established;
            c.owned_   = true;
            wheel_->cancel(c.timer_);
            c.tries_           = 0;
            c.unresolved_wait_ = false;
            ++counters_.connections_accepted;
            raise(c, stream::Event::Accepted);
            // The same segment may carry data or a FIN: keep going.
        }
        if (ack.after(c.snd_nxt_)) {
            ++counters_.dropped_unexpected;  // acknowledges what was never sent
            ack_now(c);
            return;
        }
        const std::size_t writable_before = writable(c);
        if (ack.after(c.snd_una_)) {
            c.snd_una_ = ack;
            raise(c, stream::Event::Acked);
            if (c.fin_sent_ && ack == c.snd_nxt_) {  // our FIN is acknowledged
                switch (c.state_) {
                    case State::FinWait1:
                        c.state_ = State::FinWait2;
                        wheel_->cancel(c.timer_);
                        break;
                    case State::Closing:
                    case State::LastAck:
                        wheel_->cancel(c.timer_);
                        c.finish_pending_ = true;
                        break;
                    default:
                        break;
                }
            }
        }
        update_peer_window(c, header);
        if (writable(c) > writable_before) {
            raise(c, stream::Event::Writable);
        }
        maybe_finish(c);
        if (c.state_ == State::Closed) {
            return;
        }

        // 4. Payload.
        if (segment.payload_length > 0) {
            const bool receiving = c.state_ == State::Established || c.state_ == State::FinWait1 || c.state_ == State::FinWait2;
            if (!receiving) {
                ++counters_.dropped_unexpected;  // data after the peer's FIN
                ack_now(c);
                return;
            }
            if (!accept_payload(c, datagram, segment)) {
                ack_now(c);  // reports the last accepted byte; the FIN behind refused data is not consumed
                return;
            }
            apply_ack_policy(c, segment.payload_length);
        }

        // 5. FIN, in order: its sequence is rcv_nxt after the payload.
        if (header.flags.has(wire::TcpFlag::Fin)) {
            receive_fin(c);
        }
    }

    template <typename Ip>
    bool Stack<Ip>::accept_payload(ConnectionType& c, Datagram& datagram, const Segment& segment) noexcept {
        if (c.chain_count_ >= config_.receive_segments) {
            ++counters_.dropped_no_slot;
            return false;
        }
        if (pool_.available() == 0) {
            ++counters_.dropped_no_node;
            return false;
        }
        const auto offset = static_cast<std::uint16_t>(datagram.l3_offset + datagram.l3_length + segment.payload_offset);
        const std::optional<std::uint32_t> node = pool_.acquire(std::move(datagram.packet), offset, segment.payload_length);
        assert(node.has_value() && "a node was available");
        if (c.chain_tail_ == detail::no_node) {
            c.chain_head_ = *node;
        } else {
            pool_.node(c.chain_tail_).next = *node;
        }
        c.chain_tail_ = *node;
        ++c.chain_count_;
        c.rcv_nxt_       = c.rcv_nxt_ + segment.payload_length;
        c.unread_bytes_ += segment.payload_length;
        raise(c, stream::Event::Readable);
        return true;
    }

    /// RFC 5681: ordinary in-order data waits for flush; recovery, and every second full-sized segment, do not.
    template <typename Ip>
    void Stack<Ip>::apply_ack_policy(ConnectionType& c, const std::size_t accepted) noexcept {
        if (c.recovery_end_) {
            ack_now(c);
            if (!c.rcv_nxt_.before(*c.recovery_end_)) {
                c.recovery_end_.reset();
            }
            return;
        }
        if (accepted >= mss_) {
            if (c.full_segments_since_ack_ < 2) {
                ++c.full_segments_since_ack_;
            }
            if (c.full_segments_since_ack_ >= 2) {
                ack_now(c);  // a success resets the count; a refusal leaves it saturated at two
                return;
            }
        }
        queue_ack(c);
    }

    template <typename Ip>
    void Stack<Ip>::receive_fin(ConnectionType& c) noexcept {
        c.rcv_nxt_     = c.rcv_nxt_ + 1;
        c.peer_closed_ = true;
        raise(c, stream::Event::PeerClosed);
        switch (c.state_) {
            case State::Established:
                c.state_ = State::CloseWait;
                queue_ack(c);  // the application's close may carry it
                break;
            case State::FinWait1:
                c.state_ = State::Closing;
                ack_now(c);
                break;
            case State::FinWait2:
                c.finish_pending_ = true;
                ack_now(c);  // the terminal ACK goes before Closed is published; flush retries a refusal
                maybe_finish(c);
                break;
            default:
                queue_ack(c);
                break;
        }
    }

    /// RFC 793's window update rule: a newer segment, or the same one with a newer acknowledgement.
    template <typename Ip>
    void Stack<Ip>::update_peer_window(ConnectionType& c, const wire::TcpHeader& header) noexcept {
        if (c.wl1_.before(header.sequence) || (c.wl1_ == header.sequence && !c.wl2_.after(header.acknowledgement))) {
            c.snd_wnd_ = header.window;
            c.wl1_     = header.sequence;
            c.wl2_     = header.acknowledgement;
        }
    }

    /// RFC 793's reset for a segment with no connection: with ACK, sequence at its acknowledgement;
    /// without, sequence zero and acknowledgement past everything it occupied.
    template <typename Ip>
    void Stack<Ip>::reply_reset(const Datagram& datagram, const Segment& segment) noexcept {
        std::optional<Packet> packet = ip_->allocate();
        if (!packet) {
            ++counters_.send_refused;
            return;
        }
        const std::optional<std::span<std::byte>> room = packet->append(wire::TcpHeader::size);
        if (!room) {
            ++counters_.send_refused;
            return;
        }
        wire::TcpHeader reply{.source_port = segment.header.destination_port, .destination_port = segment.header.source_port};
        if (segment.header.flags.has(wire::TcpFlag::Ack)) {
            reply.sequence = segment.header.acknowledgement;
            reply.flags    = wire::TcpFlags{wire::TcpFlag::Rst};
        } else {
            reply.acknowledgement = segment.header.sequence + segment.length();
            reply.flags           = wire::TcpFlags{wire::TcpFlag::Rst, wire::TcpFlag::Ack};
        }
        reply.write(*room);
        const auto sent = ip_->send(std::move(*packet),
                                    net::SendRequest{.destination = datagram.source,
                                                     .protocol    = wire::Ipv4Protocol::Tcp,
                                                     .checksum    = device::L4Checksum::Tcp},
                                    now_);
        if (sent) {
            ++counters_.resets_sent;
        } else if (sent.error() == net::SendError::Unresolved) {
            ++counters_.send_unresolved;
        } else {
            ++counters_.send_refused;
        }
        // On failure the packet is still in `packet` and goes back to the pool with it.
    }

}  // namespace aloe::tcp
```

`common/tcp/stack/tcp_transmit.hpp` (the application-facing `prepare`, `commit` and `consume` arrive in Tasks 7 and 8):

```cpp
#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>

#include <tcp_stack.hpp>

// The transmit path of tcp::Stack: every segment leaves through send_segment; pure ACKs and their policy.
namespace aloe::tcp {

    /**
     * Prepends the header (and the MSS option on a SYN) to `packet`, which holds the payload, and hands
     * it to IP. A segment with ACK offers the receive window: the candidate edge `rcv_nxt + (B - unread)`
     * when it is later than the current edge and a packet slot is free, else the current edge. Only a
     * successful send records the offer and clears the pending ACK; a refusal returns the packet to the
     * pool and leaves every number as it was.
     */
    template <typename Ip>
    std::expected<void, net::SendError> Stack<Ip>::send_segment(ConnectionType& c,
                                                                Packet&& packet,
                                                                const Sequence sequence,
                                                                const wire::TcpFlags flags,
                                                                const bool with_mss) noexcept {
        const std::size_t header_size = wire::TcpHeader::size + (with_mss ? wire::TcpOptions::mss_size : 0);
        const std::optional<std::span<std::byte>> room = packet.prepend(header_size);
        if (!room) {
            ++counters_.send_refused;  // not a packet from allocate(): no headroom
            packet = Packet{};
            return std::unexpected{net::SendError::Refused};
        }
        const bool with_ack = flags.has(wire::TcpFlag::Ack);
        Sequence offer      = c.rcv_adv_;
        bool extends        = false;
        if (with_ack) {
            const Sequence candidate = c.rcv_nxt_ + (byte_budget_ - static_cast<std::uint32_t>(c.unread_bytes_));
            if (candidate.after(c.rcv_adv_) && c.chain_count_ < config_.receive_segments) {
                offer   = candidate;
                extends = true;
            }
        }
        const std::uint16_t window = with_ack ? static_cast<std::uint16_t>(offer.value - c.rcv_nxt_.value)
                                              : static_cast<std::uint16_t>(byte_budget_);
        wire::TcpHeader{.source_port      = c.local_.port,
                        .destination_port = c.remote_.port,
                        .sequence         = sequence,
                        .acknowledgement  = with_ack ? c.rcv_nxt_ : Sequence{},
                        .data_offset      = static_cast<std::uint8_t>(header_size),
                        .flags            = flags,
                        .window           = window}
            .write(*room);
        if (with_mss) {
            wire::TcpOptions::write_mss(room->subspan(wire::TcpHeader::size), mss_);
        }
        const auto sent = ip_->send(std::move(packet),
                                    net::SendRequest{.destination = c.remote_.address,
                                                     .protocol    = wire::Ipv4Protocol::Tcp,
                                                     .checksum    = device::L4Checksum::Tcp},
                                    now_);
        if (!sent) {
            packet = Packet{};  // NOLINT(bugprone-use-after-move): IP moves only on success; the packet is ours to drop
            if (sent.error() == net::SendError::Unresolved) {
                ++counters_.send_unresolved;
            } else {
                ++counters_.send_refused;
            }
            return sent;
        }
        if (with_ack) {
            c.rcv_adv_ = offer;
            if (extends) {
                ++counters_.window_updates;
            }
            c.ack_pending_ = false;
            acks_.remove(c);
            c.full_segments_since_ack_ = 0;
        }
        return {};
    }

    template <typename Ip>
    std::expected<void, net::SendError> Stack<Ip>::send_control(ConnectionType& c,
                                                                const wire::TcpFlags flags,
                                                                const Sequence sequence) noexcept {
        std::optional<Packet> packet = ip_->allocate();
        if (!packet) {
            ++counters_.send_refused;
            return std::unexpected{net::SendError::Refused};
        }
        return send_segment(c, std::move(*packet), sequence, flags, flags.has(wire::TcpFlag::Syn));
    }

    template <typename Ip>
    bool Stack<Ip>::send_pure_ack(ConnectionType& c) noexcept {
        if (send_control(c, wire::TcpFlags{wire::TcpFlag::Ack}, c.snd_nxt_)) {
            ++counters_.pure_acks_sent;
            return true;
        }
        return false;
    }

    /// An ordinary ACK: marked, sent by flush unless a data segment carries it first.
    template <typename Ip>
    void Stack<Ip>::queue_ack(ConnectionType& c) noexcept {
        c.ack_pending_ = true;
        if (!acks_.contains(c)) {
            acks_.push_back(c);
        }
    }

    /// An immediate ACK: queued through IP before the next segment is processed; a refusal leaves it pending.
    template <typename Ip>
    void Stack<Ip>::ack_now(ConnectionType& c) noexcept {
        if (!send_pure_ack(c)) {
            queue_ack(c);
        }
    }

    /// A Writable hint at the next process, at most one per marked connection.
    template <typename Ip>
    void Stack<Ip>::mark_retry(ConnectionType& c) noexcept {
        if (!retries_.contains(c)) {
            retries_.push_back(c);
        }
    }

    template <typename Ip>
    std::size_t Stack<Ip>::writable(const ConnectionType& c) const noexcept {
        if (c.prepared_ || (c.state_ != State::Established && c.state_ != State::CloseWait)) {
            return 0;
        }
        const std::uint32_t in_flight = c.snd_nxt_.value - c.snd_una_.value;
        const std::uint32_t usable    = c.snd_wnd_ > in_flight ? c.snd_wnd_ - in_flight : 0;  // no unsigned underflow
        return std::min<std::size_t>(c.mss(), usable);
    }

}  // namespace aloe::tcp
```

Add the six headers to `${TCP}.Stack` in `common/tcp/CMakeLists.txt`, and to the umbrella:

```cpp
#include <tcp_connection.hpp>
#include <tcp_fwd.hpp>
#include <tcp_stack.hpp>
```

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Tcp Aloe.Tests.Unit.Frames Aloe.Tests.Unit.Net && ctest --preset debug -R '^Aloe[.]Tests[.]Unit[.](Tcp|Frames|Net)$' --output-on-failure`
Expected: PASS. If a `TcpHandshake` case fails on the SYN-ACK's window, check that `rcv_adv_` is set before `send_control` in `passive_open`; if `ControlRetrySchedule` fires one step late, check `arm_retry` uses `now_` set by `advance`'s empty `process`, not the stamp of the last receive.

- [ ] **Step 7: Format and commit**

```bash
./scripts/check-format.sh
git add common/tcp tests/shared tests/unit_tests/common/tcp
git commit -m "feat(tcp): establish connections and retry control segments"
```

## Task 7: In-order receive, byte windows and consumption

**Files:**
- Modify: `common/tcp/stack/tcp_transmit.hpp` (adds `consume`), `tests/unit_tests/common/tcp/CMakeLists.txt`
- Create: `tests/unit_tests/common/tcp/test_tcp_data.cpp`, `tests/unit_tests/common/tcp/test_tcp_window.cpp`

**Interfaces:**
- Consumes: Task 6's receive dispatcher, which already chains in-order payload, trims overlap, drops at the slot and node caps without advancing `rcv_nxt`, and offers the window from `send_segment`.
- Produces: `void Stack<Ip>::consume(ConnectionType&, std::size_t) noexcept`, reached through `Connection::consume`: advances node offsets, frees emptied nodes, reduces `unread_bytes_`, and queues an ordinary window update when the offered edge can move.

- [ ] **Step 1: Write the failing tests**

`tests/unit_tests/common/tcp/test_tcp_data.cpp` (the send half arrives in Task 8):

```cpp
#include <aloe/frames>
#include <aloe/stream>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <span>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>

namespace {

    using namespace std::chrono_literals;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;
    using aloe::wire::TcpSequence;
    using TcpReceive = aloe::testing::TcpFixture;

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpReceive,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

    [[nodiscard]] std::vector<std::byte> text(const char* s) {
        std::vector<std::byte> out;
        for (; *s != '\0'; ++s) {
            out.push_back(static_cast<std::byte>(*s));
        }
        return out;
    }

    /// The unread bytes copied out: for assertions only; the brick itself never copies.
    template <typename C>
    [[nodiscard]] std::vector<std::byte> unread_of(const C& c) {
        std::vector<std::byte> out;
        for (const std::span<const std::byte> chunk : c.unread()) {
            out.insert(out.end(), chunk.begin(), chunk.end());
        }
        return out;
    }

    template <typename C>
    [[nodiscard]] std::vector<std::size_t> chunk_sizes(const C& c) {
        std::vector<std::size_t> out;
        for (const std::span<const std::byte> chunk : c.unread()) {
            out.push_back(chunk.size());
        }
        return out;
    }

}  // namespace

TEST_P(TcpReceive, ThreeSegmentsGiveThreeChunksAndConsumeCrossesBoundaries) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const std::size_t nodes = tcp_->nodes_available();
    receive(peer_.data(text("abc")));
    auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.readable());
    receive(peer_.data(text("defgh")));
    receive(peer_.data(text("ij")));
    EXPECT_EQ(tcp_->pending_events(), 1U) << "three Readable raises, one entry";
    EXPECT_EQ(c->unread().size(), 10U);
    EXPECT_EQ(chunk_sizes(*c), (std::vector<std::size_t>{3, 5, 2}));
    EXPECT_EQ(unread_of(*c), text("abcdefghij"));
    EXPECT_EQ(tcp_->nodes_available(), nodes - 3);
    EXPECT_TRUE(burst_empty()) << "the held packets were moved out of the burst";
    EXPECT_TRUE(collect_queued().empty()) << "ordinary in-order data waits for flush";

    c->consume(4);
    EXPECT_EQ(c->unread().size(), 6U);
    EXPECT_EQ(chunk_sizes(*c), (std::vector<std::size_t>{4, 2}));
    EXPECT_EQ(unread_of(*c), text("efghij"));
    EXPECT_EQ(tcp_->nodes_available(), nodes - 2) << "the first packet went back";
    c->consume(6);
    EXPECT_TRUE(c->unread().empty());
    EXPECT_EQ(tcp_->nodes_available(), nodes);

    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "one coalesced ACK for the three segments";
    EXPECT_EQ(frames[0].tcp->flags, TcpFlags{TcpFlag::Ack});
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 11});
    EXPECT_EQ(frames[0].tcp->window, aloe::testing::tcp_fabric_budget) << "everything consumed: the full budget";
    EXPECT_EQ(tcp_->counters().pure_acks_sent, 1U);
}

TEST_P(TcpReceive, FinalHandshakeAckCarriesData) {
    ASSERT_TRUE(tcp_->listen(aloe::testing::tcp_listen_port).has_value());
    receive(peer_.syn());
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    peer_.see(frames[0]);
    receive(peer_.data(text("abc")));  // the peer's ACK completing the handshake, with its first bytes
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.accepted());
    EXPECT_TRUE(event->events.readable());
    auto& c = *event->connection;
    EXPECT_EQ(c.state(), aloe::tcp::State::Established);
    EXPECT_EQ(c.unread().size(), 3U);
    EXPECT_EQ(unread_of(c), text("abc"));
    EXPECT_EQ(tcp_->counters().connections_accepted, 1U);
    EXPECT_EQ(tcp_->counters().dropped_out_of_window, 0U) << "the window was offered with the SYN-ACK";
}

TEST_P(TcpReceive, OverlapIsTrimmedByOffsetAndDuplicatesAckedAtOnce) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(text("abcdef")));
    // A retransmission starting three bytes back, carrying three new bytes.
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, text("defghi"), TcpSequence{aloe::testing::tcp_peer_isn + 4}, peer_.rcv_nxt()));
    EXPECT_EQ(c->unread().size(), 9U);
    EXPECT_EQ(unread_of(*c), text("abcdefghi"));
    EXPECT_EQ(chunk_sizes(*c), (std::vector<std::size_t>{6, 3})) << "the second chunk starts after the trimmed prefix";
    EXPECT_EQ(tcp_->counters().dropped_duplicate, 0U);
    EXPECT_TRUE(collect_queued().empty());
    // A fully duplicate segment: dropped, and acknowledged before the next segment is processed.
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, text("abc"), TcpSequence{aloe::testing::tcp_peer_isn + 1}, peer_.rcv_nxt()));
    EXPECT_EQ(tcp_->counters().dropped_duplicate, 1U);
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 10});
    EXPECT_EQ(c->unread().size(), 9U);
}

TEST_P(TcpReceive, ChecksumVerdicts) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    peer_.spec().checksums = aloe::frames::Checksums::Wrong;
    receive(peer_.data(text("bad")));
    peer_.rewind(3);
    EXPECT_EQ(tcp_->counters().dropped_bad_checksum, 1U) << (offloads() ? "the device's Bad verdict" : "the software check");
    EXPECT_TRUE(c->unread().empty());
    EXPECT_TRUE(collect().empty()) << "nothing is acknowledged";
    peer_.spec().checksums = aloe::frames::Checksums::Correct;
    receive(peer_.data(text("good")));
    EXPECT_EQ(c->unread().size(), 4U);
    EXPECT_EQ(tcp_->counters().dropped_bad_checksum, 1U);
}

TEST_P(TcpReceive, Ipv4OptionsShiftThePayload) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(aloe::frames::with_ipv4_options(peer_.data(text("options")), 2));
    EXPECT_EQ(c->unread().size(), 7U);
    EXPECT_EQ(unread_of(*c), text("options"));
}

TEST_P(TcpReceive, BadHeadersAndMalformedOptionsAreDroppedSilently) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    auto frame                 = peer_.data(text("x"));
    frame[14 + 20 + 12]        = std::byte{0x40};  // data offset 16: under the fixed header
    receive(frame);
    EXPECT_EQ(tcp_->counters().dropped_bad_header, 1U);
    peer_.rewind(1);
    aloe::frames::TcpPeer other{aloe::testing::peer_spec(40001), TcpSequence{2000U}};
    auto syn            = other.syn();
    syn[14 + 20 + 21]   = std::byte{0};  // the MSS option with length 0
    receive(syn);
    EXPECT_EQ(tcp_->counters().dropped_bad_header, 2U);
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(tcp_->table_size(), 1U);
    EXPECT_TRUE(c->unread().empty());
}

TEST_P(TcpReceive, SlotCapDropsWithoutAdvancingAndRecovers) {
    configure_tcp({.receive_segments = 2});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(text("a")));
    receive(peer_.data(text("b")));
    receive(peer_.data(text("c")));
    EXPECT_EQ(tcp_->counters().dropped_no_slot, 1U);
    EXPECT_EQ(c->unread().size(), 2U);
    auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U) << "a refused segment is acknowledged at once";
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 3}) << "the last accepted byte";
    EXPECT_EQ(frames[0].tcp->window, 2U * aloe::testing::tcp_fabric_mss - 2U) << "credit is not retracted to zero";
    receive(peer_.data(text("d")));  // a gap now: `c` was never accepted
    EXPECT_EQ(tcp_->counters().dropped_out_of_order, 1U);
    frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 3});
    c->consume(1);
    peer_.rewind(2);
    receive(peer_.data(text("c")));
    EXPECT_EQ(c->unread().size(), 2U);
    EXPECT_EQ(unread_of(*c), text("bc"));
    receive(peer_.data(text("d")));
    EXPECT_EQ(tcp_->counters().dropped_no_slot, 2U) << "two slots, both held";
}

TEST_P(TcpReceive, SharedPoolExhaustionNeverAdvancesRcvNxt) {
    configure_tcp({.receive_pool = 2});
    auto* first = open_passive(peer_);
    ASSERT_NE(first, nullptr);
    aloe::frames::TcpPeer other{aloe::testing::peer_spec(40001), TcpSequence{2000U}};
    auto* second = open_passive(other);
    ASSERT_NE(second, nullptr);
    receive(peer_.data(text("a")));
    receive(peer_.data(text("b")));
    EXPECT_EQ(tcp_->nodes_available(), 0U);
    receive(other.data(text("x")));
    EXPECT_EQ(tcp_->counters().dropped_no_node, 1U);
    EXPECT_TRUE(second->unread().empty());
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{2001U}) << "nothing of the second was accepted";
    EXPECT_EQ(frames[0].tcp->destination_port, 40001U);
    first->consume(2);
    EXPECT_EQ(tcp_->nodes_available(), 2U);
    other.rewind(1);
    receive(other.data(text("x")));
    EXPECT_EQ(second->unread().size(), 1U);
    EXPECT_EQ(tcp_->counters().dropped_no_node, 1U);
}
```

`tests/unit_tests/common/tcp/test_tcp_window.cpp`:

```cpp
#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>

namespace {

    using namespace std::chrono_literals;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;
    using aloe::wire::TcpSequence;
    using TcpWindow         = aloe::testing::TcpFixture;
    using TcpWindowRefusing = aloe::testing::TcpRefusingFixture;

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpWindow,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpWindowRefusing,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

    constexpr std::uint32_t budget = aloe::testing::tcp_fabric_budget;

    /// The right edge a pure ACK offers: its acknowledgement plus its window.
    [[nodiscard]] TcpSequence right_edge(const aloe::frames::ParsedFrame& frame) {
        return frame.tcp->acknowledgement + frame.tcp->window;
    }

}  // namespace

TEST_P(TcpWindow, OneByteCostsOneByteOfCreditAndKeepsTheEdge) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(tcp_->byte_budget(), 46720U) << "32 packets at MSS 1460";
    const TcpSequence initial_edge = TcpSequence{aloe::testing::tcp_peer_isn + 1} + budget;
    receive(peer_.data(aloe::frames::pattern(1)));
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, budget - 1U);
    EXPECT_EQ(right_edge(frames[0]), initial_edge) << "a whole packet node, one byte of credit";
    receive(peer_.data(aloe::frames::pattern(100)));
    frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, budget - 101U);
    EXPECT_EQ(right_edge(frames[0]), initial_edge);
    EXPECT_EQ(tcp_->counters().window_updates, 0U);
    c->consume(50);
    frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "consumption that frees credit is a window update";
    EXPECT_EQ(frames[0].tcp->window, budget - 51U);
    EXPECT_EQ(right_edge(frames[0]), initial_edge + 50U);
    EXPECT_EQ(tcp_->counters().window_updates, 1U);
    EXPECT_TRUE(collect().empty()) << "once";
}

TEST_P(TcpWindow, PartialConsumeFreesNoNodeButMovesTheEdge) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const std::size_t nodes = tcp_->nodes_available();
    receive(peer_.data(aloe::frames::pattern(10)));
    std::ignore = collect();
    c->consume(4);
    EXPECT_EQ(tcp_->nodes_available(), nodes - 1) << "the packet still holds six bytes";
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, budget - 6U);
}

TEST_P(TcpWindow, WrapsNearTheTop) {
    aloe::frames::TcpPeer high{aloe::testing::peer_spec(), TcpSequence{0xfffffff0U}};
    auto* c = open_passive(high);
    ASSERT_NE(c, nullptr);
    receive(high.data(aloe::frames::pattern(32)));
    EXPECT_EQ(c->unread().size(), 32U);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{0x11U}) << "0xfffffff1 + 32, wrapped";
    EXPECT_EQ(frames[0].tcp->window, budget - 32U);
    EXPECT_EQ(right_edge(frames[0]), TcpSequence{0xfffffff1U} + budget);
    receive(high.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(4), TcpSequence{0xfffffffeU}, high.rcv_nxt()));
    EXPECT_EQ(tcp_->counters().dropped_duplicate, 1U) << "entirely before rcv_nxt across the wrap";
    EXPECT_EQ(tcp_->counters().dropped_out_of_order, 0U);
}

TEST_P(TcpWindow, ZeroWindowAcceptsPureAcksAndReopensOnConsume) {
    configure_tcp({.receive_segments = 1});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(tcp_->byte_budget(), 1460U);
    receive(peer_.data(aloe::frames::pattern(1460)));
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, 0U);
    receive(peer_.ack());
    EXPECT_EQ(tcp_->counters().dropped_out_of_window, 0U) << "a zero-length segment at rcv_nxt is acceptable at a zero window";
    receive(peer_.data(aloe::frames::pattern(1)));
    EXPECT_EQ(tcp_->counters().dropped_out_of_window, 1U) << "a byte is not";
    frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, 0U);
    peer_.rewind(1);
    c->consume(1460);
    frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, 1460U) << "reopened";
    EXPECT_EQ(tcp_->counters().window_updates, 1U);
    receive(peer_.data(aloe::frames::pattern(1)));
    EXPECT_EQ(c->unread().size(), 1U);
}

TEST_P(TcpWindow, TinySegmentsExhaustSlotsWithCreditLeft) {
    configure_tcp({.receive_segments = 4});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    for (int i = 0; i < 4; ++i) {
        receive(peer_.data(aloe::frames::pattern(1)));
    }
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    const std::uint32_t credit = 4U * aloe::testing::tcp_fabric_mss - 4U;
    EXPECT_EQ(frames[0].tcp->window, credit);
    receive(peer_.data(aloe::frames::pattern(1)));
    EXPECT_EQ(tcp_->counters().dropped_no_slot, 1U);
    frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, credit) << "neither extended nor retracted while every slot is held";
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 5});
    c->consume(1);
    frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, credit + 1U) << "a slot and a byte came back";
}

TEST_P(TcpWindow, BudgetIsCappedAtTheWireMaximum) {
    configure_tcp({.receive_segments = 64});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(tcp_->byte_budget(), 65535U);
    EXPECT_EQ(peer_.stack_window(), 65535U);
}

TEST_P(TcpWindowRefusing, RefusedUpdateLeavesThePreviousOffer) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(100)));
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    peer_.see(frames[0]);
    EXPECT_EQ(peer_.stack_window(), budget - 100U);
    refuse();
    c->consume(100);
    EXPECT_TRUE(collect().empty()) << "the update could not leave";
    EXPECT_EQ(tcp_->counters().window_updates, 0U) << "an offer that was not queued is not recorded";
    EXPECT_EQ(tcp_->counters().send_refused, 1U);
    allow();
    frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "still pending: flush retries it";
    EXPECT_EQ(frames[0].tcp->window, budget);
    EXPECT_EQ(tcp_->counters().window_updates, 1U);
}
```

Add both files to `Aloe.Tests.Unit.Tcp`.

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Tcp`
Expected: link errors for `Stack<...>::consume` (declared, undefined).

- [ ] **Step 3: Write `consume`**

Append to `common/tcp/stack/tcp_transmit.hpp`, inside the namespace:

```cpp
    /// Advances the chain's front by `count` bytes, freeing packets as they empty; a reopened edge queues an update.
    template <typename Ip>
    void Stack<Ip>::consume(ConnectionType& c, std::size_t count) noexcept {
        assert(count <= c.unread_bytes_ && "consume past unread()");
        while (count > 0) {
            typename detail::TcpNodePool<Packet>::Node& node = pool_.node(c.chain_head_);
            const std::size_t taken                          = std::min<std::size_t>(count, node.length);
            node.offset                                      = static_cast<std::uint16_t>(node.offset + taken);
            node.length                                      = static_cast<std::uint16_t>(node.length - taken);
            count           -= taken;
            c.unread_bytes_ -= taken;
            if (node.length == 0) {
                const std::uint32_t next = node.next;
                pool_.release(c.chain_head_);
                c.chain_head_ = next;
                if (next == detail::no_node) {
                    c.chain_tail_ = detail::no_node;
                }
                --c.chain_count_;
            }
        }
        const bool receiving = c.state_ == State::Established || c.state_ == State::FinWait1 || c.state_ == State::FinWait2;
        if (!receiving) {
            return;  // the peer sends no more data, or the connection is closed: no offer to make
        }
        const Sequence candidate = c.rcv_nxt_ + (byte_budget_ - static_cast<std::uint32_t>(c.unread_bytes_));
        if (candidate.after(c.rcv_adv_) && c.chain_count_ < config_.receive_segments) {
            queue_ack(c);  // an ordinary window update, including the reopening of a zero window
        }
    }
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Tcp && ctest --preset debug -R '^Aloe[.]Tests[.]Unit[.]Tcp$' --output-on-failure`
Expected: PASS in both offload modes.

- [ ] **Step 5: Format and commit**

```bash
./scripts/check-format.sh
git add common/tcp tests/unit_tests/common/tcp
git commit -m "feat(tcp): receive packet-backed streams with byte windows"
```

## Task 8: Prepared transmit packets, backpressure and the ACK policy

**Files:**
- Modify: `common/tcp/stack/tcp_transmit.hpp` (adds `prepare` and `commit`), `tests/unit_tests/common/tcp/test_tcp_data.cpp`, `tests/unit_tests/common/tcp/CMakeLists.txt`, `tests/unit_tests/common/stream/CMakeLists.txt`
- Create: `tests/unit_tests/common/tcp/test_tcp_acks.cpp`, `tests/unit_tests/common/stream/test_stream_tcp.cpp`

**Interfaces:**
- Consumes: Task 6's `send_segment`, `mark_retry`, `writable`, the ACK emitters and the retry list.
- Produces: `std::optional<std::span<std::byte>> Stack<Ip>::prepare(ConnectionType&, std::size_t)`: nothing for a count of zero or when `writable()` is zero, with no allocation; a span of `min(count, writable())` bytes inside a fresh packet otherwise; an allocation failure marks a retry hint and returns nothing. `bool Stack<Ip>::commit(ConnectionType&, std::size_t)`: `commit(0)` discards the preparation and returns true, nothing was refused and no hint follows; otherwise one PSH+ACK segment through `send_segment`; a refusal returns the packet to the pool, leaves every number as it was, marks a retry hint and returns false. The retry list holds at most one entry per connection and is drained by the next `process`, empty or not.

- [ ] **Step 1: Write the failing tests**

Add to `tests/unit_tests/common/tcp/test_tcp_data.cpp`, in the anonymous namespace:

```cpp
    using TcpSend         = aloe::testing::TcpFixture;
    using TcpSendRefusing = aloe::testing::TcpRefusingFixture;

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpSend,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpSendRefusing,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

    [[nodiscard]] std::vector<std::byte> payload_of(const aloe::frames::ParsedFrame& frame) {
        const auto bytes = aloe::frames::tcp_payload(frame);
        return {bytes.begin(), bytes.end()};
    }
```

and the tests:

```cpp
TEST_P(TcpSend, PrepareCommitWritesInPlace) {
    auto* c = open_active(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence first = c->committed();
    EXPECT_EQ(c->writable(), aloe::testing::tcp_fabric_mss) << "min(mss, the peer's 65535)";
    const auto out = c->prepare(5);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->size(), 5U);
    EXPECT_EQ(c->writable(), 0U) << "a prepare is open";
    std::ranges::copy(text("hello"), out->begin());
    (*out)[0] = static_cast<std::byte>('j');  // written after prepare, before commit: the span is the packet
    ASSERT_TRUE(c->commit(5));
    EXPECT_EQ(c->committed(), first + 5U);
    EXPECT_EQ(c->unacknowledged(), 5U);
    EXPECT_EQ(c->writable(), aloe::testing::tcp_fabric_mss);
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->flags, (TcpFlags{TcpFlag::Psh, TcpFlag::Ack}));
    EXPECT_EQ(frames[0].tcp->sequence, first);
    EXPECT_EQ(frames[0].tcp->acknowledgement, peer_.snd_nxt());
    EXPECT_EQ(frames[0].tcp->window, aloe::testing::tcp_fabric_budget);
    EXPECT_EQ(payload_of(frames[0]), text("jello"));
    EXPECT_EQ(aloe::frames::l4_checksum_residue(*frames[0].ipv4, frames[0].l4), 0U)
        << (offloads() ? "completed by the device" : "computed in software");
    EXPECT_EQ(tcp_->counters().data_segments_sent, 1U);
    EXPECT_TRUE(collect().empty()) << "no pure ACK follows data that carried it";
}

TEST_P(TcpSend, TwoCommitsAreTwoSegments) {
    auto* c = open_active(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence first = c->committed();
    EXPECT_EQ(c->send(text("ab")), 2U);
    EXPECT_EQ(c->send(text("cd")), 2U);
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 2U);
    EXPECT_EQ(payload_of(frames[0]), text("ab"));
    EXPECT_EQ(payload_of(frames[1]), text("cd"));
    EXPECT_EQ(frames[0].tcp->sequence, first);
    EXPECT_EQ(frames[1].tcp->sequence, first + 2U);
    EXPECT_EQ(tcp_->counters().data_segments_sent, 2U);
}

TEST_P(TcpSend, ClampedByPeerMssAndWindow) {
    peer_.spec().mss    = 536;
    peer_.spec().window = 100;
    auto* c             = open_active(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->mss(), 536U);
    EXPECT_EQ(c->writable(), 100U) << "the peer's window, smaller than its MSS";
    const auto out = c->prepare(2000);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->size(), 100U);
    ASSERT_TRUE(c->commit(0));
    EXPECT_EQ(c->send(aloe::frames::pattern(300)), 100U) << "the window, then prepare gives nothing";
    EXPECT_EQ(c->writable(), 0U);
    auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(payload_of(frames[0]).size(), 100U);
    peer_.see(frames[0]);
    receive(peer_.ack());
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.acked());
    EXPECT_TRUE(event->events.writable());
    EXPECT_EQ(c->writable(), 100U);
    EXPECT_EQ(c->acknowledged(), c->committed());
    peer_.spec().window = 65535;
    receive(peer_.ack());
    EXPECT_EQ(c->writable(), 536U) << "now the MSS";
}

TEST_P(TcpSend, PeerWindowShrinkBelowInFlight) {
    peer_.spec().window = 1000;
    auto* c             = open_active(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->send(aloe::frames::pattern(600)), 600U);
    auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    peer_.spec().window = 500;
    receive(peer_.ack());  // acknowledges nothing new, shrinks the window below what is in flight
    EXPECT_EQ(c->writable(), 0U) << "clamped, not wrapped";
    EXPECT_EQ(tcp_->pending_events(), 0U) << "no Writable: nothing increased";
    peer_.see(frames[0]);
    receive(peer_.ack());  // acknowledges the 600 with window 500
    EXPECT_EQ(c->writable(), 500U);
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.writable());
}

TEST_P(TcpSend, CommitZeroCancelsAndEmptySendPreparesNothing) {
    auto* c = open_active(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence before = c->committed();
    ASSERT_TRUE(c->prepare(10).has_value());
    EXPECT_TRUE(c->commit(0));
    EXPECT_EQ(c->committed(), before);
    EXPECT_EQ(c->writable(), aloe::testing::tcp_fabric_mss);
    EXPECT_EQ(c->send({}), 0U);
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(tcp_->counters().data_segments_sent, 0U);
    EXPECT_EQ(tcp_->pending_events(), 0U) << "no hint for a cancelled preparation";
}

TEST_P(TcpSend, AckProgressRaisesWritableFromZeroAndAcrossAPositiveValue) {
    peer_.spec().window = 100;
    auto* c             = open_active(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->send(aloe::frames::pattern(100)), 100U);
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(c->writable(), 0U);
    const TcpSequence start = frames[0].tcp->sequence;
    receive(peer_.segment({TcpFlag::Ack}, {}, peer_.snd_nxt(), start + 40U));
    auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.acked());
    EXPECT_TRUE(event->events.writable()) << "from zero";
    EXPECT_EQ(c->writable(), 40U);
    EXPECT_EQ(c->acknowledged(), start + 40U);
    EXPECT_EQ(c->unacknowledged(), 60U);
    receive(peer_.segment({TcpFlag::Ack}, {}, peer_.snd_nxt(), start + 100U));
    event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.writable()) << "from forty to a hundred";
    EXPECT_EQ(c->writable(), 100U);
    receive(peer_.segment({TcpFlag::Ack}, {}, peer_.snd_nxt(), start + 100U));
    EXPECT_EQ(tcp_->pending_events(), 0U) << "a duplicate ACK increases nothing";
    receive(peer_.segment({TcpFlag::Ack}, {}, peer_.snd_nxt(), start + 200U));
    EXPECT_EQ(tcp_->counters().dropped_unexpected, 1U) << "acknowledges what was never sent";
    EXPECT_EQ(collect_queued().size(), 1U) << "and is answered with an ACK";
}

TEST_P(TcpSendRefusing, RefusalPreservesNumbersAndRaisesOneHint) {
    auto* c = open_active(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence sequence  = c->committed();
    const std::size_t eligible = c->writable();
    refuse();
    const auto out = c->prepare(5);
    ASSERT_TRUE(out.has_value());
    std::ranges::copy(text("hello"), out->begin());
    EXPECT_FALSE(c->commit(5));
    EXPECT_EQ(c->committed(), sequence);
    EXPECT_EQ(c->unacknowledged(), 0U);
    EXPECT_EQ(c->writable(), eligible) << "eligibility is unchanged; the device is the problem";
    EXPECT_EQ(tcp_->counters().send_refused, 1U);
    EXPECT_EQ(tcp_->counters().data_segments_sent, 0U);
    EXPECT_EQ(tcp_->pending_events(), 0U) << "the hint is deferred to the next process";
    advance(0ms);
    auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->events, aloe::stream::Events{aloe::stream::Event::Writable});
    advance(0ms);
    EXPECT_FALSE(poll().has_value()) << "one hint per refusal";
    allow();
    EXPECT_EQ(c->send(text("hello")), 5U);
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->sequence, sequence) << "the refused segment's number was never spent";
}

TEST_P(TcpSendRefusing, AllocationFailureRaisesAHintAndRetainsNothing) {
    auto* c = open_active(peer_);
    ASSERT_NE(c, nullptr);
    device.fail_allocations = 1;
    EXPECT_FALSE(c->prepare(5).has_value());
    EXPECT_EQ(c->writable(), aloe::testing::tcp_fabric_mss) << "no prepare is open";
    advance(0ms);
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.writable());
    EXPECT_TRUE(c->prepare(5).has_value()) << "the next allocation succeeds";
    EXPECT_TRUE(c->commit(0));
}

TEST_P(TcpSendRefusing, PartialHelperSendStopsAtTheRefusal) {
    auto* c = open_active(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence sequence = c->committed();
    // The ring holds one packet; the device refuses the second flush.
    EXPECT_EQ(c->send(aloe::frames::pattern(100)), 100U);
    refuse();
    EXPECT_EQ(c->send(aloe::frames::pattern(3000)), 0U) << "the first segment of this send is refused: nothing accepted";
    EXPECT_EQ(c->committed(), sequence + 100U);
    allow();
    EXPECT_EQ(c->send(aloe::frames::pattern(3000)), 3000U);
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 4U) << "100, 1460, 1460, 80";
    EXPECT_EQ(payload_of(frames[1]).size(), 1460U);
    EXPECT_EQ(payload_of(frames[3]).size(), 80U);
    EXPECT_EQ(frames[3].tcp->sequence, sequence + 100U + 2920U);
}
```

`tests/unit_tests/common/tcp/test_tcp_acks.cpp`:

```cpp
#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>

namespace {

    using namespace std::chrono_literals;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;
    using aloe::wire::TcpSequence;
    using TcpAcks         = aloe::testing::TcpFixture;
    using TcpAcksRefusing = aloe::testing::TcpRefusingFixture;

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpAcks,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpAcksRefusing,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

    constexpr std::uint16_t mss = aloe::testing::tcp_fabric_mss;

    [[nodiscard]] std::size_t pure_acks(const std::vector<aloe::frames::ParsedFrame>& frames) {
        std::size_t count = 0;
        for (const auto& frame : frames) {
            if (aloe::testing::is_flags(frame, {TcpFlag::Ack}) && aloe::frames::tcp_payload(frame).empty()) {
                ++count;
            }
        }
        return count;
    }

}  // namespace

TEST_P(TcpAcks, ShortInOrderSegmentsCoalesceUntilFlush) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    for (int i = 0; i < 5; ++i) {
        receive(peer_.data(aloe::frames::pattern(10)));
    }
    EXPECT_TRUE(collect_queued().empty());
    const auto frames = collect();
    EXPECT_EQ(pure_acks(frames), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 51});
    EXPECT_EQ(tcp_->counters().pure_acks_sent, 1U);
}

TEST_P(TcpAcks, SameTickReplyCarriesTheAck) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(10)));
    EXPECT_EQ(c->send(aloe::frames::pattern(4)), 4U);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "the data segment, and no pure ACK after it";
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 11});
    EXPECT_EQ(aloe::frames::tcp_payload(frames[0]).size(), 4U);
    EXPECT_EQ(tcp_->counters().pure_acks_sent, 0U);
}

TEST_P(TcpAcks, EverySecondFullSizedSegmentIsAckedAtOnce) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(mss)));
    EXPECT_TRUE(collect_queued().empty());
    receive(peer_.data(aloe::frames::pattern(mss)));
    auto frames = collect_queued();
    EXPECT_EQ(pure_acks(frames), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 1 + 2 * mss});
    receive(peer_.data(aloe::frames::pattern(mss)));
    EXPECT_TRUE(collect_queued().empty());
    receive(peer_.data(aloe::frames::pattern(mss)));
    EXPECT_EQ(pure_acks(collect_queued()), 1U);
    receive(peer_.data(aloe::frames::pattern(10)));
    EXPECT_TRUE(collect_queued().empty()) << "a short segment is ordinary";
    EXPECT_EQ(pure_acks(collect()), 1U);
}

TEST_P(TcpAcks, ThreeGapSegmentsInOneBurstAttemptThreeDuplicateAcks) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(3)));
    std::ignore = collect();
    const TcpSequence expected = peer_.snd_nxt();  // 1004
    // Three segments above a gap, injected together and processed in one pass.
    for (const std::uint32_t at : {10U, 20U, 30U}) {
        auto packet = harness_->allocate(0);
        ASSERT_TRUE(packet.has_value());
        ASSERT_TRUE(aloe::frames::fill(
            *packet, peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(10), expected + at, peer_.rcv_nxt())));
        std::array<aloe::fabric::Packet, 1> out{std::move(*packet)};
        ASSERT_EQ(harness_->transmit(0, out), 1U);
    }
    process_pending();
    EXPECT_EQ(tcp_->counters().dropped_out_of_order, 3U);
    auto frames = collect_queued();
    EXPECT_EQ(pure_acks(frames), 3U) << "one per segment, not one coalesced at the end";
    for (const auto& frame : frames) {
        EXPECT_EQ(frame.tcp->acknowledgement, expected);
    }
    // Recovery: every segment that advances rcv_nxt is acknowledged at once until the remembered end, 1044.
    receive(peer_.data(aloe::frames::pattern(10)));  // 1004..1013
    frames = collect_queued();
    EXPECT_EQ(pure_acks(frames), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, expected + 10U);
    receive(peer_.data(aloe::frames::pattern(10)));  // 1014..1023: still below the gap end
    EXPECT_EQ(pure_acks(collect_queued()), 1U);
    receive(peer_.data(aloe::frames::pattern(20)));  // 1024..1043
    EXPECT_EQ(pure_acks(collect_queued()), 1U) << "reaching the remembered end";
    receive(peer_.data(aloe::frames::pattern(5)));   // past it: ordinary again
    EXPECT_TRUE(collect_queued().empty());
    EXPECT_EQ(pure_acks(collect()), 1U);
    EXPECT_EQ(c->unread().size(), 48U);
}

TEST_P(TcpAcks, DuplicateAndOutOfWindowDataAckedAtOnceRejectedRstIsSilent) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(3)));
    std::ignore = collect();
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(3), peer_.iss() + 1U, peer_.rcv_nxt()));
    EXPECT_EQ(tcp_->counters().dropped_duplicate, 1U);
    EXPECT_EQ(pure_acks(collect_queued()), 1U);
    const TcpSequence beyond = peer_.snd_nxt() + aloe::testing::tcp_fabric_budget + 10U;
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(3), beyond, peer_.rcv_nxt()));
    EXPECT_EQ(tcp_->counters().dropped_out_of_order, 1U) << "a gap, far beyond the window";
    EXPECT_EQ(pure_acks(collect_queued()), 1U);
    receive(peer_.segment({TcpFlag::Rst, TcpFlag::Ack}, {}, beyond, peer_.rcv_nxt()));
    EXPECT_TRUE(collect_queued().empty()) << "a rejected RST is not answered";
    EXPECT_EQ(c->state(), aloe::tcp::State::Established);
    EXPECT_EQ(tcp_->counters().connections_reset, 0U);
    EXPECT_EQ(tcp_->counters().dropped_out_of_window, 1U);
}

TEST_P(TcpAcksRefusing, RefusalLeavesOneAckPendingWithoutReplayingDuplicates) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(3)));
    std::ignore = collect();
    refuse();
    for (const std::uint32_t at : {10U, 20U, 30U}) {
        receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(10), peer_.snd_nxt() + at, peer_.rcv_nxt()));
    }
    EXPECT_EQ(tcp_->counters().send_refused, 3U) << "three attempts";
    EXPECT_TRUE(collect().empty());
    allow();
    const auto frames = collect();
    EXPECT_EQ(pure_acks(frames), 1U) << "the pending ACK, once: no manufactured duplicates";
}

TEST_P(TcpAcksRefusing, FullSegmentCountSaturatesAtTwoUntilAnAckLeaves) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    refuse();
    receive(peer_.data(aloe::frames::pattern(mss)));
    receive(peer_.data(aloe::frames::pattern(mss)));
    EXPECT_EQ(tcp_->counters().send_refused, 1U) << "the second full segment attempted an ACK";
    receive(peer_.data(aloe::frames::pattern(mss)));
    EXPECT_EQ(tcp_->counters().send_refused, 2U) << "saturated at two: every further full segment attempts again";
    allow();
    EXPECT_EQ(pure_acks(collect()), 1U);
    receive(peer_.data(aloe::frames::pattern(mss)));
    EXPECT_TRUE(collect_queued().empty()) << "the count was reset by the ACK that left";
    receive(peer_.data(aloe::frames::pattern(mss)));
    EXPECT_EQ(pure_acks(collect_queued()), 1U);
}
```

`tests/unit_tests/common/stream/test_stream_tcp.cpp`, pinning the concrete types in the Stream binary:

```cpp
#include <aloe/fabric>
#include <aloe/net>
#include <aloe/stream>
#include <aloe/tcp>

#include <gtest/gtest.h>

// The brick's connection is the stream contract, and its view is the read view: pinned where the contract lives.
static_assert(aloe::stream::IsStream<aloe::tcp::Connection<aloe::net::Ipv4<aloe::fabric::Port>>>);
static_assert(aloe::stream::IsReadView<aloe::tcp::ReadView<aloe::fabric::Packet>>);

TEST(StreamContract, TcpConnectionModelsIt) {
    SUCCEED();
}
```

Add `test_tcp_acks.cpp` to `Aloe.Tests.Unit.Tcp`; add `test_stream_tcp.cpp` to `Aloe.Tests.Unit.Stream` and link `Aloe::Common::Tcp` and `Aloe::Fixtures::Fabric` to that test target only, not to `Aloe::Common::Stream`.

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Tcp Aloe.Tests.Unit.Stream`
Expected: link errors for `Stack<...>::prepare` and `commit`.

- [ ] **Step 3: Write `prepare` and `commit`**

Append to `common/tcp/stack/tcp_transmit.hpp`, inside the namespace:

```cpp
    template <typename Ip>
    std::optional<std::span<std::byte>> Stack<Ip>::prepare(ConnectionType& c, const std::size_t count) noexcept {
        assert(!c.prepared_ && "one prepare at a time: commit first");
        if (count == 0) {
            return std::nullopt;
        }
        const std::size_t room = writable(c);
        if (room == 0) {
            return std::nullopt;  // nothing allocates when nothing is writable
        }
        const std::size_t size       = std::min(count, room);
        std::optional<Packet> packet = ip_->allocate();
        if (!packet) {
            mark_retry(c);  // the pool may have packets by the next process
            return std::nullopt;
        }
        const std::optional<std::span<std::byte>> span = packet->append(size);
        if (!span) {
            mark_retry(c);  // cannot happen: size is at most the MSS and a packet holds a whole frame
            return std::nullopt;
        }
        c.prepared_      = std::move(*packet);  // the span points into the block, which does not move
        c.prepared_size_ = size;
        return span;
    }

    template <typename Ip>
    bool Stack<Ip>::commit(ConnectionType& c, const std::size_t count) noexcept {
        assert(c.prepared_ && "commit without a prepare");
        assert(count <= c.prepared_size_ && "commit more than was prepared");
        Packet packet = std::move(*c.prepared_);
        c.prepared_.reset();
        const std::size_t prepared = c.prepared_size_;
        c.prepared_size_           = 0;
        if (count == 0) {
            return true;  // discarded: `packet` goes back to the pool; nothing was refused
        }
        if (c.state_ != State::Established && c.state_ != State::CloseWait) {
            return false;  // reset or closed since the prepare: no hint, the events say why
        }
        packet.trim_back(prepared - count);
        if (!send_segment(c, std::move(packet), c.snd_nxt_, wire::TcpFlags{wire::TcpFlag::Ack, wire::TcpFlag::Psh}, false)) {
            mark_retry(c);
            return false;
        }
        c.snd_nxt_ = c.snd_nxt_ + static_cast<std::uint32_t>(count);
        ++counters_.data_segments_sent;
        return true;
    }
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Tcp Aloe.Tests.Unit.Stream Aloe.Tests.Unit.Frames && ctest --preset debug -R '^Aloe[.]Tests[.]Unit[.](Tcp|Stream|Frames)$' --output-on-failure`
Expected: PASS. Every input slot is empty after processing (`burst_empty()` in the receive tests) and a failed preparation retains nothing (`AllocationFailureRaisesAHintAndRetainsNothing`).

- [ ] **Step 5: Format and commit**

```bash
./scripts/check-format.sh
git add common/tcp tests/unit_tests/common/tcp tests/unit_tests/common/stream
git commit -m "feat(tcp): transmit prepared segments and preserve ACK feedback"
```

## Task 9: Close and reset, the two-brick exchange, and placement

**Files:**
- Modify: `common/tcp/stack/tcp_control.hpp` (adds `close`), `tests/unit_tests/common/tcp/CMakeLists.txt`
- Create: `tests/unit_tests/common/tcp/test_tcp_close.cpp`, `test_tcp_two_stacks.cpp`, `test_tcp_placement.cpp`, `docs/architecture/tcp.md`

**Interfaces:**
- Consumes: Task 6's FIN handling, timer, `abort`, `release`, `choose_port`; Task 8's send path.
- Produces: `void Stack<Ip>::close(ConnectionType&) noexcept`: in Established moves to FinWait1, in CloseWait to LastAck, discards an open prepare, sends FIN+ACK at `snd_nxt`, records that sequence for retransmission, advances `snd_nxt` by one and arms the timer; nothing in any other state. The rest of this task pins behaviour already written: the synchronized close states, terminal ACK before `Closed`, RST handling, slot release and reuse, port choice and placement.

- [ ] **Step 1: Write the failing tests**

`tests/unit_tests/common/tcp/test_tcp_close.cpp`:

```cpp
#include <aloe/frames>
#include <aloe/stream>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>

namespace {

    using namespace std::chrono_literals;
    using aloe::stream::Event;
    using aloe::stream::Events;
    using aloe::tcp::State;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;
    using aloe::wire::TcpSequence;
    using TcpClose         = aloe::testing::TcpFixture;
    using TcpCloseRefusing = aloe::testing::TcpRefusingFixture;

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpClose,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpCloseRefusing,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

    [[nodiscard]] std::vector<std::byte> text(const char* s) {
        std::vector<std::byte> out;
        for (; *s != '\0'; ++s) {
            out.push_back(static_cast<std::byte>(*s));
        }
        return out;
    }

}  // namespace

TEST_P(TcpClose, LocalFirst) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence committed = c->committed();
    c->close();
    EXPECT_EQ(c->state(), State::FinWait1);
    EXPECT_EQ(c->writable(), 0U) << "no more sends";
    EXPECT_EQ(wheel_.pending(), 1U);
    auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->flags, (TcpFlags{TcpFlag::Fin, TcpFlag::Ack}));
    EXPECT_EQ(frames[0].tcp->sequence, committed);
    EXPECT_EQ(c->committed(), committed + 1U) << "the FIN takes a sequence number";
    receive(peer_.ack(frames[0]));
    EXPECT_EQ(c->state(), State::FinWait2);
    EXPECT_EQ(wheel_.pending(), 0U);
    auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->events, Events{Event::Acked});
    receive(peer_.fin());
    EXPECT_EQ(c->state(), State::Closed);
    EXPECT_TRUE(c->peer_closed());
    frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U) << "the terminal ACK was queued before Closed was published";
    EXPECT_EQ(frames[0].tcp->acknowledgement, peer_.snd_nxt());
    event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->events, (Events{Event::PeerClosed, Event::Closed}));
    EXPECT_EQ(tcp_->counters().connections_closed, 1U);
    EXPECT_EQ(tcp_->counters().control_segments_sent, 2U) << "the SYN-ACK and the FIN";
    c->release();
    EXPECT_EQ(tcp_->table_size(), 0U);
    EXPECT_TRUE(collect().empty()) << "a normal close sends no RST on release";
    EXPECT_EQ(tcp_->counters().resets_sent, 0U);
}

TEST_P(TcpClose, PeerFirstWithUnreadData) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(text("bye")));
    receive(peer_.fin());
    EXPECT_EQ(c->state(), State::CloseWait);
    EXPECT_TRUE(c->peer_closed());
    EXPECT_EQ(c->unread().size(), 3U) << "what came before the FIN is still readable";
    EXPECT_TRUE(collect_queued().empty()) << "the application's close may carry the ACK";
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->events, (Events{Event::Readable, Event::PeerClosed}));
    c->consume(3);
    c->close();
    EXPECT_EQ(c->state(), State::LastAck);
    auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->flags, (TcpFlags{TcpFlag::Fin, TcpFlag::Ack}));
    EXPECT_EQ(frames[0].tcp->acknowledgement, peer_.snd_nxt()) << "the FIN carries the peer's FIN ACK";
    EXPECT_TRUE(collect().empty()) << "and no pure ACK follows it";
    receive(peer_.ack(frames[0]));
    EXPECT_EQ(c->state(), State::Closed);
    EXPECT_EQ(poll()->events, (Events{Event::Acked, Event::Closed}));
    EXPECT_EQ(tcp_->counters().connections_closed, 1U);
    EXPECT_EQ(wheel_.pending(), 0U);
}

TEST_P(TcpClose, Simultaneous) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    c->close();
    const auto fin = collect_queued();
    ASSERT_EQ(fin.size(), 1U);
    receive(peer_.fin());  // the peer's FIN crossed ours: it acknowledges nothing new
    EXPECT_EQ(c->state(), State::Closing);
    auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U) << "a FIN in FinWait1 is acknowledged at once";
    EXPECT_EQ(frames[0].tcp->acknowledgement, peer_.snd_nxt());
    receive(peer_.ack(fin[0]));
    EXPECT_EQ(c->state(), State::Closed);
    EXPECT_TRUE(poll()->events.closed());
    EXPECT_EQ(tcp_->counters().connections_closed, 1U);
}

TEST_P(TcpClose, DataAndFinTogether) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack, TcpFlag::Fin}, text("end"), peer_.snd_nxt(), peer_.rcv_nxt()));
    EXPECT_EQ(c->unread().size(), 3U);
    EXPECT_TRUE(c->peer_closed());
    EXPECT_EQ(c->state(), State::CloseWait);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, peer_.snd_nxt() + 4U) << "three bytes and the FIN";
}

TEST_P(TcpClose, HandshakeAckWithFin) {
    ASSERT_TRUE(tcp_->listen(aloe::testing::tcp_listen_port).has_value());
    receive(peer_.syn());
    const auto syn_ack = collect();
    ASSERT_EQ(syn_ack.size(), 1U);
    peer_.see(syn_ack[0]);
    receive(peer_.fin());  // completes the handshake and closes the peer's half in one segment
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->events, (Events{Event::Accepted, Event::PeerClosed}));
    EXPECT_EQ(event->connection->state(), State::CloseWait);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 2});
}

TEST_P(TcpClose, DuplicateFin) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.fin());
    const auto first = collect();
    ASSERT_EQ(first.size(), 1U);
    peer_.rewind(1);
    receive(peer_.fin());  // retransmitted
    EXPECT_EQ(tcp_->counters().dropped_duplicate, 1U);
    const auto again = collect_queued();
    ASSERT_EQ(again.size(), 1U) << "acknowledged at once";
    EXPECT_EQ(again[0].tcp->acknowledgement, first[0].tcp->acknowledgement) << "rcv_nxt advanced once";
    EXPECT_EQ(c->state(), State::CloseWait);
    EXPECT_EQ(tcp_->pending_events(), 1U);
}

TEST_P(TcpClose, FinRetransmitKeepsItsSequenceThenTimesOut) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    c->close();
    const auto first = collect_queued();
    ASSERT_EQ(first.size(), 1U);
    advance(1000ms);
    const auto second = collect();
    ASSERT_EQ(second.size(), 1U);
    EXPECT_EQ(second[0].tcp->flags, (TcpFlags{TcpFlag::Fin, TcpFlag::Ack}));
    EXPECT_EQ(second[0].tcp->sequence, first[0].tcp->sequence);
    EXPECT_EQ(tcp_->counters().retransmits, 1U);
    advance(62000ms);
    EXPECT_EQ(c->state(), State::Closed);
    EXPECT_TRUE(poll()->events.timed_out());
    EXPECT_EQ(tcp_->counters().connections_timed_out, 1U);
    EXPECT_EQ(wheel_.pending(), 0U);
}

TEST_P(TcpClose, CloseInOtherStatesDoesNothing) {
    const auto connected = tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    std::ignore = collect();
    (*connected)->close();
    EXPECT_EQ((*connected)->state(), State::SynSent);
    EXPECT_TRUE(collect().empty());
    (*connected)->release();
    std::ignore = collect();
}

TEST_P(TcpClose, AbortSendsRstAndKeepsUnreadUntilRelease) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(text("kept")));
    const std::size_t nodes = tcp_->nodes_available();
    c->abort();
    EXPECT_EQ(c->state(), State::Closed);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "the RST, and no pending ACK after it";
    EXPECT_EQ(frames[0].tcp->flags, (TcpFlags{TcpFlag::Rst, TcpFlag::Ack}));
    EXPECT_EQ(frames[0].tcp->sequence, c->committed());
    EXPECT_EQ(poll()->events, (Events{Event::Readable, Event::Closed}));
    EXPECT_EQ(c->unread().size(), 4U);
    EXPECT_EQ(tcp_->nodes_available(), nodes);
    EXPECT_EQ(wheel_.pending(), 0U);
    EXPECT_EQ(tcp_->counters().resets_sent, 1U);
    c->release();
    EXPECT_EQ(tcp_->nodes_available(), nodes + 1);
    EXPECT_EQ(tcp_->counters().resets_sent, 1U) << "already closed: release sends nothing";
}

TEST_P(TcpClose, ReceivedResetThenLateSegmentsGetRstAndRstGetsSilence) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(text("kept")));
    receive(peer_.rst());
    EXPECT_EQ(c->state(), State::Closed);
    EXPECT_EQ(poll()->events, (Events{Event::Readable, Event::Reset}));
    EXPECT_EQ(c->unread().size(), 4U) << "held segments stay until release";
    EXPECT_EQ(tcp_->counters().connections_reset, 1U);
    EXPECT_TRUE(collect().empty());
    receive(peer_.data(text("late")));
    EXPECT_EQ(tcp_->counters().dropped_closed, 1U);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_TRUE(frames[0].tcp->flags.has(TcpFlag::Rst));
    receive(peer_.rst());
    EXPECT_EQ(tcp_->counters().dropped_closed, 2U);
    EXPECT_TRUE(collect().empty()) << "a RST to a closed entry is discarded, never answered";
    EXPECT_EQ(tcp_->pending_events(), 0U);
}

TEST_P(TcpClose, ReleaseOfAnOpenConnectionResetsAndFreesWithNoEvent) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(text("x")));
    c->release();
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_TRUE(frames[0].tcp->flags.has(TcpFlag::Rst));
    EXPECT_EQ(tcp_->table_size(), 0U);
    EXPECT_EQ(tcp_->pending_events(), 0U) << "the Closed raised by the abort went with the slot";
    EXPECT_EQ(wheel_.pending(), 0U);
    advance(70000ms);
    EXPECT_EQ(tcp_->pending_events(), 0U) << "a released connection raises nothing, ever";
    EXPECT_TRUE(collect().empty());
}

TEST_P(TcpCloseRefusing, FinalAckSurvivesRefusalAndClosedWaitsForIt) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    c->close();
    const auto fin = collect_queued();
    ASSERT_EQ(fin.size(), 1U);
    receive(peer_.ack(fin[0]));
    EXPECT_EQ(c->state(), State::FinWait2);
    std::ignore = poll();
    refuse();
    receive(peer_.fin());
    EXPECT_EQ(c->state(), State::FinWait2) << "the final transition waits until its ACK can be queued";
    EXPECT_EQ(poll()->events, Events{Event::PeerClosed}) << "PeerClosed now; Closed only with the ACK";
    EXPECT_EQ(tcp_->counters().send_refused, 1U);
    allow();
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "flush retried the terminal ACK";
    EXPECT_EQ(frames[0].tcp->acknowledgement, peer_.snd_nxt());
    EXPECT_EQ(c->state(), State::Closed);
    EXPECT_EQ(poll()->events, Events{Event::Closed});
    EXPECT_EQ(tcp_->counters().connections_closed, 1U);
}
```

`tests/unit_tests/common/tcp/test_tcp_two_stacks.cpp`:

```cpp
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/tcp>
#include <aloe/wire>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <span>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#include <net_fixture.hpp>

// Two real bricks on two fabric ports, one thread, a hand-written loop each: the echo the docs lead with,
// with no runtime, no scripted peer and every counter accounted for.
namespace {

    using namespace std::chrono_literals;
    using Ipv4      = aloe::net::Ipv4<aloe::fabric::Port>;
    using Tcp       = aloe::tcp::Stack<Ipv4>;
    using TimePoint = aloe::core::TimePoint;

    constexpr aloe::wire::MacAddress server_mac{0x02, 0, 0, 0, 0, 0x0a};
    constexpr aloe::wire::MacAddress client_mac{0x02, 0, 0, 0, 0, 0x0b};
    constexpr aloe::wire::Ipv4Address server_ip{10, 0, 0, 10};
    constexpr aloe::wire::Ipv4Address client_ip{10, 0, 0, 11};
    constexpr std::size_t message = 5000;  ///< More than three segments at MSS 1460.

    /// One brick with its loop state: what a program writes once per shard.
    struct Brick {
        Brick(aloe::fabric::Port& port, const aloe::wire::Ipv4Address address)
            : queue{port, 0, 64, counters},
              ip{queue, {.address = address, .prefix = 24}},
              tcp{ip, wheel, {}} {
        }

        aloe::loop::ShardCounters counters;
        aloe::loop::ShardQueue<aloe::fabric::Port> queue;
        Ipv4 ip;
        aloe::loop::TimerWheel wheel{1ms, TimePoint{}};
        Tcp tcp;
        std::vector<aloe::fabric::Packet> burst = std::vector<aloe::fabric::Packet>(64);

        /// Receive, process, timers: the first half of a tick; the test drains events in between, then `flush`.
        void receive(const TimePoint now) {
            const std::size_t received = queue.receive(burst);
            ip.process(std::span<aloe::fabric::Packet>{burst}.first(received), now);
            tcp.process(ip.received(aloe::wire::Ipv4Protocol::Tcp), now);
            std::ignore = wheel.advance(now);
        }

        void flush(const TimePoint now) {
            tcp.flush(now);
            std::ignore = queue.flush();
        }
    };

    class TcpTwoStacks : public ::testing::TestWithParam<aloe::fabric::EmulatedOffloads> {
    protected:
        aloe::fabric::Fabric fabric_;
        aloe::fabric::Port& server_port_ = fabric_.add_port({.mac = server_mac, .pool_size = 256, .offloads = GetParam()});
        aloe::fabric::Port& client_port_ = fabric_.add_port({.mac = client_mac, .pool_size = 256, .offloads = GetParam()});
        Brick server_{server_port_, server_ip};
        Brick client_{client_port_, client_ip};
        TimePoint now_{};
    };

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpTwoStacks,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

    /// The echo's own copy: unread bytes go back out through prepare and commit, in place on the way out.
    void echo(Tcp::ConnectionType& c) {
        while (!c.unread().empty()) {
            const std::span<const std::byte> chunk = c.unread().front();
            const auto out                         = c.prepare(chunk.size());
            if (!out) {
                return;  // resume on Writable
            }
            std::ranges::copy(chunk.first(out->size()), out->begin());
            if (!c.commit(out->size())) {
                return;
            }
            c.consume(out->size());
        }
    }

}  // namespace

TEST_P(TcpTwoStacks, EchoAcrossMssAndClose) {
    server_.ip.learn(client_ip, client_mac, now_);
    client_.ip.learn(server_ip, server_mac, now_);
    ASSERT_TRUE(server_.tcp.listen(7).has_value());
    const auto sent = aloe::frames::pattern(message);
    std::vector<std::byte> echoed;
    std::size_t offered = 0;
    bool server_done = false;
    bool client_done = false;

    const auto session = client_.tcp.connect({.address = server_ip, .port = 7}, now_);
    ASSERT_TRUE(session.has_value());
    Tcp::ConnectionType& c = **session;

    for (int tick = 0; tick < 200 && !(server_done && client_done); ++tick) {
        now_ += 1ms;
        server_.receive(now_);
        while (const auto event = server_.tcp.poll_event()) {
            auto& s = *event->connection;
            if (event->events.readable() || event->events.writable()) {
                echo(s);
            }
            if (s.peer_closed() && s.unread().empty() && s.state() == aloe::tcp::State::CloseWait) {
                s.close();
            }
            if (event->events.closed() || event->events.reset() || event->events.timed_out()) {
                s.release();
                server_done = true;
            }
        }
        server_.flush(now_);

        client_.receive(now_);
        while (const auto event = client_.tcp.poll_event()) {
            if (event->events.connected() || event->events.writable()) {
                offered += c.send(std::span<const std::byte>{sent}.subspan(offered));
            }
            if (event->events.readable()) {
                for (const std::span<const std::byte> chunk : c.unread()) {
                    echoed.insert(echoed.end(), chunk.begin(), chunk.end());
                }
                c.consume(c.unread().size());
                if (echoed.size() == message) {
                    c.close();
                }
            }
            if (event->events.closed() || event->events.reset() || event->events.timed_out()) {
                c.release();
                client_done = true;
            }
        }
        client_.flush(now_);
    }

    EXPECT_TRUE(server_done && client_done) << "both sides closed within 200 ticks";
    EXPECT_EQ(echoed, sent);
    EXPECT_EQ(offered, message);
    const auto& sc = server_.tcp.counters();
    const auto& cc = client_.tcp.counters();
    EXPECT_EQ(sc.connections_accepted, 1U);
    EXPECT_EQ(cc.connections_opened, 1U);
    EXPECT_EQ(sc.connections_closed, 1U);
    EXPECT_EQ(cc.connections_closed, 1U);
    EXPECT_EQ(sc.dropped_no_connection, 0U);
    EXPECT_EQ(cc.dropped_no_connection, 0U);
    EXPECT_EQ(sc.retransmits, 0U);
    EXPECT_EQ(cc.retransmits, 0U);
    EXPECT_EQ(sc.resets_sent + cc.resets_sent, 0U);
    EXPECT_GE(cc.data_segments_sent, 4U) << "5000 bytes is at least four segments at MSS 1460";
    EXPECT_EQ(sc.data_segments_sent, cc.data_segments_sent) << "one echo segment per received segment";
    EXPECT_EQ(sc.dropped_bad_checksum + cc.dropped_bad_checksum, 0U);
    EXPECT_EQ(sc.dropped_out_of_window + cc.dropped_out_of_window, 0U);
    EXPECT_EQ(sc.dropped_no_slot + sc.dropped_no_node, 0U);
    EXPECT_EQ(server_.tcp.table_size() + client_.tcp.table_size(), 0U);
    EXPECT_EQ(server_.wheel.pending() + client_.wheel.pending(), 0U);
}
```

`tests/unit_tests/common/tcp/test_tcp_placement.cpp`:

```cpp
#include <aloe/device>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#include <net_fixture.hpp>
#include <tcp_test_device.hpp>

// A four-queue port with one brick per queue, a peer port opposite: every connection, inbound or outbound, is
// served by the queue the card's hash selects, and nothing lands on a brick that does not know it.
namespace {

    using namespace std::chrono_literals;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpSequence;
    using Ipv4      = aloe::net::Ipv4<aloe::fabric::Port>;
    using Tcp       = aloe::tcp::Stack<Ipv4>;
    using TimePoint = aloe::core::TimePoint;

    constexpr std::uint16_t queues = 4;
    constexpr std::size_t flows    = 24;
    constexpr aloe::wire::MacAddress stack_mac{0x02, 0, 0, 0, 0, 0x01};
    constexpr aloe::wire::MacAddress peer_mac{0x02, 0, 0, 0, 0, 0x02};
    constexpr aloe::wire::Ipv4Address stack_ip{10, 0, 0, 2};
    constexpr aloe::wire::Ipv4Address peer_ip{10, 0, 0, 1};

    struct Brick {
        Brick(aloe::fabric::Port& port, const std::uint16_t index)
            : queue{port, index, 64, counters},
              ip{queue, {.address = stack_ip, .prefix = 24}},
              tcp{ip, wheel, {}} {
            ip.learn(peer_ip, peer_mac, TimePoint{});
        }

        aloe::loop::ShardCounters counters;
        aloe::loop::ShardQueue<aloe::fabric::Port> queue;
        Ipv4 ip;
        aloe::loop::TimerWheel wheel{1ms, TimePoint{}};
        Tcp tcp;
        std::vector<aloe::fabric::Packet> burst = std::vector<aloe::fabric::Packet>(64);

        void tick(const TimePoint now) {
            const std::size_t received = queue.receive(burst);
            ip.process(std::span<aloe::fabric::Packet>{burst}.first(received), now);
            tcp.process(ip.received(aloe::wire::Ipv4Protocol::Tcp), now);
            std::ignore = wheel.advance(now);
            tcp.flush(now);
            std::ignore = queue.flush();
        }
    };

    [[nodiscard]] aloe::frames::TcpSpec peer_spec(const std::uint16_t peer_port) {
        return {.destination_mac  = stack_mac,
                .source_mac       = peer_mac,
                .source           = peer_ip,
                .destination      = stack_ip,
                .source_port      = peer_port,
                .destination_port = 7,
                .mss              = 1460};
    }

    class TcpPlacement : public ::testing::Test {
    protected:
        aloe::fabric::Fabric fabric_;
        aloe::fabric::Port& port_ = fabric_.add_port({.mac = stack_mac, .queues = queues, .pool_size = 128});
        aloe::fabric::Port& peer_ = fabric_.add_port({.mac = peer_mac, .pool_size = 128, .queue_depth = 4096});
        std::vector<std::unique_ptr<Brick>> bricks_;
        TimePoint now_{};

        void SetUp() override {
            ASSERT_TRUE(port_.steering().enabled);
            for (std::uint16_t index = 0; index < queues; ++index) {
                bricks_.push_back(std::make_unique<Brick>(port_, index));
                ASSERT_TRUE(bricks_.back()->tcp.listen(7).has_value());
            }
        }

        void tick_all() {
            now_ += 1ms;
            for (auto& brick : bricks_) {
                brick->tick(now_);
            }
        }

        void inject(const std::span<const std::byte> frame) {
            auto packet = peer_.allocate(0);
            ASSERT_TRUE(packet.has_value());
            ASSERT_TRUE(aloe::frames::fill(*packet, frame));
            std::array<aloe::fabric::Packet, 1> out{std::move(*packet)};
            ASSERT_EQ(peer_.transmit(0, out), 1U);
        }

        [[nodiscard]] std::vector<aloe::frames::ParsedFrame> peer_received() {
            std::vector<aloe::frames::ParsedFrame> frames;
            std::array<aloe::fabric::Packet, 16> out;
            for (std::size_t count = peer_.receive(0, out); count > 0; count = peer_.receive(0, out)) {
                for (std::size_t index = 0; index < count; ++index) {
                    frames.push_back(aloe::frames::parse_frame(aloe::frames::bytes_of(out[index])).value());
                    out[index] = aloe::fabric::Packet{};
                }
            }
            return frames;
        }

        /// The queue the card delivers a segment from the peer at `peer_port` to our `local_port` to.
        [[nodiscard]] std::uint16_t expected_queue(const std::uint16_t peer_port, const std::uint16_t local_port) const {
            return aloe::device::queue_for(port_.steering(),
                                           {.source           = peer_ip,
                                            .destination      = stack_ip,
                                            .source_port      = peer_port,
                                            .destination_port = local_port,
                                            .protocol         = aloe::wire::Ipv4Protocol::Tcp});
        }

        [[nodiscard]] std::uint64_t stray_drops() const {
            std::uint64_t drops = 0;
            for (const auto& brick : bricks_) {
                drops += brick->tcp.counters().dropped_no_connection + brick->tcp.counters().dropped_unexpected;
            }
            return drops;
        }
    };

}  // namespace

TEST_F(TcpPlacement, InboundSynsAreTakenByTheHashedQueue) {
    std::vector<aloe::frames::TcpPeer> peers;
    for (std::size_t flow = 0; flow < flows; ++flow) {
        peers.emplace_back(peer_spec(static_cast<std::uint16_t>(40000 + flow)), TcpSequence{1000U});
        inject(peers.back().syn());
    }
    tick_all();
    const auto syn_acks = peer_received();
    ASSERT_EQ(syn_acks.size(), flows);
    std::vector<std::uint16_t> owners(flows, queues);
    for (const auto& frame : syn_acks) {
        const std::size_t flow = frame.tcp->destination_port - 40000U;
        peers[flow].see(frame);
        inject(peers[flow].ack());
    }
    tick_all();
    std::size_t accepted = 0;
    for (std::uint16_t index = 0; index < queues; ++index) {
        while (const auto event = bricks_[index]->tcp.poll_event()) {
            ASSERT_TRUE(event->events.accepted());
            const std::size_t flow = event->connection->remote().port - 40000U;
            owners[flow]           = index;
            ++accepted;
        }
    }
    EXPECT_EQ(accepted, flows);
    for (std::size_t flow = 0; flow < flows; ++flow) {
        EXPECT_EQ(owners[flow], expected_queue(static_cast<std::uint16_t>(40000 + flow), 7)) << "flow " << flow;
    }
    std::set<std::uint16_t> used(owners.begin(), owners.end());
    EXPECT_EQ(used.size(), queues) << "24 flows exercise every queue";
    EXPECT_EQ(stray_drops(), 0U);
}

TEST_F(TcpPlacement, OutboundConnectsChoosePortsWhoseRepliesComeBackToTheirQueue) {
    std::vector<std::pair<std::uint16_t, std::uint16_t>> opened;  // queue, local port
    for (std::size_t flow = 0; flow < flows; ++flow) {
        const auto index     = static_cast<std::uint16_t>(flow % queues);
        const auto connected = bricks_[index]->tcp.connect({.address = peer_ip, .port = 7}, now_);
        ASSERT_TRUE(connected.has_value()) << "flow " << flow;
        opened.emplace_back(index, (*connected)->local().port);
        EXPECT_EQ(expected_queue(7, (*connected)->local().port), index) << "the port was chosen for this queue";
    }
    tick_all();
    const auto syns = peer_received();
    ASSERT_EQ(syns.size(), flows);
    std::vector<aloe::frames::TcpPeer> peers;
    for (const auto& syn : syns) {
        aloe::frames::TcpPeer peer{peer_spec(7), TcpSequence{2000U}};
        inject(peer.syn_ack(syn));
        peers.push_back(peer);
    }
    tick_all();
    std::size_t connected = 0;
    for (std::uint16_t index = 0; index < queues; ++index) {
        while (const auto event = bricks_[index]->tcp.poll_event()) {
            EXPECT_TRUE(event->events.connected());
            const auto found = std::ranges::find(opened, std::pair{index, event->connection->local().port});
            EXPECT_NE(found, opened.end()) << "the reply reached the brick that opened";
            ++connected;
        }
    }
    EXPECT_EQ(connected, flows);
    EXPECT_EQ(stray_drops(), 0U);
    EXPECT_EQ(peer_received().size(), flows) << "one handshake ACK per connection";
}

TEST_F(TcpPlacement, AddressesOnlyRssIsUnplaceableOffItsQueue) {
    aloe::testing::TcpTestDevice device{port_};
    aloe::device::RssDescription addresses_only = aloe::device::round_robin_rss(queues);
    addresses_only.types.ipv4_tcp               = false;  // addresses only: one queue for every port
    device.steering_override                    = addresses_only;
    const std::uint16_t picked = aloe::device::queue_for(
        addresses_only, {.source = peer_ip, .destination = stack_ip, .protocol = aloe::wire::Ipv4Protocol::Tcp});
    const auto other = static_cast<std::uint16_t>((picked + 1) % queues);
    aloe::loop::ShardCounters counters;
    aloe::loop::ShardQueue<aloe::testing::TcpTestDevice> queue{device, other, 16, counters};
    aloe::net::Ipv4<aloe::testing::TcpTestDevice> ip{queue, {.address = stack_ip, .prefix = 24}};
    aloe::loop::TimerWheel wheel{1ms, TimePoint{}};
    aloe::tcp::Stack<aloe::net::Ipv4<aloe::testing::TcpTestDevice>> tcp{ip, wheel, {}};
    const auto failed = tcp.connect({.address = peer_ip, .port = 7}, now_);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error(), aloe::tcp::ConnectError::Unplaceable);
    EXPECT_EQ(tcp.table_size(), 0U);
    aloe::loop::ShardQueue<aloe::testing::TcpTestDevice> right_queue{device, picked, 16, counters};
    aloe::net::Ipv4<aloe::testing::TcpTestDevice> right_ip{right_queue, {.address = stack_ip, .prefix = 24}};
    aloe::tcp::Stack<aloe::net::Ipv4<aloe::testing::TcpTestDevice>> right{right_ip, wheel, {}};
    right_ip.learn(peer_ip, peer_mac, now_);
    EXPECT_TRUE(right.connect({.address = peer_ip, .port = 7}, now_).has_value()) << "on the picked queue any port does";
}

TEST_F(TcpPlacement, DisabledSteeringCannotReachNonzeroQueue) {
    aloe::testing::TcpTestDevice device{port_};
    device.steering_override = aloe::device::RssDescription{};  // off: every frame lands on queue 0
    aloe::loop::ShardCounters counters;
    aloe::loop::ShardQueue<aloe::testing::TcpTestDevice> queue1{device, 1, 16, counters};
    aloe::net::Ipv4<aloe::testing::TcpTestDevice> ip1{queue1, {.address = stack_ip, .prefix = 24}};
    aloe::loop::TimerWheel wheel{1ms, TimePoint{}};
    aloe::tcp::Stack<aloe::net::Ipv4<aloe::testing::TcpTestDevice>> on_one{ip1, wheel, {}};
    const auto failed = on_one.connect({.address = peer_ip, .port = 7}, now_);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error(), aloe::tcp::ConnectError::Unplaceable);
    aloe::loop::ShardQueue<aloe::testing::TcpTestDevice> queue0{device, 0, 16, counters};
    aloe::net::Ipv4<aloe::testing::TcpTestDevice> ip0{queue0, {.address = stack_ip, .prefix = 24}};
    aloe::tcp::Stack<aloe::net::Ipv4<aloe::testing::TcpTestDevice>> on_zero{ip0, wheel, {}};
    ip0.learn(peer_ip, peer_mac, now_);
    EXPECT_TRUE(on_zero.connect({.address = peer_ip, .port = 7}, now_).has_value());
}

TEST_F(TcpPlacement, CursorWrapsSkipsListenersAndOccupiedTuplesThenNoPort) {
    aloe::fabric::Port& single = fabric_.add_port({.mac = {0x02, 0, 0, 0, 0, 0x03}, .pool_size = 64});
    aloe::loop::ShardCounters counters;
    aloe::loop::ShardQueue<aloe::fabric::Port> queue{single, 0, 16, counters};
    Ipv4 ip{queue, {.address = {10, 0, 0, 3}, .prefix = 24}};
    ip.learn(peer_ip, peer_mac, now_);
    aloe::loop::TimerWheel wheel{1ms, TimePoint{}};
    Tcp tcp{ip, wheel, {.ephemeral_first = 40000, .ephemeral_last = 40002}};
    ASSERT_TRUE(tcp.listen(40001).has_value());
    const auto first = tcp.connect({.address = peer_ip, .port = 7}, now_);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ((*first)->local().port, 40000U);
    const auto second = tcp.connect({.address = peer_ip, .port = 7}, now_);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ((*second)->local().port, 40002U) << "40001 is a listener";
    const auto third = tcp.connect({.address = peer_ip, .port = 7}, now_);
    ASSERT_FALSE(third.has_value());
    EXPECT_EQ(third.error(), aloe::tcp::ConnectError::NoPort) << "the cursor wrapped to its start";
    const auto elsewhere = tcp.connect({.address = peer_ip, .port = 8}, now_);
    ASSERT_TRUE(elsewhere.has_value()) << "another peer port: the tuples are free";
    EXPECT_EQ((*elsewhere)->local().port, 40000U);
    (*first)->release();
    const auto reused = tcp.connect({.address = peer_ip, .port = 7}, now_);
    ASSERT_TRUE(reused.has_value());
    EXPECT_EQ((*reused)->local().port, 40000U) << "freed and taken again";
    Tcp tiny{ip, wheel, {.connections = 1, .ephemeral_first = 50000, .ephemeral_last = 50001}};
    ASSERT_TRUE(tiny.connect({.address = peer_ip, .port = 7}, now_).has_value());
    const auto full = tiny.connect({.address = peer_ip, .port = 7}, now_);
    ASSERT_FALSE(full.has_value());
    EXPECT_EQ(full.error(), aloe::tcp::ConnectError::TableFull);
    for (auto* c : {*second, *elsewhere, *reused}) {
        c->release();
    }
    std::ignore = queue.flush();
}
```

Add the three files to `Aloe.Tests.Unit.Tcp`; `test_tcp_placement.cpp` needs `<set>` and `<utility>`.

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Tcp`
Expected: link errors for `Stack<...>::close`.

- [ ] **Step 3: Write `close`**

Append to `common/tcp/stack/tcp_control.hpp`, inside the namespace:

```cpp
    /// FIN after what was committed. Established moves to FinWait1, CloseWait to LastAck; elsewhere nothing.
    template <typename Ip>
    void Stack<Ip>::close(ConnectionType& c) noexcept {
        switch (c.state_) {
            case State::Established:
                c.state_ = State::FinWait1;
                break;
            case State::CloseWait:
                c.state_ = State::LastAck;
                break;
            default:
                return;
        }
        c.prepared_.reset();  // nothing more is sent after the FIN
        c.prepared_size_ = 0;
        c.fin_sequence_  = c.snd_nxt_;
        c.fin_sent_      = true;
        const auto sent  = send_control(c, wire::TcpFlags{wire::TcpFlag::Fin, wire::TcpFlag::Ack}, c.fin_sequence_);
        if (sent) {
            ++counters_.control_segments_sent;
        }
        c.snd_nxt_         = c.snd_nxt_ + 1;  // the FIN takes its number whether or not it left: the timer resends it
        c.tries_           = 0;
        c.unresolved_wait_ = !sent && sent.error() == net::SendError::Unresolved;
        arm_retry(c);
    }
```

- [ ] **Step 4: Run the brick gate**

Run: `cmake --build --preset debug && ctest --preset debug -R '^Aloe[.]Tests[.]Unit[.](Tcp|Stream|Frames|Net|Loop.*)$' --output-on-failure`
Expected: PASS. The two-brick echo completes with no runtime dependency; the placement test exercises all four queues. Then build and run `gcc-debug`, `asan` and `tsan` for the same targets.

- [ ] **Step 5: Start `docs/architecture/tcp.md`**

Write the page in the module pattern. "What it is": the brick, its four verbs, one per shard. "Key types": `Stack<Ip>`, `Connection<Ip>`, `ConnectionEvent<Ip>`, `ReadView<Packet>`, `TcpConfig`, `TcpCounters`, `FlowTable`, the errors and states, in the concrete spellings of this plan. "Usage": the hand-written loop from the spec with the real names (the one in `test_tcp_two_stacks.cpp`), the explicit `poll_event` drain, the echo's own copy explained, and that an application producing new messages encodes straight into `prepare`. "Design notes": zero-copy both ways; the chain and the byte window with independent packet caps, the non-retreating edge and what happens at a cap (dropped without acknowledgement, recovered by the peer's retransmission, never by retracting credit); the ACK exceptions; the ownership boundaries as logical state; the table's separate bucket hash and placement by port choice, with `Unplaceable` for addresses-only RSS and disabled steering off queue 0; the control retry schedule (1, 3, 7, 15, 31 seconds, timeout at 63) and the 10 ms unresolved poll; the terminal ACK before `Closed`; a comparison with Linux, Seastar and TLDK in a few lines; and what phase 1 leaves out and why: no data retransmission, no out-of-order storage, no TIME_WAIT, no SACK, scaling or timestamps, no congestion control, no deployment or latency claim. Task 14 completes the page.

- [ ] **Step 6: Format and commit**

```bash
./scripts/check-format.sh
git add common/tcp tests/unit_tests/common/tcp docs/architecture/tcp.md
git commit -m "feat(tcp): close streams and place connections on their owning queues"
```

## Task 10: Runtime tick hooks, sibling views and guarded control posting

**Files:**
- Modify: `common/runtime/context/shard_context.hpp`, `common/runtime/context/shard_context.cpp`, `common/runtime/shard/shard.hpp`, `common/runtime/runtime/runtime.hpp`
- Modify: `tests/unit_tests/common/runtime/test_shard.cpp`, `test_shard_context.cpp`, `test_runtime.cpp`, `test_cross_shard.cpp`, `docs/architecture/runtime.md`

**Interfaces:**
- Produces on `ShardContext`: `void set_now(core::TimePoint) noexcept`; `std::span<ShardContext* const> siblings() const noexcept` (every other context, empty for a standalone shard, stable while running); setup-only `void set_siblings(std::span<ShardContext* const>)`; `bool post_control(loop::Work&) noexcept` from any thread, true when the node was accepted and will run once, false when the context has closed admission and the node stays the caller's; `bool try_finish() noexcept` on the owner, false without a stop request, else locks, rechecks `drained()` and closes admission atomically with a successful check; `struct ArpSink{object, learn}` with `set_arp_sink` (setup and teardown) and `void deliver_arp(wire::Ipv4Address, wire::MacAddress) noexcept` (owner). `drained()` stays an observational query.
- Produces on `Shard`: the step order of the spec with optional `on_tick(now)` and `on_flush(now)` detected independently (`detail::HasOnTick<S>`, `detail::HasOnFlush<S>`); `run()` exits through `try_finish`.
- Produces on `Runtime`: siblings bound after construction and before `start`; `stop` posts each owned `StopWork` through `post_control` and keeps a rejected node; a thread whose hook fails requests stop on its context, drains the control posts already accepted through `run_once` and closes admission before reporting the failure, with no device polling.

- [ ] **Step 1: Write the failing tests**

Add to `tests/unit_tests/common/runtime/test_shard.cpp`, in the anonymous namespace:

```cpp
    /// A stack that records what the shard calls, in order, with the stamps it sees; `on_tick` queues work.
    class HookStack {
    public:
        using Packet = aloe::fabric::Packet;

        struct Ready : aloe::loop::Work {
            explicit Ready(HookStack* owner) noexcept
                : aloe::loop::Work{&Ready::run_it},
                  stack{owner} {
            }

            static void run_it(aloe::loop::Work& work) noexcept {
                auto& self = static_cast<Ready&>(work);
                self.stack->order.emplace_back("ready");
                self.stack->pending_at_ready = self.stack->queue_->pending();
                self.stack->transmit_one();
            }

            HookStack* stack;
        };

        HookStack(aloe::runtime::ShardContext& context, aloe::loop::ShardQueue<aloe::fabric::Port>& queue) noexcept
            : context_{&context},
              queue_{&queue},
              ready_{this} {
        }

        void on_receive(std::span<Packet> burst) noexcept {
            order.emplace_back("receive");
            stamps.push_back(context_->now());
            received += burst.size();
        }

        void on_tick(const aloe::core::TimePoint now) noexcept {
            order.emplace_back("tick");
            stamps.push_back(now);
            transmit_one();                   // leaves at the early flush, before tasks run
            context_->ready().push(ready_);   // runs inside run_once, this step
        }

        void on_flush(const aloe::core::TimePoint now) noexcept {
            order.emplace_back("flush");
            stamps.push_back(now);
            pending_at_flush = queue_->pending();  // the ready work's frame is still in the ring
        }

        void transmit_one() noexcept {
            auto packet = queue_->allocate();
            if (packet && aloe::frames::fill(*packet, aloe::frames::ethernet_frame(client, server, 0x88b5, aloe::frames::pattern(20)))) {
                std::ignore = queue_->transmit(std::move(*packet));
            }
        }

        std::vector<std::string> order;
        std::vector<aloe::core::TimePoint> stamps;
        std::size_t received         = 0;
        std::size_t pending_at_ready = 99;
        std::size_t pending_at_flush = 99;

    private:
        aloe::runtime::ShardContext* context_;
        aloe::loop::ShardQueue<aloe::fabric::Port>* queue_;
        Ready ready_;
    };

    class TickOnly {
    public:
        using Packet = aloe::fabric::Packet;
        TickOnly(aloe::runtime::ShardContext&, aloe::loop::ShardQueue<aloe::fabric::Port>&) noexcept {}
        void on_receive(std::span<Packet>) noexcept {}
        void on_tick(aloe::core::TimePoint) noexcept { ++ticks; }
        int ticks = 0;
    };

    class FlushOnly {
    public:
        using Packet = aloe::fabric::Packet;
        FlushOnly(aloe::runtime::ShardContext&, aloe::loop::ShardQueue<aloe::fabric::Port>&) noexcept {}
        void on_receive(std::span<Packet>) noexcept {}
        void on_flush(aloe::core::TimePoint) noexcept { ++flushes; }
        int flushes = 0;
    };

    static_assert(aloe::runtime::IsStack<HookStack, aloe::fabric::Port>);
    static_assert(aloe::runtime::IsStack<TickOnly, aloe::fabric::Port>);
    static_assert(aloe::runtime::IsStack<FlushOnly, aloe::fabric::Port>);
    static_assert(aloe::runtime::detail::HasOnTick<HookStack> && aloe::runtime::detail::HasOnFlush<HookStack>);
    static_assert(aloe::runtime::detail::HasOnTick<TickOnly> && !aloe::runtime::detail::HasOnFlush<TickOnly>);
    static_assert(!aloe::runtime::detail::HasOnTick<FlushOnly> && aloe::runtime::detail::HasOnFlush<FlushOnly>);
```

and the tests:

```cpp
TEST_F(ShardTest, TickHooksSeeCurrentStampAndRunInOrder) {
    aloe::runtime::Shard<aloe::fabric::Port, HookStack> shard{config_, server_, 0, start};
    send(client_, aloe::frames::ethernet_frame(server, client, aloe::frames::ethertype_experimental, aloe::frames::pattern(30)));
    EXPECT_TRUE(shard.step(start + 7ms));
    EXPECT_EQ(shard.stack().order, (std::vector<std::string>{"receive", "tick", "ready", "flush"}));
    EXPECT_EQ(shard.stack().stamps, (std::vector<TimePoint>{start + 7ms, start + 7ms, start + 7ms}))
        << "on_receive sees the step's stamp through the context, before run_once";
    EXPECT_EQ(shard.stack().received, 1U);
    EXPECT_EQ(shard.stack().pending_at_ready, 0U) << "what on_tick queued left at the early flush";
    EXPECT_EQ(shard.stack().pending_at_flush, 1U) << "what the ready work queued is still in the ring at on_flush";
    EXPECT_EQ(shard.queue().pending(), 0U) << "and leaves at the final flush";
    EXPECT_EQ(drain(client_).size(), 2U);
}

TEST_F(ShardTest, EmptyReceiveTicksStillRunBothHooks) {
    aloe::runtime::Shard<aloe::fabric::Port, HookStack> shard{config_, server_, 0, start};
    EXPECT_TRUE(shard.step(start + 1ms)) << "the hook sent something";
    EXPECT_EQ(shard.stack().order, (std::vector<std::string>{"tick", "ready", "flush"}));
    EXPECT_EQ(shard.context().now(), start + 1ms);
}

TEST_F(ShardTest, WorkQueuedByReadyWorkWaitsForTheNextStep) {
    aloe::runtime::Shard<aloe::fabric::Port, HookStack> shard{config_, server_, 0, start};
    std::ignore = shard.step(start + 1ms);
    EXPECT_EQ(shard.context().counters().work_run, 1U);
    std::ignore = shard.step(start + 2ms);
    EXPECT_EQ(shard.context().counters().work_run, 2U) << "one chain per step, never a recursive drain";
}

TEST_F(ShardTest, OptionalHooksAreIndependent) {
    aloe::runtime::Shard<aloe::fabric::Port, TickOnly> ticking{config_, server_, 0, start};
    aloe::runtime::Shard<aloe::fabric::Port, FlushOnly> flushing{config_, server_, 0, start};
    aloe::runtime::Shard<aloe::fabric::Port, aloe::testing::EchoStack<aloe::fabric::Port>> neither{config_, server_, 0, start};
    std::ignore = ticking.step(start);
    std::ignore = flushing.step(start);
    EXPECT_FALSE(neither.step(start)) << "unchanged: an empty step is idle";
    EXPECT_EQ(ticking.stack().ticks, 1);
    EXPECT_EQ(flushing.stack().flushes, 1);
    EXPECT_TRUE(ticking.context().siblings().empty()) << "a standalone shard has no siblings";
}
```

Add to `tests/unit_tests/common/runtime/test_shard_context.cpp`:

```cpp
namespace {
    struct Counting : aloe::loop::Work {
        Counting() noexcept
            : aloe::loop::Work{&Counting::run_it} {
        }
        static void run_it(aloe::loop::Work& work) noexcept {
            ++static_cast<Counting&>(work).runs;
        }
        int runs = 0;
    };
}  // namespace

TEST_F(ShardContextTest, SetNowRecordsTheStampWithoutRunningAnything) {
    Counting work;
    context_.ready().push(work);
    context_.set_now(start + 5ms);
    EXPECT_EQ(context_.now(), start + 5ms);
    EXPECT_EQ(work.runs, 0);
    EXPECT_EQ(context_.counters().work_run, 0U);
}

TEST_F(ShardContextTest, ControlPostAcceptedBeforeDrainOrRejectedAfterClosure) {
    Counting first;
    Counting second;
    EXPECT_FALSE(context_.try_finish()) << "no stop requested: nothing to finish, no lock taken";
    EXPECT_TRUE(context_.post_control(first));
    EXPECT_FALSE(context_.drained());
    EXPECT_TRUE(context_.run_once(start));
    EXPECT_EQ(first.runs, 1);
    {
        const aloe::runtime::ShardContext::Current current{context_};
        context_.request_stop();
    }
    EXPECT_TRUE(context_.post_control(second)) << "stop requested but not yet finished: still accepted";
    EXPECT_FALSE(context_.try_finish()) << "the accepted node keeps the context open";
    EXPECT_TRUE(context_.run_once(start));
    EXPECT_EQ(second.runs, 1);
    EXPECT_TRUE(context_.try_finish());
    Counting late;
    EXPECT_FALSE(context_.post_control(late)) << "closed: the node stays the caller's";
    EXPECT_EQ(late.runs, 0);
    EXPECT_FALSE(context_.run_once(start));
}

TEST_F(ShardContextTest, ArpSinkDeliversOnTheOwnerWithTheStamp) {
    struct Learned {
        aloe::wire::Ipv4Address address;
        aloe::wire::MacAddress mac;
        TimePoint at;
        int calls = 0;
    } learned{};
    context_.set_arp_sink({.object = &learned,
                           .learn  = [](void* object,
                                       const aloe::wire::Ipv4Address address,
                                       const aloe::wire::MacAddress mac,
                                       const TimePoint now) noexcept {
                               auto& self   = *static_cast<Learned*>(object);
                               self.address = address;
                               self.mac     = mac;
                               self.at      = now;
                               ++self.calls;
                           }});
    context_.set_now(start + 3ms);
    context_.deliver_arp({10, 0, 0, 254}, {0x02, 0, 0, 0, 0, 0xfe});
    EXPECT_EQ(learned.calls, 1);
    EXPECT_EQ(learned.address, (aloe::wire::Ipv4Address{10, 0, 0, 254}));
    EXPECT_EQ(learned.at, start + 3ms);
    context_.set_arp_sink({});
    context_.deliver_arp({10, 0, 0, 1}, {});
    EXPECT_EQ(learned.calls, 1) << "no sink: discarded";
}
```

Add to `tests/unit_tests/common/runtime/test_runtime.cpp`:

```cpp
TEST_F(RuntimeTest, SiblingsExcludeSelfAndAreBoundBeforeStart) {
    EchoRuntime runtime{config_, server_};
    for (std::uint16_t index = 0; index < queues; ++index) {
        const auto siblings = runtime.shard(index).context().siblings();
        EXPECT_EQ(siblings.size(), static_cast<std::size_t>(queues - 1));
        for (const aloe::runtime::ShardContext* sibling : siblings) {
            EXPECT_NE(sibling, &runtime.shard(index).context());
        }
    }
}

TEST_F(RuntimeTest, AFailedHookClosesControlOnEveryShardWithoutDeadlock) {
    std::atomic<int> hook_calls{0};
    config_.thread_hook = [&hook_calls] {
        if (++hook_calls == 2) {
            throw std::runtime_error{"second hook fails"};
        }
    };
    EchoRuntime runtime{config_, server_};
    EXPECT_THROW(runtime.start(), aloe::runtime::RuntimeError);
    struct Never : aloe::loop::Work {
        Never() noexcept
            : aloe::loop::Work{[](aloe::loop::Work&) noexcept {}} {
        }
    } never;
    for (std::uint16_t index = 0; index < queues; ++index) {
        EXPECT_FALSE(runtime.shard(index).context().post_control(never)) << "shard " << index << " closed admission";
    }
}

TEST_F(RuntimeTest, StopAfterAShardFinishedKeepsItsNode) {
    EchoRuntime runtime{config_, server_};
    runtime.start();
    runtime.stop();
    runtime.join();
    runtime.stop();  // idempotent, and every context is closed: the owned nodes are retained, nothing is pushed
    SUCCEED();
}
```

Add to `tests/unit_tests/common/runtime/test_cross_shard.cpp` (the `Runtime.Threads` target):

```cpp
// A producer posts control work to a running shard until the shard closes admission; every accepted node runs
// exactly once, every rejected node stays with the producer, and nothing is posted to an abandoned inbox.
TEST(ControlPost, AcceptedNodesRunOnceAndRejectedNodesStayWithTheProducer) {
    aloe::fabric::Fabric fabric;
    auto& port = fabric.add_port({.mac = server, .queues = 1, .pool_size = 16});
    aloe::runtime::Shard<aloe::fabric::Port, aloe::testing::EchoStack<aloe::fabric::Port>> shard{
        {.idle = aloe::runtime::IdlePolicy::Yield, .yield_after = 1}, port, 0, std::chrono::steady_clock::now()};

    struct Node : aloe::loop::Work {
        explicit Node(std::atomic<int>* counter) noexcept
            : aloe::loop::Work{&Node::run_it},
              ran{counter} {
        }
        static void run_it(aloe::loop::Work& work) noexcept {
            static_cast<Node&>(work).ran->fetch_add(1);
        }
        std::atomic<int>* ran;
    };
    struct Stop : aloe::loop::Work {
        explicit Stop(aloe::runtime::ShardContext* owner) noexcept
            : aloe::loop::Work{&Stop::run_it},
              context{owner} {
        }
        static void run_it(aloe::loop::Work& work) noexcept {
            static_cast<Stop&>(work).context->request_stop();
        }
        aloe::runtime::ShardContext* context;
    };

    std::atomic<int> ran{0};
    std::latch go{2};
    std::vector<std::unique_ptr<Node>> nodes;
    nodes.reserve(10000);
    int accepted = 0;
    std::jthread runner{[&] {
        go.arrive_and_wait();
        shard.run();
    }};
    go.arrive_and_wait();
    Stop stop{&shard.context()};
    for (int i = 0; i < 10000; ++i) {
        nodes.push_back(std::make_unique<Node>(&ran));
        if (shard.context().post_control(*nodes.back())) {
            ++accepted;
        }
        if (i == 100) {
            ASSERT_TRUE(shard.context().post_control(stop));
        }
        if (i % 50 == 0) {
            std::this_thread::yield();
        }
    }
    runner.join();
    EXPECT_EQ(ran.load(), accepted) << "every accepted node ran exactly once";
    EXPECT_LT(accepted, 10000) << "the shard closed admission after the stop";
    EXPECT_GT(accepted, 100);
    EXPECT_FALSE(shard.context().post_control(*nodes.front())) << "closed for good";
}
```

`<latch>`, `<memory>` and `<vector>` are needed.

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Runtime Aloe.Tests.Unit.Runtime.Threads`
Expected: compile errors, `no member named 'siblings'`, `no member named 'post_control'`, `no member named 'HasOnTick'`.

- [ ] **Step 3: Implement the context, the step and the runtime plumbing**

In `common/runtime/context/shard_context.hpp`, add the includes `<mutex>`, `<span>`, `<vector>`, `<ipv4_address.hpp>`, `<mac_address.hpp>`, and inside the class:

```cpp
        /// A cold-path sink for ARP resolutions learned by another shard; the stack above registers it.
        struct ArpSink {
            void* object = nullptr;
            void (*learn)(void*, wire::Ipv4Address, wire::MacAddress, core::TimePoint) noexcept = nullptr;
        };

        /// Records the step's stamp before any stack callback; runs nothing and advances no timer.
        void set_now(const core::TimePoint now) noexcept {
            now_ = now;
        }

        /// Every other context of the runtime, bound before any thread starts; empty for a standalone shard.
        [[nodiscard]] std::span<ShardContext* const> siblings() const noexcept {
            return siblings_;
        }

        /// Setup only, before `run`: copies the pointers.
        void set_siblings(std::span<ShardContext* const> siblings);

        /**
         * Any thread. Posts a cold-path work node that will run once on this shard, or returns false when
         * the shard has closed admission, in which case the node stays the caller's. The ordinary
         * `Inbox::push` keeps its contract for same-shard and scheduler work; this guarded path is for
         * messages between shards whose producer must know whether the node was taken.
         */
        [[nodiscard]] bool post_control(loop::Work& work) noexcept;

        /**
         * Shard thread. False while no stop is requested, without taking the lock. Otherwise rechecks
         * `drained()` under the control lock and, when it holds, closes admission atomically with the
         * check: no later `post_control` succeeds. `Shard::run` exits on true.
         */
        [[nodiscard]] bool try_finish() noexcept;

        /// Setup and teardown by the stack above. A default sink discards deliveries.
        void set_arp_sink(const ArpSink sink) noexcept {
            arp_sink_ = sink;
        }

        /// Shard thread: hands a resolution to the sink with the current stamp.
        void deliver_arp(wire::Ipv4Address address, wire::MacAddress mac) noexcept;
```

and the members `std::vector<ShardContext*> siblings_;`, `std::mutex control_mutex_;`, `bool control_closed_ = false;`, `ArpSink arp_sink_{};`. The copy and move deletions already exist.

In `common/runtime/context/shard_context.cpp`:

```cpp
    void ShardContext::set_siblings(const std::span<ShardContext* const> siblings) {
        siblings_.assign(siblings.begin(), siblings.end());
    }

    bool ShardContext::post_control(loop::Work& work) noexcept {
        const std::lock_guard lock{control_mutex_};
        if (control_closed_) {
            return false;
        }
        inbox_.push(work);
        return true;
    }

    bool ShardContext::try_finish() noexcept {
        if (!stopping_) {
            return false;
        }
        const std::lock_guard lock{control_mutex_};
        if (!drained()) {
            return false;
        }
        control_closed_ = true;
        return true;
    }

    void ShardContext::deliver_arp(const wire::Ipv4Address address, const wire::MacAddress mac) noexcept {
        if (arp_sink_.learn != nullptr) {
            arp_sink_.learn(arp_sink_.object, address, mac, now_);
        }
    }
```

`<mutex>` is included; `.clang-tidy` does not enable `bugprone-exception-escape`, so the lock inside `noexcept` passes; a lock failure is a programming error and terminates.

In `common/runtime/shard/shard.hpp`, add after `IsStack`:

```cpp
    namespace detail {

        /// Optional hooks of a stack, detected independently; a stack with neither keeps the old tick.
        template <typename S>
        concept HasOnTick = requires(S& stack, core::TimePoint now) {
            { stack.on_tick(now) } -> std::same_as<void>;
        };

        template <typename S>
        concept HasOnFlush = requires(S& stack, core::TimePoint now) {
            { stack.on_flush(now) } -> std::same_as<void>;
        };

    }  // namespace detail
```

Replace `step` and `run`:

```cpp
        /**
         * One tick, in the order the TCP tick needs: the stamp is set before any callback; receive and
         * `on_receive`; `on_tick` (events raised by packets drained, retry hints published); an early
         * flush so control segments and immediate ACKs leave; `run_once` (inbox, timers, one chain of
         * tasks); `on_flush` (events raised by timers and tasks drained, remaining ordinary ACKs
         * emitted); the final flush. Work queued while the chain runs waits for the next step.
         * Returns whether anything was received, run, fired or sent.
         */
        bool step(const core::TimePoint now) noexcept {
            const ShardContext::Current current{context_};
            context_.set_now(now);
            bool busy                  = false;
            const std::size_t received = queue_.receive(burst_);
            if (received > 0) {
                context_.counters().frames_received += received;
                const std::span<Packet> frames{burst_.data(), received};
                stack_.on_receive(frames);
                for (Packet& packet : frames) {
                    if (!packet.empty()) {
                        packet = Packet{};
                    }
                }
                busy = true;
            }
            if constexpr (detail::HasOnTick<Stack>) {
                stack_.on_tick(now);
            }
            busy = queue_.flush() > 0 || busy;
            busy = context_.run_once(now) || busy;
            if constexpr (detail::HasOnFlush<Stack>) {
                stack_.on_flush(now);
            }
            busy = queue_.flush() > 0 || busy;
            ++context_.counters().ticks;
            if (!busy) {
                ++context_.counters().idle_ticks;
            }
            return busy;
        }

        /// Ticks until the context is drained and has closed control admission. Returns with the ring empty.
        void run() noexcept {
            std::uint32_t idle = 0;
            while (!context_.try_finish()) {
                if (step(core::Clock::now())) {
                    idle = 0;
                } else if (config_.idle == IdlePolicy::Yield && ++idle >= config_.yield_after) {
                    std::this_thread::yield();
                }
            }
            std::ignore = queue_.flush();
            queue_.discard();
        }
```

In `common/runtime/runtime/runtime.hpp`: at the end of the constructor, bind the siblings:

```cpp
            std::vector<ShardContext*> contexts;
            contexts.reserve(queues);
            for (const auto& shard : shards_) {
                contexts.push_back(&shard->context());
            }
            for (std::size_t index = 0; index < shards_.size(); ++index) {
                std::vector<ShardContext*> others;
                others.reserve(queues - 1);
                for (std::size_t other = 0; other < contexts.size(); ++other) {
                    if (other != index) {
                        others.push_back(contexts[other]);
                    }
                }
                shards_[index]->context().set_siblings(others);
            }
```

In `stop`, replace the push:

```cpp
        for (std::unique_ptr<StopWork>& pending : stops_) {  // not `stop`: GCC's -Wshadow sees the member function
            std::ignore = pending->context->post_control(*pending);  // rejected: that shard already finished; the node is ours
        }
```

In the thread body's `catch`, before `return`, call `close_control(index);`, and add the private member:

```cpp
        /// A thread whose hook failed never enters `run()`: it drains the control posts a sibling may already have
        /// made and closes admission, so no allocated node is left in an inbox nobody reads. No device polling.
        void close_control(const std::uint16_t index) noexcept {
            ShardContext& context = shards_[index]->context();
            const ShardContext::Current current{context};
            context.request_stop();
            while (!context.try_finish()) {
                std::ignore = context.run_once(core::Clock::now());
            }
        }
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Runtime Aloe.Tests.Unit.Runtime.Threads Aloe.Tests.Integration.Runtime.Ring && ctest --preset debug -R '^Aloe[.]Tests[.](Unit[.]Runtime|Unit[.]Runtime[.]Threads|Integration[.]Runtime[.]Ring)$' --output-on-failure`
Expected: PASS, the existing echo and stop tests unchanged. Then configure `tsan` and run `Aloe.Tests.Unit.Runtime.Threads` under it.

- [ ] **Step 5: Document and format**

In `docs/architecture/runtime.md`: the tick with both hooks and its event-completion timing (an event raised inside `run_once` completes its waiter on the next step, with or without traffic); `set_now`; `siblings()`; the cold control-post ownership rule (`post_control` true means the node runs once, false means it is still yours; ordinary `Inbox::push` is unchanged for scheduler work); `try_finish` as the loop's exit; the ARP sink. Run `./scripts/check-format.sh`.

- [ ] **Step 6: Commit**

```bash
git add common/runtime tests/unit_tests/common/runtime docs/architecture/runtime.md
git commit -m "feat(runtime): add TCP tick phases and guarded control delivery"
```

## Task 11: Stream owners, wait slots and shard senders

**Files:**
- Create: `common/runtime/stream/stream_wait.hpp`, `streams.hpp`, `stream_senders.hpp`, `stream_handle.hpp`
- Create: `tests/unit_tests/common/runtime/test_streams.cpp`
- Modify: `common/runtime/CMakeLists.txt`, `common/runtime/export/aloe/runtime`, `tests/unit_tests/common/runtime/CMakeLists.txt`, `docs/architecture/stream.md`, `docs/architecture/runtime.md`

**Interfaces:**
- Consumes: `tcp::Stack<Ip>` and its connection; `ShardContext::ready()`, `timers()`, `now()`; `detail::ShardSenderAttributes`; `execution::ex` stop tokens and callbacks.
- Produces: `runtime::Streams<Stack>(Stack&, ShardContext&)` with `wake(now)`, `cancel(index)`, `deadline(index, when)`, `release(index)`, `accept(port)` and `connect(peer)` returning concrete senders, `tcp()`, `context()`; `runtime::Stream<Stack>`, a move-only owner with `connection()`, `index()`, `valid()`, the synchronous members (`unread`, `consume`, `peer_closed`, `writable`, `prepare`, `commit`, `committed`, `acknowledged`, `unacknowledged`, `events`, `state`, `local`, `remote`, `mss`), the senders `readable(n)`, `writable(n)`, `acked(sequence)`, `closed()`, `send(bytes)`, `close()`, and `cancel()`, `deadline(when)`, `release()`; `runtime::detail::StreamWait`, `WaitKind`, `WaitOutcome`, the policies and `StreamSender<Stack, Policy>`. Values are `std::expected<T, stream::Error>`; cancellation, deadlines and shutdown use the stopped channel. Errors map: `ListenError::InUse` to `Refused`, `TableFull` to `TableFull`; `ConnectError` by name; a reset before `Connected` to `Refused`, after it to `Reset`; `closed()` succeeds on a normal `Closed`.

Lifetime rules the code below keeps: a wait is parked on its slot's kind pointer and linked on the slot's active list; a wake detaches the parked pointer before queueing, so a second wake cannot queue it twice; a queued completion stays on the active list until it runs, so `release` can turn its saved outcome into stopped and unlink it; `release` drops every callback of every active wait, queues the parked ones stopped, clears the slot, then reconstructs the slot's stop source; a queued completion that runs afterwards uses only its own outcome and receiver. Every completion is pushed onto the context's run queue and runs inside `run_once`; no receiver is called from a wake pass.

- [ ] **Step 1: Write the failing tests**

`tests/unit_tests/common/runtime/test_streams.cpp`:

```cpp
#include <aloe/execution>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/runtime>
#include <aloe/stream>
#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <net_fixture.hpp>
#include <tcp_fixture.hpp>
#include <tcp_test_device.hpp>

// The senders over a real brick, with the context driven by hand: IP and TCP process, wake, run_once, wake, flush.
// No TcpStack yet; that composition arrives in Task 12.
namespace {

    using namespace std::chrono_literals;
    namespace ex = aloe::execution::ex;
    using aloe::stream::Error;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpSequence;
    using Device    = aloe::testing::TcpTestDevice;
    using Ipv4      = aloe::net::Ipv4<Device>;
    using Tcp       = aloe::tcp::Stack<Ipv4>;
    using Streams   = aloe::runtime::Streams<Tcp>;
    using Stream    = aloe::runtime::Stream<Tcp>;
    using TimePoint = aloe::core::TimePoint;

    constexpr TimePoint start{};

    /// Records one completion; the token lets a test stop the operation from the receiver's side.
    template <typename T>
    struct Recorder {
        using receiver_concept = ex::receiver_t;

        struct Env {
            ex::inplace_stop_token token;
            [[nodiscard]] ex::inplace_stop_token query(ex::get_stop_token_t) const noexcept { return token; }
        };

        std::optional<T>* value;
        int* completions;
        bool* stopped;
        ex::inplace_stop_token token{};

        void set_value(T v) const noexcept {
            *value = std::move(v);
            ++*completions;
        }
        void set_stopped() const noexcept {
            *stopped = true;
            ++*completions;
        }
        [[nodiscard]] Env get_env() const noexcept { return Env{token}; }
    };

    template <typename T>
    struct Slot {
        std::optional<T> value;
        int completions = 0;
        bool stopped    = false;
        ex::inplace_stop_source source;

        [[nodiscard]] Recorder<T> receiver() noexcept { return Recorder<T>{&value, &completions, &stopped, source.get_token()}; }
    };

    class StreamsTest : public ::testing::Test {
    protected:
        aloe::fabric::Fabric fabric_;
        aloe::fabric::Port& harness_ = fabric_.add_port({.mac = aloe::testing::harness_mac, .pool_size = 256, .queue_depth = 4096});
        aloe::fabric::Port& port_    = fabric_.add_port({.mac = aloe::testing::stack_mac, .pool_size = 128});
        Device device_{port_};
        aloe::runtime::ShardContext context_{{.index = 0}, start};
        aloe::runtime::ShardContext::Current current_{context_};
        aloe::loop::ShardQueue<Device> queue_{device_, 0, 1, context_.counters()};
        Ipv4 ip_{queue_, aloe::testing::stack_config()};
        Tcp tcp_{ip_, context_.timers(), {}};
        Streams streams_{tcp_, context_};
        TimePoint now_ = start;
        std::vector<aloe::fabric::Packet> burst_ = std::vector<aloe::fabric::Packet>(64);
        aloe::frames::TcpPeer peer_{aloe::testing::peer_spec(), TcpSequence{aloe::testing::tcp_peer_isn}};

        StreamsTest() {
            ip_.learn(aloe::testing::harness_ip, aloe::testing::harness_mac, now_);
        }

        /// The receive half of a step: inject nothing or `frame`, process IP and TCP, then the first wake.
        void receive(const std::span<const std::byte> frame) {
            auto packet = harness_.allocate(0);
            ASSERT_TRUE(packet.has_value());
            ASSERT_TRUE(aloe::frames::fill(*packet, frame));
            std::array<aloe::fabric::Packet, 1> out{std::move(*packet)};
            ASSERT_EQ(harness_.transmit(0, out), 1U);
            const std::size_t received = queue_.receive(burst_);
            ip_.process(std::span<aloe::fabric::Packet>{burst_}.first(received), now_);
            tcp_.process(ip_.received(aloe::wire::Ipv4Protocol::Tcp), now_);
            streams_.wake(now_);
        }

        /// The rest of a step: tasks, the second wake, flush. Returns how many completions ran.
        std::size_t run_step() {
            now_ += 1ms;
            context_.set_now(now_);
            tcp_.process({}, now_);
            streams_.wake(now_);
            std::ignore = queue_.flush();
            const std::uint64_t before = context_.counters().work_run;
            std::ignore                = context_.run_once(now_);
            streams_.wake(now_);
            tcp_.flush(now_);
            std::ignore = queue_.flush();
            return context_.counters().work_run - before;
        }

        [[nodiscard]] std::vector<aloe::frames::ParsedFrame> on_the_wire() {
            std::ignore = queue_.flush();
            std::vector<aloe::frames::ParsedFrame> frames;
            std::array<aloe::fabric::Packet, 16> out;
            for (std::size_t count = harness_.receive(0, out); count > 0; count = harness_.receive(0, out)) {
                for (std::size_t index = 0; index < count; ++index) {
                    frames.push_back(aloe::frames::parse_frame(aloe::frames::bytes_of(out[index])).value());
                    out[index] = aloe::fabric::Packet{};
                }
            }
            return frames;
        }

        /// A connection accepted through the senders, owned by the returned stream.
        [[nodiscard]] Stream accept_one(aloe::frames::TcpPeer& peer) {
            Slot<std::expected<Stream, Error>> slot;
            auto operation = ex::connect(streams_.accept(aloe::testing::tcp_listen_port), slot.receiver());
            ex::start(operation);
            receive(peer.syn());
            auto frames = on_the_wire();
            if (frames.size() != 1) {
                ADD_FAILURE() << "expected the SYN-ACK";
                return {};
            }
            peer.see(frames[0]);
            receive(peer.ack());
            std::ignore = run_step();
            if (slot.completions != 1 || !slot.value || !*slot.value) {
                ADD_FAILURE() << "accept did not complete with a stream";
                return {};
            }
            return std::move(**slot.value);
        }

        [[nodiscard]] std::vector<std::byte> text(const char* s) {
            std::vector<std::byte> out;
            for (; *s != '\0'; ++s) {
                out.push_back(static_cast<std::byte>(*s));
            }
            return out;
        }
    };

}  // namespace

TEST_F(StreamsTest, ImmediateStateBeforeParking) {
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    receive(peer_.data(text("abc")));
    Slot<std::expected<std::size_t, Error>> readable;
    auto operation = ex::connect(stream.readable(1), readable.receiver());
    ex::start(operation);
    EXPECT_EQ(readable.completions, 1) << "the level is met: no parking, no run_once";
    EXPECT_EQ(readable.value, std::expected<std::size_t, Error>{3U});
    EXPECT_EQ(context_.counters().work_run, 0U);
}

TEST_F(StreamsTest, EverySenderValueAndError) {
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    Slot<std::expected<std::size_t, Error>> readable;
    Slot<std::expected<std::size_t, Error>> writable;
    Slot<std::expected<void, Error>> acked;
    Slot<std::expected<void, Error>> closed;
    auto readable_op = ex::connect(stream.readable(2), readable.receiver());
    auto closed_op   = ex::connect(stream.closed(), closed.receiver());
    ex::start(readable_op);
    ex::start(closed_op);
    EXPECT_EQ(readable.completions + closed.completions, 0) << "parked";
    receive(peer_.data(text("ab")));
    EXPECT_EQ(readable.completions, 0) << "queued, not run: the wake pass never calls a receiver";
    EXPECT_EQ(run_step(), 1U);
    EXPECT_EQ(readable.value, std::expected<std::size_t, Error>{2U});

    const auto bytes = text("hello");
    Slot<std::expected<std::size_t, Error>> sent;
    auto send_op = ex::connect(stream.send(bytes), sent.receiver());
    ex::start(send_op);
    EXPECT_EQ(sent.value, std::expected<std::size_t, Error>{5U}) << "writable: completes at once";
    auto frames = on_the_wire();
    ASSERT_EQ(frames.size(), 1U);
    const TcpSequence after = frames[0].tcp->sequence + 5U;
    auto acked_op           = ex::connect(stream.acked(after), acked.receiver());
    ex::start(acked_op);
    EXPECT_EQ(acked.completions, 0);
    peer_.see(frames[0]);
    receive(peer_.ack());
    EXPECT_EQ(run_step(), 1U);
    EXPECT_TRUE(acked.value.has_value() && acked.value->has_value());

    auto writable_op = ex::connect(stream.writable(1), writable.receiver());
    ex::start(writable_op);
    EXPECT_EQ(writable.value, std::expected<std::size_t, Error>{1460U});

    Slot<std::expected<void, Error>> close;
    auto close_op = ex::connect(stream.close(), close.receiver());
    ex::start(close_op);
    frames = on_the_wire();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_TRUE(frames[0].tcp->flags.has(TcpFlag::Fin));
    receive(peer_.ack(frames[0]));
    receive(peer_.fin());
    EXPECT_EQ(run_step(), 2U) << "closed() and close() both complete";
    EXPECT_TRUE(closed.value.has_value() && closed.value->has_value()) << "a normal close is not an error";
    EXPECT_TRUE(close.value.has_value() && close.value->has_value());
    stream.release();
    EXPECT_EQ(tcp_.table_size(), 0U);
}

TEST_F(StreamsTest, ResetCompletesWaitersWithErrors) {
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    Slot<std::expected<std::size_t, Error>> readable;
    Slot<std::expected<void, Error>> closed;
    auto readable_op = ex::connect(stream.readable(1), readable.receiver());
    auto closed_op   = ex::connect(stream.closed(), closed.receiver());
    ex::start(readable_op);
    ex::start(closed_op);
    receive(peer_.rst());
    EXPECT_EQ(run_step(), 2U);
    EXPECT_EQ(readable.value, std::expected<std::size_t, Error>{std::unexpected{Error::Reset}});
    EXPECT_EQ(closed.value, std::expected<void, Error>{std::unexpected{Error::Reset}});
    Slot<std::expected<std::size_t, Error>> late;
    auto late_op = ex::connect(stream.writable(1), late.receiver());
    ex::start(late_op);
    EXPECT_EQ(late.value, std::expected<std::size_t, Error>{std::unexpected{Error::Reset}}) << "terminal state is remembered";
}

TEST_F(StreamsTest, ConnectValueAndErrors) {
    Slot<std::expected<Stream, Error>> refused;
    Ipv4 bare{queue_, {.address = aloe::testing::stack_ip, .prefix = 24}};
    Tcp bare_tcp{bare, context_.timers(), {}};
    Streams bare_streams{bare_tcp, context_};
    auto bare_op = ex::connect(bare_streams.connect({.address = aloe::testing::far_ip, .port = 7}), refused.receiver());
    ex::start(bare_op);
    EXPECT_EQ(refused.completions, 1);
    ASSERT_TRUE(refused.value.has_value());
    EXPECT_EQ(refused.value->error(), Error::NoRoute);

    Slot<std::expected<Stream, Error>> connected;
    auto op = ex::connect(streams_.connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}), connected.receiver());
    ex::start(op);
    EXPECT_EQ(connected.completions, 0) << "parked on Connected";
    auto frames = on_the_wire();
    ASSERT_EQ(frames.size(), 1U);
    receive(peer_.syn_ack(frames[0]));
    EXPECT_EQ(run_step(), 1U);
    ASSERT_TRUE(connected.value.has_value() && connected.value->has_value());
    Stream stream = std::move(**connected.value);
    EXPECT_EQ(stream.state(), aloe::tcp::State::Established);
    EXPECT_EQ(on_the_wire().size(), 1U) << "the deferred handshake ACK left at the flush";

    Slot<std::expected<Stream, Error>> reset;
    aloe::frames::TcpPeer other{aloe::testing::peer_spec(40001), TcpSequence{2000U}};
    auto reset_op = ex::connect(streams_.connect({.address = aloe::testing::harness_ip, .port = 40001}), reset.receiver());
    ex::start(reset_op);
    frames = on_the_wire();
    ASSERT_EQ(frames.size(), 1U);
    other.see(frames[0]);
    receive(other.rst());
    EXPECT_EQ(run_step(), 1U);
    ASSERT_TRUE(reset.value.has_value());
    EXPECT_EQ(reset.value->error(), Error::Refused) << "a reset before Connected is a refusal";
    EXPECT_EQ(tcp_.table_size(), 1U) << "the refused slot was released by the operation";
}

TEST_F(StreamsTest, AcceptedBacklogAndListenerErrors) {
    Slot<std::expected<Stream, Error>> first;
    auto first_op = ex::connect(streams_.accept(aloe::testing::tcp_listen_port), first.receiver());
    ex::start(first_op);  // creates the listener
    std::vector<aloe::frames::TcpPeer> peers;
    for (std::uint16_t port = 40000; port < 40003; ++port) {
        peers.emplace_back(aloe::testing::peer_spec(port), TcpSequence{1000U});
    }
    for (auto& peer : peers) {
        receive(peer.syn());
        auto frames = on_the_wire();
        ASSERT_EQ(frames.size(), 1U);
        peer.see(frames[0]);
        receive(peer.ack());
    }
    EXPECT_EQ(run_step(), 1U) << "the parked accept took the first";
    ASSERT_TRUE(first.value.has_value() && first.value->has_value());
    EXPECT_EQ((**first.value).remote().port, 40000U);
    for (std::uint16_t port = 40001; port < 40003; ++port) {
        Slot<std::expected<Stream, Error>> next;
        auto op = ex::connect(streams_.accept(aloe::testing::tcp_listen_port), next.receiver());
        ex::start(op);
        EXPECT_EQ(next.completions, 1) << "from the backlog, at once";
        ASSERT_TRUE(next.value.has_value() && next.value->has_value());
        EXPECT_EQ((**next.value).remote().port, port) << "in arrival order";
        (**next.value).release();
    }
    Tcp small{ip_, context_.timers(), {.listeners = 1}};
    Streams small_streams{small, context_};
    ASSERT_TRUE(small.listen(80).has_value());
    Slot<std::expected<Stream, Error>> full;
    auto full_op = ex::connect(small_streams.accept(81), full.receiver());
    ex::start(full_op);
    ASSERT_TRUE(full.value.has_value());
    EXPECT_EQ(full.value->error(), Error::TableFull);
    Slot<std::expected<Stream, Error>> in_use;
    auto in_use_op = ex::connect(small_streams.accept(80), in_use.receiver());
    ex::start(in_use_op);
    ASSERT_TRUE(in_use.value.has_value());
    EXPECT_EQ(in_use.value->error(), Error::Refused) << "listened outside Streams: not ours to serve";
}

TEST_F(StreamsTest, PeerClosedBelowThresholdAndUnreadStaysReadable) {
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    Slot<std::expected<std::size_t, Error>> readable;
    auto op = ex::connect(stream.readable(10), readable.receiver());
    ex::start(op);
    receive(peer_.data(text("abc")));
    EXPECT_EQ(run_step(), 0U) << "three is below ten";
    receive(peer_.fin());
    EXPECT_EQ(run_step(), 1U);
    EXPECT_EQ(readable.value, std::expected<std::size_t, Error>{std::unexpected{Error::PeerClosed}});
    Slot<std::expected<std::size_t, Error>> smaller;
    auto smaller_op = ex::connect(stream.readable(3), smaller.receiver());
    ex::start(smaller_op);
    EXPECT_EQ(smaller.value, std::expected<std::size_t, Error>{3U}) << "what came before the FIN is readable";
}

TEST_F(StreamsTest, PositiveWritableThreshold) {
    peer_.spec().window = 40;
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    EXPECT_EQ(stream.writable(), 40U);
    Slot<std::expected<std::size_t, Error>> writable;
    auto op = ex::connect(stream.writable(100), writable.receiver());
    ex::start(op);
    EXPECT_EQ(writable.completions, 0) << "forty is below a hundred: parked";
    peer_.spec().window = 200;
    receive(peer_.ack());
    EXPECT_EQ(run_step(), 1U);
    EXPECT_EQ(writable.value, std::expected<std::size_t, Error>{200U});
}

TEST_F(StreamsTest, TwoDrainsQueueOnce) {
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    Slot<std::expected<std::size_t, Error>> readable;
    auto op = ex::connect(stream.readable(1), readable.receiver());
    ex::start(op);
    receive(peer_.data(text("x")));  // wake number one queued it
    streams_.wake(now_);             // wake number two finds nothing parked
    EXPECT_EQ(readable.completions, 0);
    EXPECT_EQ(run_step(), 1U) << "one node in the chain";
    EXPECT_EQ(readable.completions, 1);
}

TEST_F(StreamsTest, ReleaseQueuedCompletionThenReuseSlot) {
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    const std::uint32_t index = stream.index();
    Slot<std::expected<std::size_t, Error>> readable;
    auto op = ex::connect(stream.readable(1), readable.receiver());
    ex::start(op);
    receive(peer_.data(text("x")));  // queued with value 1
    stream.release();                // before the completion ran
    std::ignore = on_the_wire();     // the RST
    aloe::frames::TcpPeer next{aloe::testing::peer_spec(40001), TcpSequence{2000U}};
    Stream reused = accept_one(next);
    ASSERT_TRUE(reused.valid());
    EXPECT_EQ(reused.index(), index) << "the same slot";
    EXPECT_EQ(readable.completions, 1) << "the old completion ran during the accept's step";
    EXPECT_TRUE(readable.stopped) << "stopped, with its own saved state; the reused slot was never touched";
    EXPECT_FALSE(reused.connection().events().any());
    Slot<std::expected<std::size_t, Error>> fresh;
    auto fresh_op = ex::connect(reused.readable(1), fresh.receiver());
    ex::start(fresh_op);
    EXPECT_EQ(fresh.completions, 0) << "no stale wake reached the new connection";
}

TEST_F(StreamsTest, CancelQueuedCompletionAndCancelParked) {
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    Slot<std::expected<std::size_t, Error>> readable;
    auto op = ex::connect(stream.readable(1), readable.receiver());
    ex::start(op);
    receive(peer_.data(text("x")));
    stream.cancel();  // the completion is queued: its outcome becomes stopped, nothing is enqueued twice
    EXPECT_EQ(readable.completions, 0);
    EXPECT_EQ(run_step(), 1U);
    EXPECT_TRUE(readable.stopped);
    EXPECT_EQ(stream.state(), aloe::tcp::State::Closed) << "cancel aborts";
    EXPECT_TRUE(on_the_wire().back().tcp->flags.has(TcpFlag::Rst));
    Slot<std::expected<std::size_t, Error>> after;
    auto after_op = ex::connect(stream.writable(1), after.receiver());
    ex::start(after_op);
    EXPECT_TRUE(after.stopped) << "started after cancel: stopped at once";
}

TEST_F(StreamsTest, DeadlineStopsAllWaitersAndAbortsOnce) {
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    Slot<std::expected<std::size_t, Error>> readable;
    Slot<std::expected<void, Error>> acked;
    auto readable_op = ex::connect(stream.readable(1), readable.receiver());
    auto acked_op    = ex::connect(stream.acked(stream.committed() + 1U), acked.receiver());
    ex::start(readable_op);
    ex::start(acked_op);
    stream.deadline(now_ + 5ms);
    EXPECT_EQ(context_.timers().pending(), 1U) << "one timer per connection";
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(run_step(), 0U);
    }
    EXPECT_EQ(run_step(), 2U) << "the deadline fired inside run_once and both completions ran next";
    EXPECT_TRUE(readable.stopped);
    EXPECT_TRUE(acked.stopped);
    EXPECT_EQ(tcp_.counters().resets_sent, 1U);
}

TEST_F(StreamsTest, ReceiverStopDrainsParkedOperation) {
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    Slot<std::expected<std::size_t, Error>> readable;
    auto op = ex::connect(stream.readable(1), readable.receiver());
    ex::start(op);
    readable.source.request_stop();  // the scope's stop at shutdown looks like this
    EXPECT_EQ(run_step(), 1U);
    EXPECT_TRUE(readable.stopped);
    EXPECT_EQ(stream.state(), aloe::tcp::State::Established) << "a receiver stop does not abort the connection";
    Slot<std::expected<std::size_t, Error>> already;
    Recorder<std::expected<std::size_t, Error>> receiver = already.receiver();
    already.source.request_stop();
    auto already_op = ex::connect(stream.readable(1), receiver);
    ex::start(already_op);
    EXPECT_TRUE(already.stopped) << "an already requested token completes stopped in start";
}

TEST_F(StreamsTest, MoveOwnerReleasesOnce) {
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    {
        Stream moved = std::move(stream);
        EXPECT_FALSE(stream.valid());  // NOLINT(bugprone-use-after-move): the moved-from handle is inert by contract
        EXPECT_TRUE(moved.valid());
        EXPECT_EQ(tcp_.table_size(), 1U);
    }
    EXPECT_EQ(tcp_.table_size(), 0U) << "the owner's destructor released";
    stream.release();  // NOLINT(bugprone-use-after-move): inert: a no-op
    EXPECT_EQ(tcp_.counters().resets_sent, 1U);
}

TEST_F(StreamsTest, SendRefusalParksUntilRetryHint) {
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    device_.refuse_transmit = true;
    auto filler             = port_.allocate(0);
    ASSERT_TRUE(filler.has_value());
    std::ignore = queue_.transmit(std::move(*filler));  // the one-slot ring now holds a packet the device refuses
    const auto bytes = text("hello");
    Slot<std::expected<std::size_t, Error>> sent;
    auto op = ex::connect(stream.send(bytes), sent.receiver());
    ex::start(op);
    EXPECT_EQ(sent.completions, 0) << "refused: parked, although writable() is still positive";
    EXPECT_EQ(stream.writable(), 1460U);
    EXPECT_EQ(run_step(), 1U) << "the retry hint woke it, and the retry was refused again: parked again";
    EXPECT_EQ(sent.completions, 0);
    device_.refuse_transmit = false;
    EXPECT_EQ(run_step(), 1U);
    EXPECT_EQ(sent.value, std::expected<std::size_t, Error>{5U});
    const auto frames = on_the_wire();
    ASSERT_FALSE(frames.empty());
    EXPECT_EQ(aloe::frames::tcp_payload(frames.back()).size(), 5U);
}
```

Add `test_streams.cpp` to `Aloe.Tests.Unit.Runtime` and link `Aloe::Common::Tcp`, `Aloe::Common::Net`, `Aloe::Fixtures::Frames`, `${SHARED_TESTING_TARGET}.Net` and `${SHARED_TESTING_TARGET}.Tcp` to that target.

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Runtime`
Expected: compile errors, `no member named 'Streams' in namespace 'aloe::runtime'`.

- [ ] **Step 3: Write the wait node, the owner, the senders and the handle**

`common/runtime/stream/stream_wait.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include <stream_events.hpp>
#include <work.hpp>

namespace aloe::runtime::detail {

    /// The kinds of wait a connection slot parks, one of each at most.
    enum class WaitKind : std::uint8_t { Readable, Writable, Acked, Connected, Closed, Accept };

    inline constexpr std::size_t slot_wait_kinds = 5;  ///< Accept parks on a listener, not a slot.

    /// What a wait completes with, saved on the node by the wake pass; the operation turns it into its value.
    struct WaitOutcome {
        std::optional<stream::Error> error;
        std::size_t value = 0;  ///< Bytes, or the index of a connection to hand over.
        bool stopped      = false;
    };

    /**
     * @brief A parked or queued stream operation: a loop::Work the shard runs, with what the owner
     * needs to complete it from a wake pass without calling into the receiver.
     *
     * The derived operation state sets `run` (its completion) and `drop_callbacks` (resetting the
     * stop callbacks it registered). Phases: Parked on a slot's kind pointer and the slot's active
     * list; Queued once detached from the kind pointer and pushed onto the run queue, still on the
     * active list so a release can turn the outcome into stopped; Completed when run.
     */
    struct StreamWait : loop::Work {
        enum class Phase : std::uint8_t { Idle, Parked, Queued, Completed };
        using Drop = void (*)(StreamWait&) noexcept;

        StreamWait(const loop::Work::Function complete, const Drop drop) noexcept
            : loop::Work{complete},
              drop_callbacks{drop} {
        }

        Phase phase           = Phase::Idle;
        WaitKind kind         = WaitKind::Readable;
        std::uint32_t index   = 0;  ///< The connection slot; for Accept, the listener's port.
        std::size_t threshold = 0;  ///< Bytes for Readable and Writable, the sequence value for Acked.
        WaitOutcome outcome{};
        Drop drop_callbacks;
        StreamWait* next_active = nullptr;
        StreamWait* prev_active = nullptr;
        bool active             = false;  ///< On a slot's active list.
    };

}  // namespace aloe::runtime::detail
```

`common/runtime/stream/streams.hpp`:

```cpp
#pragma once

#include <aloe/core>
#include <aloe/execution>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <utility>
#include <vector>

#include <shard_context.hpp>
#include <stream_events.hpp>
#include <stream_wait.hpp>
#include <tcp_config.hpp>
#include <timer_wheel.hpp>

namespace aloe::runtime {

    template <typename Stack>
    class Stream;

    namespace detail {
        template <typename Stack, typename Policy>
        struct StreamSender;
        struct ConnectPolicy;
        struct AcceptPolicy;
    }  // namespace detail

    /**
     * @brief The runtime's side of a TCP stack: one wait slot per connection, one per listener,
     * and the wake pass that turns drained events into queued completions.
     *
     * Constructed over a `tcp::Stack` and the shard's context, with every slot allocated here.
     * `wake` drains `poll_event()` and, per flag, detaches the matching parked wait and pushes it
     * onto the run queue; it never calls a receiver. `cancel` requests the slot's stop source and
     * aborts; `deadline` arms the slot's timer, whose fire is `cancel`; `release` stops every active
     * wait, clears the slot, rebuilds its stop source and releases the brick's connection.
     */
    template <typename Stack>
    class Streams {
    public:
        using ConnectionType = typename Stack::ConnectionType;
        using StreamType     = Stream<Stack>;
        using Sequence       = typename Stack::Sequence;

        Streams(Stack& tcp, ShardContext& context)
            : tcp_{&tcp},
              context_{&context} {
            for (std::uint32_t index = 0; index < tcp.capacity(); ++index) {
                Slot& slot = slots_.emplace_back(this, index);
                slot.stop.emplace();
            }
            listeners_.reserve(tcp.config().listeners);
        }

        Streams(const Streams&)            = delete;
        Streams& operator=(const Streams&) = delete;
        Streams(Streams&&)                 = delete;
        Streams& operator=(Streams&&)      = delete;

        ~Streams() {
            for (Slot& slot : slots_) {
                context_->timers().cancel(slot.deadline);
                assert(slot.active == nullptr && "a stream operation outlived the shard's scope");
            }
        }

        /// Drains the brick's events; every completion goes onto the run queue, none runs here.
        void wake(core::TimePoint /*now*/) noexcept {
            while (const auto event = tcp_->poll_event()) {
                deliver(*event->connection, event->events);
            }
        }

        /// Requests the slot's stop source and aborts: parked waits complete stopped, later starts stop at once.
        void cancel(const std::uint32_t index) noexcept {
            Slot& slot = slots_[index];
            slot.stop->request_stop();
            tcp_->connection(index).abort();
        }

        void deadline(const std::uint32_t index, const core::TimePoint when) noexcept {
            context_->timers().arm(slots_[index].deadline, when);
        }

        /// The owner's last call for a connection: every active wait is stopped, the slot is clean for reuse.
        void release(const std::uint32_t index) noexcept {
            Slot& slot = slots_[index];
            context_->timers().cancel(slot.deadline);
            while (detail::StreamWait* wait = slot.active) {
                unlink(slot, *wait);
                wait->drop_callbacks(*wait);
                wait->outcome = detail::WaitOutcome{.stopped = true};
                if (wait->phase == detail::StreamWait::Phase::Parked) {
                    slot.waits[kind_index(wait->kind)] = nullptr;
                    wait->phase                        = detail::StreamWait::Phase::Queued;
                    context_->ready().push(*wait);
                }
            }
            slot.waits.fill(nullptr);
            slot.terminal.reset();
            slot.owned = false;
            slot.stop.emplace();  // after every callback on the old source is gone
            tcp_->release(index);
        }

        [[nodiscard]] detail::StreamSender<Stack, detail::AcceptPolicy> accept(std::uint16_t port) noexcept;
        [[nodiscard]] detail::StreamSender<Stack, detail::ConnectPolicy> connect(tcp::Endpoint peer) noexcept;

        [[nodiscard]] Stack& tcp() noexcept { return *tcp_; }
        [[nodiscard]] ShardContext& context() noexcept { return *context_; }

        // The operations' side: parking, queueing and the slot's state. Not for applications.

        void park(detail::StreamWait& wait) noexcept {
            Slot& slot                                 = slots_[wait.index];
            detail::StreamWait*& parked                = slot.waits[kind_index(wait.kind)];
            assert(parked == nullptr && "one parked operation per kind per connection");
            parked     = &wait;
            wait.phase = detail::StreamWait::Phase::Parked;
            if (!wait.active) {
                link(slot, wait);  // a re-parked send is still on the list
            }
        }

        /// Detaches a parked wait without completing it; a no-op for a queued one.
        void unpark(detail::StreamWait& wait) noexcept {
            Slot& slot = slots_[wait.index];
            if (wait.phase == detail::StreamWait::Phase::Parked) {
                slot.waits[kind_index(wait.kind)] = nullptr;
            }
            if (wait.active) {
                unlink(slot, wait);
            }
            wait.phase = detail::StreamWait::Phase::Idle;
        }

        /// Detaches from the kind pointer and pushes onto the run queue; stays on the active list until it runs.
        void queue(detail::StreamWait& wait, const detail::WaitOutcome outcome) noexcept {
            if (wait.kind == detail::WaitKind::Accept) {
                if (Listener* listener = find_listener(static_cast<std::uint16_t>(wait.index))) {
                    if (listener->accept == &wait) {
                        listener->accept = nullptr;
                    }
                }
            } else if (wait.phase == detail::StreamWait::Phase::Parked) {
                slots_[wait.index].waits[kind_index(wait.kind)] = nullptr;
            }
            wait.outcome = outcome;
            wait.phase   = detail::StreamWait::Phase::Queued;
            context_->ready().push(wait);
        }

        /// Unlinks a wait that is about to complete; the completion itself belongs to the operation.
        void retire(detail::StreamWait& wait) noexcept {
            if (wait.active && wait.kind != detail::WaitKind::Accept) {
                unlink(slots_[wait.index], wait);
            }
            wait.phase = detail::StreamWait::Phase::Completed;
        }

        [[nodiscard]] execution::ex::inplace_stop_token slot_token(const std::uint32_t index) const noexcept {
            return slots_[index].stop->get_token();
        }

        [[nodiscard]] std::optional<stream::Error> terminal(const std::uint32_t index) const noexcept {
            return slots_[index].terminal;
        }

        void own(const std::uint32_t index) noexcept { slots_[index].owned = true; }

        /// The listener for `port`, created through the brick when absent. Refused when someone else listens there.
        [[nodiscard]] std::expected<void, stream::Error> ensure_listener(const std::uint16_t port) noexcept {
            if (find_listener(port) != nullptr) {
                return {};
            }
            if (listeners_.size() == listeners_.capacity()) {
                return std::unexpected{stream::Error::TableFull};
            }
            const auto listened = tcp_->listen(port);
            if (!listened) {
                return std::unexpected{listened.error() == tcp::ListenError::TableFull ? stream::Error::TableFull
                                                                                       : stream::Error::Refused};
            }
            Listener& listener = listeners_.emplace_back();
            listener.port      = port;
            listener.backlog.reserve(tcp_->capacity());
            return {};
        }

        void park_accept(detail::StreamWait& wait) noexcept {
            Listener* listener = find_listener(static_cast<std::uint16_t>(wait.index));
            assert(listener != nullptr && listener->accept == nullptr && "one accept per listener");
            listener->accept = &wait;
            wait.phase       = detail::StreamWait::Phase::Parked;
        }

        void unpark_accept(detail::StreamWait& wait) noexcept {
            if (Listener* listener = find_listener(static_cast<std::uint16_t>(wait.index))) {
                if (listener->accept == &wait) {
                    listener->accept = nullptr;
                }
            }
            wait.phase = detail::StreamWait::Phase::Idle;
        }

        [[nodiscard]] std::optional<std::uint32_t> take_backlog(const std::uint16_t port) noexcept {
            Listener* listener = find_listener(port);
            if (listener == nullptr || listener->head == listener->backlog.size()) {
                return std::nullopt;
            }
            const std::uint32_t index = listener->backlog[listener->head++];
            if (listener->head == listener->backlog.size()) {
                listener->backlog.clear();
                listener->head = 0;
            }
            return index;
        }

    private:
        struct DeadlineTimer : loop::Timer {
            DeadlineTimer(Streams* o, const std::uint32_t i) noexcept
                : loop::Timer{&Streams::on_deadline},
                  owner{o},
                  index{i} {
            }

            Streams* owner;
            std::uint32_t index;
        };

        struct Slot {
            Slot(Streams* owner, const std::uint32_t index) noexcept
                : deadline{owner, index} {
            }

            std::array<detail::StreamWait*, detail::slot_wait_kinds> waits{};
            detail::StreamWait* active = nullptr;  ///< Parked and queued waits, for release.
            std::optional<execution::ex::inplace_stop_source> stop;
            std::optional<stream::Error> terminal;
            DeadlineTimer deadline;
            bool owned = false;
        };

        struct Listener {
            std::uint16_t port         = 0;
            detail::StreamWait* accept = nullptr;
            std::vector<std::uint32_t> backlog;  ///< Accepted connections nobody has taken, in order.
            std::size_t head = 0;
        };

        [[nodiscard]] static constexpr std::size_t kind_index(const detail::WaitKind kind) noexcept {
            return static_cast<std::size_t>(kind);
        }

        static void on_deadline(loop::Timer& timer) noexcept {
            auto& self = static_cast<DeadlineTimer&>(timer);
            self.owner->cancel(self.index);
        }

        static void link(Slot& slot, detail::StreamWait& wait) noexcept {
            wait.prev_active = nullptr;
            wait.next_active = slot.active;
            if (slot.active != nullptr) {
                slot.active->prev_active = &wait;
            }
            slot.active = &wait;
            wait.active = true;
        }

        static void unlink(Slot& slot, detail::StreamWait& wait) noexcept {
            if (wait.prev_active != nullptr) {
                wait.prev_active->next_active = wait.next_active;
            } else {
                slot.active = wait.next_active;
            }
            if (wait.next_active != nullptr) {
                wait.next_active->prev_active = wait.prev_active;
            }
            wait.prev_active = nullptr;
            wait.next_active = nullptr;
            wait.active      = false;
        }

        [[nodiscard]] Listener* find_listener(const std::uint16_t port) noexcept {
            for (Listener& listener : listeners_) {
                if (listener.port == port) {
                    return &listener;
                }
            }
            return nullptr;
        }

        /// The outcome a terminal event gives a wait of `kind`: data still counts, a normal close satisfies closed().
        [[nodiscard]] static detail::WaitOutcome terminal_outcome(const detail::WaitKind kind,
                                                                  const stream::Error error,
                                                                  const ConnectionType& c,
                                                                  const std::size_t threshold) noexcept {
            switch (kind) {
                case detail::WaitKind::Readable:
                    if (c.unread().size() >= threshold) {
                        return {.value = c.unread().size()};
                    }
                    return {.error = error == stream::Error::Closed ? stream::Error::PeerClosed : error};
                case detail::WaitKind::Acked:
                    if (!c.acknowledged().before(Sequence{static_cast<std::uint32_t>(threshold)})) {
                        return {};
                    }
                    return {.error = error};
                case detail::WaitKind::Connected:
                    return {.error = error == stream::Error::Reset ? stream::Error::Refused : error};
                case detail::WaitKind::Closed:
                    if (error == stream::Error::Closed) {
                        return {};
                    }
                    return {.error = error};
                default:
                    return {.error = error};
            }
        }

        void deliver(ConnectionType& c, const stream::Events events) noexcept {
            Slot& slot = slots_[c.index()];
            if (events.accepted()) {
                Listener* listener = find_listener(c.local().port);
                if (listener == nullptr) {
                    tcp_->release(c.index());  // nobody will ever take it: not a listener of ours
                    return;
                }
                if (listener->accept != nullptr) {
                    queue(*listener->accept, {.value = c.index()});
                } else {
                    listener->backlog.push_back(c.index());  // within the reserved capacity
                }
            }
            if (events.connected()) {
                if (detail::StreamWait* wait = slot.waits[kind_index(detail::WaitKind::Connected)]) {
                    queue(*wait, {.value = c.index()});
                }
            }
            if (events.readable() || events.peer_closed()) {
                if (detail::StreamWait* wait = slot.waits[kind_index(detail::WaitKind::Readable)]) {
                    if (c.unread().size() >= wait->threshold) {
                        queue(*wait, {.value = c.unread().size()});
                    } else if (c.peer_closed()) {
                        queue(*wait, {.error = stream::Error::PeerClosed});
                    }
                }
            }
            if (events.writable()) {
                if (detail::StreamWait* wait = slot.waits[kind_index(detail::WaitKind::Writable)]) {
                    if (c.writable() >= wait->threshold) {
                        queue(*wait, {.value = c.writable()});
                    }
                }
            }
            if (events.acked()) {
                if (detail::StreamWait* wait = slot.waits[kind_index(detail::WaitKind::Acked)]) {
                    if (!c.acknowledged().before(Sequence{static_cast<std::uint32_t>(wait->threshold)})) {
                        queue(*wait, {});
                    }
                }
            }
            std::optional<stream::Error> terminal;
            if (events.reset()) {
                terminal = stream::Error::Reset;
            } else if (events.timed_out()) {
                terminal = stream::Error::TimedOut;
            } else if (events.closed()) {
                terminal = stream::Error::Closed;
            }
            if (terminal) {
                slot.terminal = terminal;
                for (std::size_t kind = 0; kind < detail::slot_wait_kinds; ++kind) {
                    if (detail::StreamWait* wait = slot.waits[kind]) {
                        queue(*wait, terminal_outcome(static_cast<detail::WaitKind>(kind), *terminal, c, wait->threshold));
                    }
                }
            }
        }

        Stack* tcp_;
        ShardContext* context_;
        std::deque<Slot> slots_;  ///< A slot holds a timer: immovable, so a deque.
        std::vector<Listener> listeners_;
    };

}  // namespace aloe::runtime

#include <stream_handle.hpp>
#include <stream_senders.hpp>
```

`common/runtime/stream/stream_senders.hpp`:

```cpp
#pragma once

#include <aloe/core>
#include <aloe/execution>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>

#include <scheduler.hpp>
#include <stream.hpp>
#include <stream_handle.hpp>
#include <stream_wait.hpp>
#include <streams.hpp>
#include <tcp_config.hpp>

namespace aloe::runtime::detail {

    template <typename Stack, typename Receiver, typename Policy>
    struct WaitOperation;

    template <typename Op>
    using Stack_of = typename Op::StackType;

    /**
     * The policies: what a sender checks when it starts, what it does when woken, and how it turns the
     * saved outcome into its value. `begin` returns an outcome to complete with at once, or nothing
     * to park. `resume` returns true when a woken operation completes, false when it parked again.
     */
    struct ReadablePolicy {
        using Result                   = std::expected<std::size_t, stream::Error>;
        static constexpr WaitKind kind = WaitKind::Readable;

        template <typename Op>
        [[nodiscard]] static std::optional<WaitOutcome> begin(Op& op) noexcept {
            const auto& c = op.owner->tcp().connection(op.index);
            if (c.unread().size() >= op.threshold) {
                return WaitOutcome{.value = c.unread().size()};
            }
            if (const auto error = op.owner->terminal(op.index)) {
                return WaitOutcome{.error = *error == stream::Error::Closed ? stream::Error::PeerClosed : *error};
            }
            if (c.peer_closed()) {
                return WaitOutcome{.error = stream::Error::PeerClosed};
            }
            return std::nullopt;
        }

        template <typename Op>
        [[nodiscard]] static bool resume(Op&) noexcept {
            return true;
        }

        template <typename Op>
        [[nodiscard]] static Result result(const Op& op) noexcept {
            if (op.outcome.error) {
                return std::unexpected{*op.outcome.error};
            }
            return op.outcome.value;
        }
    };

    struct WritablePolicy {
        using Result                   = std::expected<std::size_t, stream::Error>;
        static constexpr WaitKind kind = WaitKind::Writable;

        template <typename Op>
        [[nodiscard]] static std::optional<WaitOutcome> begin(Op& op) noexcept {
            const auto& c = op.owner->tcp().connection(op.index);
            assert(op.threshold <= c.mss() && "writable(n): n is at most the MSS");
            if (const auto error = op.owner->terminal(op.index)) {
                return WaitOutcome{.error = *error};
            }
            if (c.writable() >= op.threshold) {
                return WaitOutcome{.value = c.writable()};
            }
            return std::nullopt;
        }

        template <typename Op>
        [[nodiscard]] static bool resume(Op&) noexcept {
            return true;
        }

        template <typename Op>
        [[nodiscard]] static Result result(const Op& op) noexcept {
            if (op.outcome.error) {
                return std::unexpected{*op.outcome.error};
            }
            return op.outcome.value;
        }
    };

    struct AckedPolicy {
        using Result                   = std::expected<void, stream::Error>;
        static constexpr WaitKind kind = WaitKind::Acked;

        template <typename Op>
        [[nodiscard]] static std::optional<WaitOutcome> begin(Op& op) noexcept {
            const auto& c = op.owner->tcp().connection(op.index);
            using Sequence = typename Stack_of<Op>::Sequence;
            if (!c.acknowledged().before(Sequence{static_cast<std::uint32_t>(op.threshold)})) {
                return WaitOutcome{};
            }
            if (const auto error = op.owner->terminal(op.index)) {
                return WaitOutcome{.error = *error};
            }
            return std::nullopt;
        }

        template <typename Op>
        [[nodiscard]] static bool resume(Op&) noexcept {
            return true;
        }

        template <typename Op>
        [[nodiscard]] static Result result(const Op& op) noexcept {
            if (op.outcome.error) {
                return std::unexpected{*op.outcome.error};
            }
            return {};
        }
    };

    /// `closed()`, and `close()` when `close_first`: the brick's close, then the wait.
    struct ClosedPolicy {
        using Result                   = std::expected<void, stream::Error>;
        static constexpr WaitKind kind = WaitKind::Closed;

        bool close_first = false;

        template <typename Op>
        [[nodiscard]] std::optional<WaitOutcome> begin(Op& op) const noexcept {
            auto& c = op.owner->tcp().connection(op.index);
            if (close_first) {
                c.close();
            }
            if (const auto error = op.owner->terminal(op.index)) {
                if (*error == stream::Error::Closed) {
                    return WaitOutcome{};
                }
                return WaitOutcome{.error = *error};
            }
            if (c.state() == tcp::State::Closed) {
                return WaitOutcome{};  // closed before Streams saw the event: the wake pass will find no wait
            }
            return std::nullopt;
        }

        template <typename Op>
        [[nodiscard]] static bool resume(Op&) noexcept {
            return true;
        }

        template <typename Op>
        [[nodiscard]] static Result result(const Op& op) noexcept {
            if (op.outcome.error) {
                return std::unexpected{*op.outcome.error};
            }
            return {};
        }
    };

    /// `send(bytes)`: commits what it can, parks on Writable for the rest, keeps the caller's span until done.
    struct SendPolicy {
        using Result                   = std::expected<std::size_t, stream::Error>;
        static constexpr WaitKind kind = WaitKind::Writable;

        std::span<const std::byte> bytes;
        std::size_t accepted = 0;

        template <typename Op>
        [[nodiscard]] std::optional<WaitOutcome> begin(Op& op) noexcept {
            op.threshold = 1;
            return advance(op);
        }

        template <typename Op>
        [[nodiscard]] bool resume(Op& op) noexcept {
            if (op.outcome.stopped || op.outcome.error) {
                return true;
            }
            const std::optional<WaitOutcome> done = advance(op);
            if (!done) {
                op.owner->park(op);  // a refused retry, or more to send: park again under the same callbacks
                return false;
            }
            op.outcome = *done;
            return true;
        }

        template <typename Op>
        [[nodiscard]] std::optional<WaitOutcome> advance(Op& op) noexcept {
            auto& c = op.owner->tcp().connection(op.index);
            if (const auto error = op.owner->terminal(op.index)) {
                return WaitOutcome{.error = *error};
            }
            accepted += stream::send(c, bytes.subspan(accepted));
            if (accepted == bytes.size()) {
                return WaitOutcome{.value = accepted};
            }
            return std::nullopt;  // a refused prepare or commit, or no credit: wait for the hint, never retry inline
        }

        template <typename Op>
        [[nodiscard]] static Result result(const Op& op) noexcept {
            if (op.outcome.error) {
                return std::unexpected{*op.outcome.error};
            }
            return op.outcome.value;
        }
    };

    [[nodiscard]] constexpr stream::Error to_error(const tcp::ConnectError error) noexcept {
        switch (error) {
            case tcp::ConnectError::TableFull:
                return stream::Error::TableFull;
            case tcp::ConnectError::NoPort:
                return stream::Error::NoPort;
            case tcp::ConnectError::Unplaceable:
                return stream::Error::Unplaceable;
            default:
                return stream::Error::NoRoute;
        }
    }

    /// `connect(peer)`: opens on start and owns the pending connection until it hands it over.
    struct ConnectPolicy {
        template <typename Stack>
        using ResultFor                = std::expected<Stream<Stack>, stream::Error>;
        static constexpr WaitKind kind = WaitKind::Connected;

        tcp::Endpoint peer;
        bool opened = false;  ///< A SynSent connection this operation owns until it hands it over.

        template <typename Op>
        [[nodiscard]] std::optional<WaitOutcome> begin(Op& op) noexcept {
            auto& tcp            = op.owner->tcp();
            const auto connected = tcp.connect(peer, op.owner->context().now());
            if (!connected) {
                return WaitOutcome{.error = to_error(connected.error())};
            }
            op.index = (*connected)->index();
            opened   = true;
            return std::nullopt;
        }

        template <typename Op>
        [[nodiscard]] static bool resume(Op&) noexcept {
            return true;
        }

        /// A stopped or failed open releases the connection it still owns; a handed-over one is the stream's.
        template <typename Op>
        void abandon(Op& op) noexcept {
            if (opened) {
                opened = false;
                op.owner->release(op.index);
            }
        }
    };

    /// `accept(port)`: the listener's backlog first, else parks on the listener.
    struct AcceptPolicy {
        template <typename Stack>
        using ResultFor                = std::expected<Stream<Stack>, stream::Error>;
        static constexpr WaitKind kind = WaitKind::Accept;

        std::uint16_t port = 0;

        template <typename Op>
        [[nodiscard]] std::optional<WaitOutcome> begin(Op& op) const noexcept {
            op.index = port;
            if (const auto listening = op.owner->ensure_listener(port); !listening) {
                return WaitOutcome{.error = listening.error()};
            }
            if (const auto ready = op.owner->take_backlog(port)) {
                return WaitOutcome{.value = *ready};
            }
            return std::nullopt;
        }

        template <typename Op>
        [[nodiscard]] static bool resume(Op&) noexcept {
            return true;
        }

        /// A connection taken from the backlog but never handed over goes back to the brick.
        template <typename Op>
        void abandon(Op& op) noexcept {
            if (op.phase == StreamWait::Phase::Queued && !op.outcome.error && !op.outcome.stopped) {
                op.owner->release(static_cast<std::uint32_t>(op.outcome.value));
            }
        }
    };

    template <typename Policy, typename Stack>
    struct PolicyResult {
        using type = typename Policy::Result;
    };

    template <typename Stack>
    struct PolicyResult<ConnectPolicy, Stack> {
        using type = std::expected<Stream<Stack>, stream::Error>;
    };

    template <typename Stack>
    struct PolicyResult<AcceptPolicy, Stack> {
        using type = std::expected<Stream<Stack>, stream::Error>;
    };

    template <typename Policy>
    concept HandsOverConnection = std::same_as<Policy, ConnectPolicy> || std::same_as<Policy, AcceptPolicy>;

    /**
     * The operation state of every stream sender: the wait node, the receiver, the policy, and the
     * two stop callbacks, on the receiver's token (scope shutdown) and on the slot's token (cancel
     * and deadline). Allocates nothing; lives in the caller's frame; completes on the shard.
     */
    template <typename Stack, typename Receiver, typename Policy>
    struct WaitOperation final : StreamWait {
        using StackType = Stack;
        using Result    = typename PolicyResult<Policy, Stack>::type;
        using StopToken = execution::ex::stop_token_of_t<execution::ex::env_of_t<Receiver>>;

        struct OnStop {
            WaitOperation* self;

            void operator()() noexcept {
                self->cancel();
            }
        };

        using ReceiverCallback = execution::ex::stop_callback_for_t<StopToken, OnStop>;
        using SlotCallback     = execution::ex::inplace_stop_callback<OnStop>;

        Streams<Stack>* owner;
        Receiver receiver;
        Policy policy;
        std::optional<ReceiverCallback> receiver_callback;
        std::optional<SlotCallback> slot_callback;

        WaitOperation(Streams<Stack>* o, Receiver r, Policy p, const std::uint32_t i, const std::size_t n) noexcept
            : StreamWait{&WaitOperation::complete, &WaitOperation::drop},
              owner{o},
              receiver{std::move(r)},
              policy{std::move(p)} {
            kind      = Policy::kind;
            index     = i;
            threshold = n;
        }

        void start() & noexcept {
            assert(ShardContext::current() == &owner->context() && "a stream sender starts on its own shard");
            const StopToken token = execution::ex::get_stop_token(execution::ex::get_env(receiver));
            if (token.stop_requested()) {
                execution::ex::set_stopped(std::move(receiver));
                return;
            }
            if (const std::optional<WaitOutcome> now = policy.begin(*this)) {
                outcome = *now;
                finish();
                return;
            }
            if constexpr (Policy::kind != WaitKind::Accept) {
                if (owner->slot_token(index).stop_requested()) {  // cancelled before this operation started
                    if constexpr (HandsOverConnection<Policy>) {
                        policy.abandon(*this);
                    }
                    outcome = WaitOutcome{.stopped = true};
                    finish();
                    return;
                }
                owner->park(*this);
                slot_callback.emplace(owner->slot_token(index), OnStop{this});
            } else {
                owner->park_accept(*this);
            }
            receiver_callback.emplace(token, OnStop{this});
        }

        /// From either token, on the shard thread. Parked: detach and queue stopped. Queued: the saved outcome
        /// becomes stopped; the node is already in the chain and is not enqueued again.
        void cancel() noexcept {
            if (phase == Phase::Parked) {
                if constexpr (Policy::kind == WaitKind::Accept) {
                    owner->unpark_accept(*this);
                } else {
                    owner->unpark(*this);
                }
                if constexpr (HandsOverConnection<Policy>) {
                    policy.abandon(*this);
                }
                outcome = WaitOutcome{.stopped = true};
                phase   = Phase::Queued;
                owner->context().ready().push(*this);
            } else if (phase == Phase::Queued) {
                if constexpr (HandsOverConnection<Policy>) {
                    policy.abandon(*this);
                }
                outcome = WaitOutcome{.stopped = true};
            }
        }

        static void complete(loop::Work& work) noexcept {
            auto& self = static_cast<WaitOperation&>(work);
            if (!self.policy.resume(self)) {
                return;  // parked again, under the same callbacks
            }
            self.owner->retire(self);
            self.finish();
        }

        static void drop(StreamWait& wait) noexcept {
            auto& self = static_cast<WaitOperation&>(wait);
            self.receiver_callback.reset();
            self.slot_callback.reset();
        }

        void finish() noexcept {
            drop(*this);
            phase = Phase::Completed;
            if (outcome.stopped) {
                execution::ex::set_stopped(std::move(receiver));
                return;
            }
            if constexpr (HandsOverConnection<Policy>) {
                if (outcome.error) {
                    policy.abandon(*this);  // a refused or timed-out open: the slot goes back before the error is reported
                    execution::ex::set_value(std::move(receiver), Result{std::unexpected{*outcome.error}});
                } else {
                    execution::ex::set_value(std::move(receiver), Result{Stream<Stack>{*owner, static_cast<std::uint32_t>(outcome.value)}});
                }
            } else {
                execution::ex::set_value(std::move(receiver), Policy::result(*this));
            }
        }
    };

    template <typename Stack, typename Policy>
    struct StreamSender {
        using sender_concept = execution::ex::sender_t;
        using Result         = typename PolicyResult<Policy, Stack>::type;
        using completion_signatures =
            execution::ex::completion_signatures<execution::ex::set_value_t(Result), execution::ex::set_stopped_t()>;

        Streams<Stack>* owner;
        std::uint32_t index;
        std::size_t threshold;
        Policy policy;

        template <execution::ex::receiver Receiver>
        [[nodiscard]] auto connect(Receiver receiver) const noexcept -> WaitOperation<Stack, Receiver, Policy> {
            return WaitOperation<Stack, Receiver, Policy>{owner, std::move(receiver), policy, index, threshold};
        }

        [[nodiscard]] ShardSenderAttributes get_env() const noexcept {
            return ShardSenderAttributes{&owner->context()};
        }
    };

}  // namespace aloe::runtime::detail

namespace aloe::runtime {

    template <typename Stack>
    detail::StreamSender<Stack, detail::AcceptPolicy> Streams<Stack>::accept(const std::uint16_t port) noexcept {
        return {this, port, 0, detail::AcceptPolicy{.port = port}};
    }

    template <typename Stack>
    detail::StreamSender<Stack, detail::ConnectPolicy> Streams<Stack>::connect(const tcp::Endpoint peer) noexcept {
        return {this, 0, 0, detail::ConnectPolicy{.peer = peer}};
    }

}  // namespace aloe::runtime
```

`Policy::result` is static in every policy and reads the operation; `begin`, `resume` and `abandon` are members where the policy carries state (`SendPolicy`, `ConnectPolicy`, `AcceptPolicy`, `ClosedPolicy`) and static otherwise, which is why the operation calls them through `policy.`.

`common/runtime/stream/stream_handle.hpp`:

```cpp
#pragma once

#include <aloe/core>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include <stream_events.hpp>
#include <stream_wait.hpp>
#include <tcp_config.hpp>

namespace aloe::runtime {

    template <typename Stack>
    class Streams;

    namespace detail {
        template <typename Stack, typename Policy>
        struct StreamSender;
        struct ReadablePolicy;
        struct WritablePolicy;
        struct AckedPolicy;
        struct ClosedPolicy;
        struct SendPolicy;
    }  // namespace detail

    /**
     * @brief A move-only owner of one connection: the synchronous stream members, the senders that
     * wait for its events, and `release` in the destructor so a task that returns for any reason
     * frees the slot.
     *
     * The synchronous `send` and `close` of the brick are reached through `connection()`; the
     * members here with those names are senders. A moved-from or released handle is inert.
     */
    template <typename Stack>
    class Stream {
    public:
        using ConnectionType = typename Stack::ConnectionType;
        using Sequence       = typename Stack::Sequence;
        using View           = typename ConnectionType::View;

        Stream() noexcept = default;

        Stream(Streams<Stack>& owner, const std::uint32_t index) noexcept
            : owner_{&owner},
              index_{index} {
            owner.own(index);
        }

        Stream(Stream&& other) noexcept
            : owner_{std::exchange(other.owner_, nullptr)},
              index_{other.index_} {
        }

        Stream& operator=(Stream&& other) noexcept {
            if (this != &other) {
                release();
                owner_ = std::exchange(other.owner_, nullptr);
                index_ = other.index_;
            }
            return *this;
        }

        Stream(const Stream&)            = delete;
        Stream& operator=(const Stream&) = delete;

        ~Stream() {
            release();
        }

        [[nodiscard]] bool valid() const noexcept { return owner_ != nullptr; }
        [[nodiscard]] std::uint32_t index() const noexcept { return index_; }
        [[nodiscard]] ConnectionType& connection() noexcept { return owner_->tcp().connection(index_); }
        [[nodiscard]] const ConnectionType& connection() const noexcept { return owner_->tcp().connection(index_); }

        [[nodiscard]] View unread() const noexcept { return connection().unread(); }
        void consume(const std::size_t count) noexcept { connection().consume(count); }
        [[nodiscard]] bool peer_closed() const noexcept { return connection().peer_closed(); }
        [[nodiscard]] std::size_t writable() const noexcept { return connection().writable(); }
        [[nodiscard]] std::optional<std::span<std::byte>> prepare(const std::size_t count) noexcept { return connection().prepare(count); }
        [[nodiscard]] bool commit(const std::size_t count) noexcept { return connection().commit(count); }
        [[nodiscard]] Sequence committed() const noexcept { return connection().committed(); }
        [[nodiscard]] Sequence acknowledged() const noexcept { return connection().acknowledged(); }
        [[nodiscard]] std::size_t unacknowledged() const noexcept { return connection().unacknowledged(); }
        [[nodiscard]] stream::Events events() const noexcept { return connection().events(); }
        [[nodiscard]] tcp::State state() const noexcept { return connection().state(); }
        [[nodiscard]] tcp::Endpoint local() const noexcept { return connection().local(); }
        [[nodiscard]] tcp::Endpoint remote() const noexcept { return connection().remote(); }
        [[nodiscard]] std::uint16_t mss() const noexcept { return connection().mss(); }

        /// `unread().size() >= count`, the bytes; `PeerClosed` with fewer; `Reset`, `TimedOut`.
        [[nodiscard]] detail::StreamSender<Stack, detail::ReadablePolicy> readable(const std::size_t count) noexcept {
            return {owner_, index_, count, {}};
        }
        /// `writable() >= count`, at most the MSS, the credit; `Closed` once closed.
        [[nodiscard]] detail::StreamSender<Stack, detail::WritablePolicy> writable(const std::size_t count) noexcept {
            return {owner_, index_, count, {}};
        }
        /// `acknowledged()` reaches `sequence`.
        [[nodiscard]] detail::StreamSender<Stack, detail::AckedPolicy> acked(const Sequence sequence) noexcept {
            return {owner_, index_, sequence.value, {}};
        }
        /// A normal `Closed`; `Reset`, `TimedOut`.
        [[nodiscard]] detail::StreamSender<Stack, detail::ClosedPolicy> closed() noexcept {
            return {owner_, index_, 0, detail::ClosedPolicy{.close_first = false}};
        }
        /// Commits all of `bytes`, parking on writable between segments; `bytes` must outlive the operation.
        [[nodiscard]] detail::StreamSender<Stack, detail::SendPolicy> send(const std::span<const std::byte> bytes) noexcept {
            return {owner_, index_, 1, detail::SendPolicy{.bytes = bytes}};
        }
        /// The brick's close, then `closed()`.
        [[nodiscard]] detail::StreamSender<Stack, detail::ClosedPolicy> close() noexcept {
            return {owner_, index_, 0, detail::ClosedPolicy{.close_first = true}};
        }

        void cancel() noexcept { owner_->cancel(index_); }
        void deadline(const core::TimePoint when) noexcept { owner_->deadline(index_, when); }

        /// Frees the slot through the owner; idempotent; the handle is inert afterwards.
        void release() noexcept {
            if (owner_ != nullptr) {
                owner_->release(index_);
                owner_ = nullptr;
            }
        }

    private:
        Streams<Stack>* owner_ = nullptr;
        std::uint32_t index_   = 0;
    };

}  // namespace aloe::runtime
```

Register in `common/runtime/CMakeLists.txt` a `${RUNTIME}.Stream` interface target with the four headers and include directory `stream/`, linking `${RUNTIME}.Scheduler`, `Aloe::Common::Tcp`, `Aloe::Common::Stream` and `Aloe::Common::Net`; add it to the exported library's `LIBRARIES`. Add `#include <streams.hpp>` to `common/runtime/export/aloe/runtime` (it pulls the handle and the senders).

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Runtime Aloe.Tests.Unit.Tcp && ctest --preset debug -R '^Aloe[.]Tests[.]Unit[.](Runtime|Tcp)$' --output-on-failure`
Expected: PASS, the old scheduler and task tests included. Then `cmake --preset asan && cmake --build --preset asan --target Aloe.Tests.Unit.Runtime && ctest --preset asan -R '^Aloe[.]Tests[.]Unit[.]Runtime$' --output-on-failure` for the queued-completion and slot-reuse cases.

- [ ] **Step 5: Document and format**

In `docs/architecture/stream.md`, complete the senders paragraph: the table of senders and their completions, errors in the value channel, stopped for cancellation, deadlines and shutdown. In `docs/architecture/runtime.md`, add `Streams`, `Stream`, the wait slot and its lifetime rules, `connection()` versus the same-named senders, and that all lifecycle calls for managed connections go through the owner. Run `./scripts/check-format.sh`.

- [ ] **Step 6: Commit**

```bash
git add common/runtime tests/unit_tests/common/runtime docs/architecture/stream.md docs/architecture/runtime.md
git commit -m "feat(runtime): wrap stream readiness in shard senders"
```

## Task 12: The ready-made TCP stack, ARP forwarding and threaded steering

**Files:**
- Create: `common/runtime/stack/tcp_runtime_stack.hpp`, `common/runtime/stack/arp_forward.hpp`
- Create: `tests/unit_tests/common/runtime/test_tcp_runtime.cpp`, `tests/unit_tests/common/runtime/test_tcp_steering.cpp`
- Modify: `common/runtime/CMakeLists.txt`, `common/runtime/export/aloe/runtime`, `tests/unit_tests/common/runtime/CMakeLists.txt`, `docs/architecture/runtime.md`, `docs/architecture/net.md`

**Interfaces:**
- Produces: `runtime::TcpStack<Device>(ShardContext&, loop::ShardQueue<Device>&, const net::Ipv4Config&, const tcp::TcpConfig&)` modelling `IsStack` with both hooks; nested `Ip = net::Ipv4<Device>`, `Tcp = tcp::Stack<Ip>`, `StreamsType = Streams<Tcp>`, `StreamType = Stream<Tcp>`; `ip()`, `tcp()`, `streams()`, `forwards_dropped()`; `on_receive` is IP then TCP at the context's stamp, then one forwarding pass over `ip.resolved()`; `on_tick` is an empty TCP process then a wake; `on_flush` is a wake then the TCP flush. Members are declared IP, TCP, Streams, so they are destroyed in the reverse order. `detail::ArpForwardWork` carries a target context, an address and a MAC; its run delivers through `deliver_arp` and frees itself; `detail::forward_resolution(siblings, address, mac)` allocates one node per sibling with `std::nothrow`, posts it through `post_control`, frees a rejected node and returns how many could not be allocated.

- [ ] **Step 1: Write the failing tests**

`tests/unit_tests/common/runtime/test_tcp_runtime.cpp`, deterministic hook tests on standalone shards:

```cpp
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/runtime>
#include <aloe/stream>
#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#include <net_fixture.hpp>
#include <tcp_fixture.hpp>

namespace {

    using namespace std::chrono_literals;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpSequence;
    using Stack     = aloe::runtime::TcpStack<aloe::fabric::Port>;
    using Shard     = aloe::runtime::Shard<aloe::fabric::Port, Stack>;
    using TimePoint = aloe::core::TimePoint;

    static_assert(aloe::runtime::IsStack<Stack, aloe::fabric::Port>);
    static_assert(aloe::runtime::detail::HasOnTick<Stack> && aloe::runtime::detail::HasOnFlush<Stack>);

    constexpr TimePoint start{};

    class TcpRuntime : public ::testing::Test {
    protected:
        aloe::fabric::Fabric fabric_;
        aloe::fabric::Port& harness_ = fabric_.add_port({.mac = aloe::testing::harness_mac, .pool_size = 256, .queue_depth = 4096});
        aloe::fabric::Port& port_    = fabric_.add_port({.mac = aloe::testing::stack_mac, .pool_size = 128});
        aloe::runtime::ShardConfig config_{.transmit_ring = 64, .idle = aloe::runtime::IdlePolicy::Yield};
        Shard shard_{config_, port_, 0, start, aloe::testing::stack_config(), aloe::tcp::TcpConfig{}};
        TimePoint now_ = start;
        aloe::frames::TcpPeer peer_{aloe::testing::peer_spec(), TcpSequence{aloe::testing::tcp_peer_isn}};

        TcpRuntime() {
            shard_.stack().ip().learn(aloe::testing::harness_ip, aloe::testing::harness_mac, start);
        }

        void inject(const std::span<const std::byte> frame) {
            auto packet = harness_.allocate(0);
            ASSERT_TRUE(packet.has_value());
            ASSERT_TRUE(aloe::frames::fill(*packet, frame));
            std::array<aloe::fabric::Packet, 1> out{std::move(*packet)};
            ASSERT_EQ(harness_.transmit(0, out), 1U);
        }

        bool step(const aloe::core::Duration by = 1ms) {
            now_ += by;
            return shard_.step(now_);
        }

        [[nodiscard]] std::vector<aloe::frames::ParsedFrame> on_the_wire() {
            std::vector<aloe::frames::ParsedFrame> frames;
            std::array<aloe::fabric::Packet, 16> out;
            for (std::size_t count = harness_.receive(0, out); count > 0; count = harness_.receive(0, out)) {
                for (std::size_t index = 0; index < count; ++index) {
                    frames.push_back(aloe::frames::parse_frame(aloe::frames::bytes_of(out[index])).value());
                    out[index] = aloe::fabric::Packet{};
                }
            }
            return frames;
        }

        /// Spawns on the shard thread, which this test thread plays.
        template <typename Task>
        void spawn(Task&& task) {
            const aloe::runtime::ShardContext::Current current{shard_.context()};
            shard_.scheduler().spawn(std::forward<Task>(task));
        }
    };

    /// Echoes one connection, then closes it; the brick's reply carries the ACK of what it answers.
    aloe::runtime::task<void> echo(Stack::StreamType stream, int* echoed) {
        for (;;) {
            const auto readable = co_await stream.readable(1);
            if (!readable) {
                break;
            }
            while (!stream.unread().empty()) {
                const auto chunk = stream.unread().front();
                if (!co_await stream.send(chunk)) {
                    co_return;
                }
                stream.consume(chunk.size());
                ++*echoed;
            }
        }
        co_await stream.close();
    }

    aloe::runtime::task<void> serve(Stack::StreamsType* streams, aloe::runtime::Scheduler scheduler, int* echoed) {
        for (;;) {
            auto stream = co_await streams->accept(aloe::testing::tcp_listen_port);
            if (!stream) {
                break;
            }
            scheduler.spawn(echo(std::move(*stream), echoed));
        }
    }

    aloe::runtime::task<void> connect_and_record(Stack::StreamsType* streams,
                                                 const aloe::tcp::Endpoint peer,
                                                 std::optional<aloe::stream::Error>* outcome,
                                                 int* completions) {
        auto stream = co_await streams->connect(peer);
        ++*completions;
        if (!stream) {
            *outcome = stream.error();
        }
    }

}  // namespace

TEST_F(TcpRuntime, SameTickReplyPiggybacksAck) {
    int echoed = 0;
    spawn(serve(&shard_.stack().streams(), shard_.scheduler(), &echoed));
    EXPECT_TRUE(step());
    inject(peer_.syn());
    EXPECT_TRUE(step());
    auto frames = on_the_wire();
    ASSERT_EQ(frames.size(), 1U);
    peer_.see(frames[0]);
    inject(peer_.ack());
    EXPECT_TRUE(step());
    EXPECT_TRUE(on_the_wire().empty());
    inject(peer_.data(aloe::frames::pattern(10)));
    EXPECT_TRUE(step());
    frames = on_the_wire();
    ASSERT_EQ(frames.size(), 1U) << "the reply, and no pure ACK before or after it";
    EXPECT_EQ(aloe::frames::tcp_payload(frames[0]).size(), 10U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, peer_.snd_nxt());
    EXPECT_EQ(shard_.stack().tcp().counters().pure_acks_sent, 0U);
    EXPECT_EQ(echoed, 1);
    EXPECT_EQ(shard_.context().counters().work_run, 2U) << "the accept completion and the readable completion";
}

TEST_F(TcpRuntime, ZeroWindowReopeningIsAdvertisedInTheSameTick) {
    aloe::fabric::Port& tiny_port = fabric_.add_port({.mac = {0x02, 0, 0, 0, 0, 0x33}, .pool_size = 128});
    Shard tiny{config_, tiny_port, 0, start, aloe::net::Ipv4Config{.address = {10, 0, 0, 3}, .prefix = 24},
               aloe::tcp::TcpConfig{.receive_segments = 1}};
    tiny.stack().ip().learn(aloe::testing::harness_ip, aloe::testing::harness_mac, start);
    int echoed = 0;
    {
        const aloe::runtime::ShardContext::Current current{tiny.context()};
        tiny.scheduler().spawn(serve(&tiny.stack().streams(), tiny.scheduler(), &echoed));
    }
    aloe::frames::TcpSpec spec   = aloe::testing::peer_spec();
    spec.destination_mac         = {0x02, 0, 0, 0, 0, 0x33};
    spec.destination             = {10, 0, 0, 3};
    aloe::frames::TcpPeer peer{spec, TcpSequence{1000U}};
    std::ignore = tiny.step(now_ += 1ms);
    inject(peer.syn());
    std::ignore = tiny.step(now_ += 1ms);
    auto frames = on_the_wire();
    ASSERT_EQ(frames.size(), 1U);
    peer.see(frames[0]);
    inject(peer.ack());
    std::ignore = tiny.step(now_ += 1ms);
    inject(peer.data(aloe::frames::pattern(1460)));  // the whole budget
    std::ignore = tiny.step(now_ += 1ms);
    frames = on_the_wire();
    ASSERT_EQ(frames.size(), 1U) << "the echo of the full segment";
    EXPECT_EQ(frames[0].tcp->window, 1460U) << "consumed by the task before the reply: the reopening rides along";
}

TEST_F(TcpRuntime, TimerEventRunsOnNextEmptyStepAndSurvivesAReceive) {
    std::optional<aloe::stream::Error> outcome;
    int completions = 0;
    spawn(connect_and_record(&shard_.stack().streams(), {.address = aloe::testing::harness_ip, .port = 40001}, &outcome, &completions));
    std::ignore = step();
    EXPECT_EQ(on_the_wire().size(), 1U) << "the SYN nobody answers";
    for (int second = 1; second < 63; ++second) {
        std::ignore = step(1s);
        EXPECT_EQ(completions, 0);
    }
    std::ignore = step(1s);  // the timeout fires inside run_once; its completion is queued at on_flush
    EXPECT_EQ(completions, 0) << "not yet: queued work runs on the next step";
    inject(aloe::frames::arp_frame(aloe::frames::arp_request(aloe::testing::harness_mac, aloe::testing::harness_ip, aloe::testing::stack_ip),
                                   aloe::wire::MacAddress::broadcast()));
    std::ignore = step();  // a receive pass does not erase it
    EXPECT_EQ(completions, 1);
    EXPECT_EQ(outcome, aloe::stream::Error::TimedOut);
    EXPECT_EQ(shard_.stack().tcp().table_size(), 0U) << "the failed open released its slot";
}

TEST_F(TcpRuntime, ArpForwardedOnceToSiblings) {
    // Two shards over a two-queue port, bound as siblings by hand and ticked from this thread.
    constexpr aloe::wire::MacAddress pair_mac{0x02, 0, 0, 0, 0, 0x44};
    constexpr aloe::wire::Ipv4Address pair_ip{10, 0, 0, 4};
    aloe::fabric::Port& pair = fabric_.add_port({.mac = pair_mac, .queues = 2, .pool_size = 128});
    const aloe::net::Ipv4Config config{.address = pair_ip, .prefix = 24, .gateway = aloe::testing::gateway_ip};
    Shard first{config_, pair, 0, start, config, aloe::tcp::TcpConfig{}};
    Shard second{config_, pair, 1, start, config, aloe::tcp::TcpConfig{}};
    const std::array<aloe::runtime::ShardContext*, 1> for_first{&second.context()};
    const std::array<aloe::runtime::ShardContext*, 1> for_second{&first.context()};
    first.context().set_siblings(for_first);
    second.context().set_siblings(for_second);
    inject(aloe::frames::arp_frame(
        aloe::frames::arp_reply(aloe::testing::gateway_mac, aloe::testing::gateway_ip, pair_mac, pair_ip), pair_mac));  // ARP: queue 0
    std::ignore = first.step(now_ += 1ms);
    EXPECT_EQ(first.stack().ip().counters().resolutions, 1U);
    EXPECT_FALSE(second.context().inbox().empty()) << "one forward posted";
    std::ignore = first.step(now_ += 1ms);  // an empty tick forwards nothing again
    std::ignore = second.step(now_);
    EXPECT_EQ(second.context().counters().inbox_received, 1U) << "forwarded once";
    EXPECT_EQ(second.stack().ip().resolve(aloe::testing::gateway_ip, now_), aloe::testing::gateway_mac) << "learned without asking";
    EXPECT_EQ(second.stack().ip().counters().arp_requests_sent, 0U);
    std::ignore = second.step(now_ += 1ms);
    EXPECT_TRUE(first.context().inbox().empty()) << "learn does not echo the resolution back";
    EXPECT_EQ(first.stack().forwards_dropped() + second.stack().forwards_dropped(), 0U);
}
```

`tests/unit_tests/common/runtime/test_tcp_steering.cpp`, for the `Runtime.Threads` target:

```cpp
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/runtime>
#include <aloe/stream>
#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

// Four shards of TcpStack serve the task echo; a client runtime opens connections to them; every reply is
// stamped with the serving shard, which must be the queue the card's hash selects. Then a connect through a
// gateway only shard 0 can learn, and stop racing the ARP forwards. The tsan preset exists for this file.
namespace {

    using namespace std::chrono_literals;
    using Stack   = aloe::runtime::TcpStack<aloe::fabric::Port>;
    using Runtime = aloe::runtime::Runtime<aloe::fabric::Port, Stack>;
    using Stream  = Stack::StreamType;

    constexpr std::uint16_t queues    = 4;
    constexpr std::size_t connections = 24;
    constexpr auto patience           = 20s;
    constexpr aloe::wire::MacAddress server_mac{0x02, 0, 0, 0, 0, 0x01};
    constexpr aloe::wire::MacAddress client_mac{0x02, 0, 0, 0, 0, 0x02};
    constexpr aloe::wire::MacAddress gateway_mac{0x02, 0, 0, 0, 0, 0xfe};
    constexpr aloe::wire::Ipv4Address server_ip{10, 0, 0, 2};
    constexpr aloe::wire::Ipv4Address client_ip{10, 0, 0, 1};
    constexpr aloe::wire::Ipv4Address gateway_ip{10, 0, 0, 254};
    constexpr aloe::wire::Ipv4Address far_ip{192, 168, 7, 7};

    [[nodiscard]] bool eventually(const std::function<bool()>& condition) {
        const auto deadline = std::chrono::steady_clock::now() + patience;
        while (std::chrono::steady_clock::now() < deadline) {
            if (condition()) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return condition();
    }

    /// Echoes what it reads with the serving shard's index appended, then closes.
    aloe::runtime::task<void> echo(Stream stream, const std::uint16_t index) {
        const auto readable = co_await stream.readable(4);
        if (!readable) {
            co_return;
        }
        std::array<std::byte, 5> reply{};
        std::ranges::copy(stream.unread().front().first(4), reply.begin());
        reply[4] = static_cast<std::byte>(index);
        stream.consume(4);
        if (!co_await stream.send(reply)) {
            co_return;
        }
        co_await stream.close();
    }

    aloe::runtime::task<void> serve(Stack::StreamsType* streams, aloe::runtime::Scheduler scheduler, const std::uint16_t index) {
        for (;;) {
            auto stream = co_await streams->accept(7);
            if (!stream) {
                break;
            }
            scheduler.spawn(echo(std::move(*stream), index));
        }
    }

    /// One connect through the gateway, from the last shard; reports whether it reached Established.
    aloe::runtime::task<void> connect_far(Stack::StreamsType* streams, std::atomic<int>* done, std::atomic<bool>* ok) {
        auto stream = co_await streams->connect({.address = far_ip, .port = 7});
        ok->store(stream.has_value());
        done->fetch_add(1);
        if (stream) {
            stream->release();
        }
    }

    /// Opens `connections` one after another and checks each stamp against `queue_for`.
    aloe::runtime::task<void> client(Stack::StreamsType* streams,
                                     const aloe::device::RssDescription* steering,
                                     std::atomic<int>* done,
                                     std::atomic<int>* wrong) {
        for (std::size_t flow = 0; flow < connections; ++flow) {
            auto stream = co_await streams->connect({.address = server_ip, .port = 7});
            if (!stream) {
                wrong->fetch_add(100);
                break;
            }
            const std::array<std::byte, 4> ping{std::byte{'p'}, std::byte{'i'}, std::byte{'n'}, std::byte{'g'}};
            if (!co_await stream->send(ping)) {
                wrong->fetch_add(100);
                break;
            }
            const auto readable = co_await stream->readable(5);
            if (!readable) {
                wrong->fetch_add(100);
                break;
            }
            const std::uint16_t expected = aloe::device::queue_for(*steering,
                                                                   {.source           = client_ip,
                                                                    .destination      = server_ip,
                                                                    .source_port      = stream->local().port,
                                                                    .destination_port = 7,
                                                                    .protocol         = aloe::wire::Ipv4Protocol::Tcp});
            if (std::to_integer<std::uint16_t>(stream->unread().front()[4]) != expected) {
                wrong->fetch_add(1);
            }
            stream->consume(5);
            co_await stream->close();
            done->fetch_add(1);
        }
    }

}  // namespace

TEST(TcpSteering, AllConnectionsStayOnTheirShard) {
    aloe::fabric::Fabric fabric;
    auto& server_port = fabric.add_port({.mac = server_mac, .queues = queues, .pool_size = 256});
    auto& client_port = fabric.add_port({.mac = client_mac, .queues = 1, .pool_size = 256, .queue_depth = 4096});
    std::atomic<int> done{0};
    std::atomic<int> wrong{0};
    const aloe::runtime::RuntimeConfig config{.shard = {.idle = aloe::runtime::IdlePolicy::Yield, .yield_after = 10}};
    Runtime server{config, server_port, aloe::net::Ipv4Config{.address = server_ip, .prefix = 24}, aloe::tcp::TcpConfig{}};
    Runtime client{config, client_port, aloe::net::Ipv4Config{.address = client_ip, .prefix = 24}, aloe::tcp::TcpConfig{}};
    for (std::uint16_t index = 0; index < queues; ++index) {
        server.shard(index).stack().ip().learn(client_ip, client_mac, std::chrono::steady_clock::now());
    }
    client.shard(0).stack().ip().learn(server_ip, server_mac, std::chrono::steady_clock::now());
    server.start();
    client.start();
    for (std::uint16_t index = 0; index < queues; ++index) {
        server.spawn(index, serve(&server.shard(index).stack().streams(), server.scheduler(index), index));
    }
    client.spawn(0, client(&client.shard(0).stack().streams(), &server_port.steering(), &done, &wrong));
    EXPECT_TRUE(eventually([&] { return done.load() == connections || wrong.load() >= 100; }));
    client.stop();
    server.stop();
    client.join();
    server.join();
    EXPECT_EQ(done.load(), connections);
    EXPECT_EQ(wrong.load(), 0) << "every reply came from the queue the hash selects";
    std::uint64_t accepted = 0;
    for (std::uint16_t index = 0; index < queues; ++index) {
        const auto& counters = server.shard(index).stack().tcp().counters();
        accepted += counters.connections_accepted;
        EXPECT_EQ(counters.dropped_no_connection, 0U) << "shard " << index;
        EXPECT_EQ(counters.dropped_unexpected, 0U) << "shard " << index;
    }
    EXPECT_EQ(accepted, connections);
    EXPECT_EQ(client.shard(0).stack().tcp().counters().dropped_no_connection, 0U);
}

TEST(TcpSteering, GatewayKnownOnlyByQueueZero) {
    aloe::fabric::Fabric fabric;
    auto& server_port  = fabric.add_port({.mac = server_mac, .queues = queues, .pool_size = 256});
    auto& gateway_port = fabric.add_port({.mac = gateway_mac, .queues = 1, .pool_size = 128, .queue_depth = 4096});
    std::atomic<int> completions{0};
    std::atomic<bool> connected{false};
    std::atomic<bool> stop_gateway{false};
    const aloe::runtime::RuntimeConfig config{.shard = {.idle = aloe::runtime::IdlePolicy::Yield, .yield_after = 10}};
    Runtime server{config, server_port,
                   aloe::net::Ipv4Config{.address = server_ip, .prefix = 24, .gateway = gateway_ip}, aloe::tcp::TcpConfig{}};
    // The gateway, scripted on this thread: answers the ARP request (which lands on queue 0 of the server, being
    // ARP) and the SYN from far_ip behind it, so the SYN-ACK hashes back to the shard that chose the port.
    std::jthread gateway{[&] {
        std::vector<aloe::fabric::Packet> burst(16);
        while (!stop_gateway.load()) {
            const std::size_t count = gateway_port.receive(0, burst);
            for (std::size_t index = 0; index < count; ++index) {
                const auto frame = aloe::frames::parse_frame(aloe::frames::bytes_of(burst[index]));
                burst[index]     = aloe::fabric::Packet{};
                if (!frame) {
                    continue;
                }
                std::vector<std::byte> reply;
                if (frame->arp && frame->arp->operation == aloe::wire::ArpOperation::Request && frame->arp->target_ip == gateway_ip) {
                    reply = aloe::frames::arp_frame(aloe::frames::arp_reply(gateway_mac, gateway_ip, frame->arp->sender_mac, frame->arp->sender_ip),
                                                    frame->arp->sender_mac);
                } else if (frame->tcp && frame->tcp->flags == aloe::wire::TcpFlags{aloe::wire::TcpFlag::Syn}) {
                    aloe::frames::TcpPeer far{{.destination_mac = server_mac, .source_mac = gateway_mac, .source = far_ip,
                                               .destination = server_ip, .source_port = 7, .destination_port = frame->tcp->source_port, .mss = 1460},
                                              aloe::wire::TcpSequence{5000U}};
                    reply = far.syn_ack(*frame);
                } else {
                    continue;
                }
                auto packet = gateway_port.allocate(0);
                if (packet && aloe::frames::fill(*packet, reply)) {
                    std::array<aloe::fabric::Packet, 1> out{std::move(*packet)};
                    std::ignore = gateway_port.transmit(0, out);
                }
            }
            if (count == 0) {
                std::this_thread::sleep_for(1ms);
            }
        }
    }};
    server.start();
    server.spawn(queues - 1, connect_far(&server.shard(queues - 1).stack().streams(), &completions, &connected));
    EXPECT_TRUE(eventually([&] { return completions.load() == 1; }));
    server.stop();
    server.join();
    stop_gateway.store(true);
    gateway.join();
    EXPECT_TRUE(connected.load());
    EXPECT_GE(server.shard(queues - 1).stack().tcp().counters().send_unresolved, 1U) << "the first SYN waited for ARP";
    EXPECT_EQ(server.shard(0).stack().ip().counters().resolutions, 1U) << "queue 0 learned the gateway from the wire";
    EXPECT_GE(server.counters(queues - 1).inbox_received, 1U) << "shard 3 learned it from shard 0";
}

TEST(TcpSteering, StopRacesArpForward) {
    for (int round = 0; round < 20; ++round) {
        aloe::fabric::Fabric fabric;
        auto& server_port = fabric.add_port({.mac = server_mac, .queues = queues, .pool_size = 128});
        auto& peer_port   = fabric.add_port({.mac = client_mac, .queues = 1, .pool_size = 128});
        const aloe::runtime::RuntimeConfig config{.shard = {.idle = aloe::runtime::IdlePolicy::Yield, .yield_after = 1}};
        Runtime server{config, server_port, aloe::net::Ipv4Config{.address = server_ip, .prefix = 24}, aloe::tcp::TcpConfig{}};
        server.start();
        std::jthread replies{[&] {
            for (int i = 0; i < 200; ++i) {
                auto packet = peer_port.allocate(0);
                if (!packet) {
                    continue;
                }
                const aloe::wire::Ipv4Address who{10, 0, 0, static_cast<std::uint8_t>(10 + (i % 100))};
                if (aloe::frames::fill(*packet, aloe::frames::arp_frame(aloe::frames::arp_reply(client_mac, who, server_mac, server_ip), server_mac))) {
                    std::array<aloe::fabric::Packet, 1> out{std::move(*packet)};
                    std::ignore = peer_port.transmit(0, out);
                }
                if (i == 50 + round) {
                    server.stop();  // while forwards are in flight
                }
            }
        }};
        replies.join();
        server.stop();
        server.join();
        // Every allocated node ran and freed itself, or was rejected and freed by the sender: ASan sees no leak,
        // TSan no race between post_control and the final drain.
        for (std::uint16_t index = 0; index < queues; ++index) {
            EXPECT_EQ(server.shard(index).stack().forwards_dropped(), 0U);
        }
    }
}
```

Add `test_tcp_runtime.cpp` to `Aloe.Tests.Unit.Runtime` and `test_tcp_steering.cpp` to `Aloe.Tests.Unit.Runtime.Threads`; both targets link `Aloe::Common::Tcp`, `Aloe::Common::Net`, `Aloe::Fixtures::Frames`, `${SHARED_TESTING_TARGET}.Net` and `${SHARED_TESTING_TARGET}.Tcp` in addition to what they have.

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Runtime Aloe.Tests.Unit.Runtime.Threads`
Expected: compile errors, `no member named 'TcpStack' in namespace 'aloe::runtime'`.

- [ ] **Step 3: Write the forwarding node and the stack**

`common/runtime/stack/arp_forward.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <new>
#include <span>

#include <ipv4_address.hpp>
#include <mac_address.hpp>
#include <shard_context.hpp>
#include <work.hpp>

namespace aloe::runtime::detail {

    /**
     * @brief A resolution on its way to a sibling shard.
     *
     * Allocated on the cold path by the shard that learned it, posted through `post_control`, and
     * freed by the target after delivery; a rejected post is freed by the sender. Carries no device
     * and no packet: an address and a MAC.
     */
    struct ArpForwardWork : loop::Work {
        ArpForwardWork(ShardContext& to, const wire::Ipv4Address a, const wire::MacAddress m) noexcept
            : loop::Work{&ArpForwardWork::deliver},
              target{&to},
              address{a},
              mac{m} {
        }

        static void deliver(loop::Work& work) noexcept {
            auto* self = static_cast<ArpForwardWork*>(&work);
            self->target->deliver_arp(self->address, self->mac);
            delete self;
        }

        ShardContext* target;
        wire::Ipv4Address address;
        wire::MacAddress mac;
    };

    /// One node per sibling. Returns how many could not be allocated; a rejected post frees its node here.
    [[nodiscard]] inline std::size_t forward_resolution(const std::span<ShardContext* const> siblings,
                                                        const wire::Ipv4Address address,
                                                        const wire::MacAddress mac) noexcept {
        std::size_t dropped = 0;
        for (ShardContext* sibling : siblings) {
            auto* work = new (std::nothrow) ArpForwardWork{*sibling, address, mac};
            if (work == nullptr) {
                ++dropped;
                continue;
            }
            if (!sibling->post_control(*work)) {
                delete work;  // that shard has finished: nothing reads its inbox
            }
        }
        return dropped;
    }

}  // namespace aloe::runtime::detail
```

`common/runtime/stack/tcp_runtime_stack.hpp`:

```cpp
#pragma once

#include <aloe/core>
#include <cstddef>
#include <cstdint>
#include <span>

#include <arp_forward.hpp>
#include <datagram.hpp>
#include <device.hpp>
#include <ipv4.hpp>
#include <shard_context.hpp>
#include <shard_queue.hpp>
#include <streams.hpp>
#include <tcp_config.hpp>
#include <tcp_stack.hpp>

namespace aloe::runtime {

    /**
     * @brief The ready-made stack for a TCP shard: the IP brick, the TCP brick and the stream owner,
     * with the tick glue. Models IsStack with both hooks.
     *
     * `on_receive` runs IP then TCP at the step's stamp and forwards that pass's ARP resolutions to
     * every sibling once. `on_tick` runs an empty TCP process, which publishes retry hints, then the
     * wake pass. `on_flush` runs the wake pass again, for events timers and tasks raised, then TCP's
     * flush. A program that wants another composition writes its own IsStack over the same bricks.
     */
    template <device::IsDevice Device>
    class TcpStack {
    public:
        using Packet      = typename Device::Packet;
        using Ip          = net::Ipv4<Device>;
        using Tcp         = tcp::Stack<Ip>;
        using StreamsType = Streams<Tcp>;
        using StreamType  = Stream<Tcp>;

        TcpStack(ShardContext& context,
                 loop::ShardQueue<Device>& queue,
                 const net::Ipv4Config& ip_config,
                 const tcp::TcpConfig& tcp_config)
            : context_{&context},
              ip_{queue, ip_config},
              tcp_{ip_, context.timers(), tcp_config},
              streams_{tcp_, context} {
            context.set_arp_sink({.object = this, .learn = &TcpStack::learn});
        }

        TcpStack(const TcpStack&)            = delete;
        TcpStack& operator=(const TcpStack&) = delete;
        TcpStack(TcpStack&&)                 = delete;
        TcpStack& operator=(TcpStack&&)      = delete;

        ~TcpStack() {
            context_->set_arp_sink({});
        }

        void on_receive(const std::span<Packet> burst) noexcept {
            const core::TimePoint now = context_->now();
            ip_.process(burst, now);
            tcp_.process(ip_.received(wire::Ipv4Protocol::Tcp), now);
            for (const net::ArpResolution& resolution : ip_.resolved()) {
                forwards_dropped_ += detail::forward_resolution(context_->siblings(), resolution.address, resolution.mac);
            }
        }

        void on_tick(const core::TimePoint now) noexcept {
            tcp_.process({}, now);
            streams_.wake(now);
        }

        void on_flush(const core::TimePoint now) noexcept {
            streams_.wake(now);
            tcp_.flush(now);
        }

        [[nodiscard]] Ip& ip() noexcept { return ip_; }
        [[nodiscard]] Tcp& tcp() noexcept { return tcp_; }
        [[nodiscard]] StreamsType& streams() noexcept { return streams_; }
        /// Forwards that could not be allocated; read on the shard or after it stopped.
        [[nodiscard]] std::uint64_t forwards_dropped() const noexcept { return forwards_dropped_; }

    private:
        static void learn(void* object,
                          const wire::Ipv4Address address,
                          const wire::MacAddress mac,
                          const core::TimePoint now) noexcept {
            static_cast<TcpStack*>(object)->ip_.learn(address, mac, now);  // reports nothing: no echo between shards
        }

        ShardContext* context_;
        Ip ip_;             ///< Declared first: destroyed last.
        Tcp tcp_;           ///< Over `ip_` and the context's wheel.
        StreamsType streams_;  ///< Over `tcp_`: destroyed first.
        std::uint64_t forwards_dropped_ = 0;
    };

}  // namespace aloe::runtime
```

Register `${RUNTIME}.Stack` in `common/runtime/CMakeLists.txt` with the two headers and `stack/` on the include path, linking `${RUNTIME}.Stream`, `${RUNTIME}.Context`, `Aloe::Common::Tcp`, `Aloe::Common::Net`; add it to the exported library and `#include <tcp_runtime_stack.hpp>` to the umbrella.

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Runtime Aloe.Tests.Unit.Runtime.Threads Aloe.Tests.Unit.Tcp && ctest --preset debug -R '^Aloe[.]Tests[.]Unit[.](Runtime|Runtime[.]Threads|Tcp)$' --output-on-failure`
Expected: PASS. Then `cmake --preset tsan && cmake --build --preset tsan --target Aloe.Tests.Unit.Runtime.Threads Aloe.Tests.Unit.Runtime && ctest --preset tsan -R '^Aloe[.]Tests[.]Unit[.]Runtime' --output-on-failure` and the same under `asan`: clean. Stop completes with pending control posts, deadline timers and parked accept, connect and readable operations.

- [ ] **Step 5: Document and format**

In `docs/architecture/runtime.md`, add `TcpStack`, its tick, the forwarding rule (once per receive pass, never on an empty tick, `learn` does not echo) and the ownership of a forward node. In `docs/architecture/net.md`, replace the "deferred" note about cross-shard ARP with the forwarding that now exists in the runtime and the hand-written channel a multi-queue program owns (one `loop::Inbox` per loop, a `Work` carrying the resolution). Run `./scripts/check-format.sh`.

- [ ] **Step 6: Commit**

```bash
git add common/runtime tests/unit_tests/common/runtime docs/architecture/runtime.md docs/architecture/net.md
git commit -m "feat(runtime): compose TCP shards and forward ARP resolutions"
```

## Task 13: Real-mbuf integration and Linux tap interoperability

**Files:**
- Create: `tests/integration_tests/tcp/CMakeLists.txt`, `tests/integration_tests/tcp/test_tcp_ring.cpp`, `tests/manual_tests/tcp/CMakeLists.txt`, `tests/manual_tests/tcp/test_tcp_tap.cpp`
- Modify: `tests/integration_tests/CMakeLists.txt`, `tests/manual_tests/CMakeLists.txt`, `docs/guides/getting-started.md`

**Interfaces:**
- Produces: targets `Aloe.Tests.Integration.Tcp.Ring` (no root; the brick over `net_ring`, which loops a queue's transmit into its receive, with a scripted peer at another address) and `Aloe.Tests.Manual.Tcp.Tap` (root; four scenarios against the kernel over `net_tap0,iface=aloe-tcp`, kernel at 10.78.0.1/24, Aloe at 10.78.0.2/24). Each ring case probes a fresh port; the ring test drains what the stack transmitted before injecting the peer's answer and never feeds the stack's own frames back into IP.

- [ ] **Step 1: Write the ring test**

`tests/integration_tests/tcp/CMakeLists.txt`:

```cmake
##############################################################################
# The TCP brick over the ring driver: a scripted peer on real mbufs, no root
##############################################################################
add_integration_test(${INTEGRATION_TESTING_TARGET}.Tcp.Ring
        test_tcp_ring.cpp
)
target_link_libraries(${INTEGRATION_TESTING_TARGET}.Tcp.Ring
        PRIVATE
        Aloe::Common::Tcp
        Aloe::Common::Net
        Aloe::Common::Ethdev
        Aloe::Fixtures::Frames
        ${SHARED_TESTING_TARGET}.Ethdev
        ${TEST_LIBS}
)
##############################################################################
```

Add `add_subdirectory(tcp)` to `tests/integration_tests/CMakeLists.txt`.

`tests/integration_tests/tcp/test_tcp_ring.cpp`:

```cpp
#include <aloe/ethdev>
#include <aloe/frames>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/stream>
#include <aloe/tcp>
#include <aloe/wire>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <eal_environment.hpp>
#include <gtest/gtest.h>

// The brick over DPDK's ring driver, which loops a queue's transmit back into its own receive. A scripted
// peer at another address drives it: what the stack transmits is read back from the ring and never fed to
// IP again; the peer's frames are injected and processed. Real mbufs, headroom, offsets, software checksums.
namespace {

    using namespace std::chrono_literals;
    using Packet    = aloe::ethdev::Packet;
    using Ipv4      = aloe::net::Ipv4<aloe::ethdev::Port>;
    using Tcp       = aloe::tcp::Stack<Ipv4>;
    using TimePoint = aloe::core::TimePoint;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;
    using aloe::wire::TcpSequence;

    constexpr aloe::wire::Ipv4Address stack_ip{10, 0, 0, 2};
    constexpr aloe::wire::Ipv4Address peer_ip{10, 0, 0, 1};
    constexpr aloe::wire::MacAddress peer_mac{0x02, 0, 0, 0, 0xfe, 0xed};

    const auto* const environment = ::testing::AddGlobalTestEnvironment(new aloe::testing::EalEnvironment{"net_ring0"});

    class TcpRing : public ::testing::Test {
    protected:
        aloe::ethdev::Port port_{
            aloe::ethdev::PortConfig{.name = aloe::testing::probe_vdev("net_ring"), .queues = 1, .pool_size = 256}};
        aloe::loop::ShardCounters counters_;
        aloe::loop::ShardQueue<aloe::ethdev::Port> queue_{port_, 0, 16, counters_};
        Ipv4 ip_{queue_, {.address = stack_ip, .prefix = 24}};
        aloe::loop::TimerWheel wheel_{1ms, TimePoint{}};
        Tcp tcp_{ip_, wheel_, {}};
        TimePoint now_{};
        std::vector<Packet> burst_ = std::vector<Packet>(64);
        aloe::frames::TcpPeer peer_{{.destination_mac  = port_.mac(),
                                     .source_mac       = peer_mac,
                                     .source           = peer_ip,
                                     .destination      = stack_ip,
                                     .source_port      = 40000,
                                     .destination_port = 7,
                                     .mss              = 1460},
                                    TcpSequence{1000U}};

        TcpRing() {
            ip_.learn(peer_ip, peer_mac, now_);
        }

        /// Flushes, then reads back what the stack put on the ring: its own transmissions, taken apart.
        [[nodiscard]] std::vector<aloe::frames::ParsedFrame> collect() {
            tcp_.flush(now_);
            std::ignore = queue_.flush();
            std::vector<aloe::frames::ParsedFrame> frames;
            std::array<Packet, 16> out;
            for (std::size_t count = port_.receive(0, out); count > 0; count = port_.receive(0, out)) {
                for (std::size_t index = 0; index < count; ++index) {
                    frames.push_back(aloe::frames::parse_frame(aloe::frames::bytes_of(out[index])).value());
                    out[index] = Packet{};
                }
            }
            return frames;
        }

        /// Puts the peer's frame on the ring and processes it: the ring must be empty of our own frames first.
        void receive(const std::span<const std::byte> frame) {
            auto packet = port_.allocate(0);
            ASSERT_TRUE(packet.has_value());
            ASSERT_TRUE(aloe::frames::fill(*packet, frame));
            std::array<Packet, 1> out{std::move(*packet)};
            ASSERT_EQ(port_.transmit(0, out), 1U);
            const std::size_t received = port_.receive(0, burst_);
            ASSERT_EQ(received, 1U) << "only the injected frame: our own were drained by collect()";
            ip_.process(std::span<Packet>{burst_}.first(received), now_);
            tcp_.process(ip_.received(aloe::wire::Ipv4Protocol::Tcp), now_);
            ASSERT_TRUE(burst_[0].empty()) << "taken or freed";
        }

        [[nodiscard]] std::vector<std::byte> text(const char* s) {
            std::vector<std::byte> out;
            for (; *s != '\0'; ++s) {
                out.push_back(static_cast<std::byte>(*s));
            }
            return out;
        }
    };

}  // namespace

TEST_F(TcpRing, ScriptedPeerConnectsSendsAndCloses) {
    ASSERT_TRUE(tcp_.listen(7).has_value());
    receive(peer_.syn());
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->flags, (TcpFlags{TcpFlag::Syn, TcpFlag::Ack}));
    EXPECT_EQ(frames[0].tcp->data_offset, 24U);
    EXPECT_EQ(frames[0].ipv4->total_length, 44U) << "20 of IPv4 and 24 of TCP: the 54-byte base plus the option";
    EXPECT_EQ(frames[0].ipv4->header_length, 20U);
    EXPECT_EQ(aloe::frames::l4_checksum_residue(*frames[0].ipv4, frames[0].l4), 0U) << "software checksum on a real mbuf";
    EXPECT_EQ(aloe::wire::internet_checksum(frames[0].ipv4_header), 0U);
    peer_.see(frames[0]);
    receive(peer_.ack());
    const auto event = tcp_.poll_event();
    ASSERT_TRUE(event.has_value());
    ASSERT_TRUE(event->events.accepted());
    auto& c = *event->connection;
    EXPECT_EQ(c.mss(), 1460U);

    receive(aloe::frames::with_ipv4_options(peer_.data(text("hello ring")), 2));  // options: a nonzero L4 offset in a real mbuf
    EXPECT_EQ(c.unread().size(), 10U);
    std::vector<std::byte> seen;
    for (const std::span<const std::byte> chunk : c.unread()) {
        seen.insert(seen.end(), chunk.begin(), chunk.end());
    }
    EXPECT_EQ(seen, text("hello ring"));
    c.consume(6);
    EXPECT_EQ(c.unread().size(), 4U) << "partial consumption keeps the mbuf";
    const auto reply = text("ring");
    const auto out   = c.prepare(reply.size());
    ASSERT_TRUE(out.has_value());
    std::ranges::copy(reply, out->begin());
    ASSERT_TRUE(c.commit(reply.size()));
    frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "the reply carried the ACK";
    EXPECT_EQ(frames[0].tcp->flags, (TcpFlags{TcpFlag::Psh, TcpFlag::Ack}));
    EXPECT_EQ(frames[0].tcp->acknowledgement, peer_.snd_nxt());
    EXPECT_EQ(frames[0].ipv4->total_length, 44U);
    EXPECT_EQ(aloe::frames::l4_checksum_residue(*frames[0].ipv4, frames[0].l4), 0U);
    const auto payload = aloe::frames::tcp_payload(frames[0]);
    EXPECT_EQ(std::vector<std::byte>(payload.begin(), payload.end()), reply);
    c.consume(4);

    receive(peer_.ack(frames[0]));
    EXPECT_EQ(c.unacknowledged(), 0U);
    receive(peer_.fin());
    EXPECT_TRUE(c.peer_closed());
    c.close();
    frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->flags, (TcpFlags{TcpFlag::Fin, TcpFlag::Ack}));
    receive(peer_.ack(frames[0]));
    EXPECT_EQ(c.state(), aloe::tcp::State::Closed);
    c.release();
    EXPECT_EQ(wheel_.pending(), 0U);
    EXPECT_EQ(tcp_.table_size(), 0U);
    EXPECT_EQ(tcp_.nodes_available(), tcp_.config().receive_pool) << "every mbuf went back";
    EXPECT_EQ(ip_.counters().dropped_martian, 0U);
    EXPECT_EQ(tcp_.counters().dropped_bad_checksum, 0U);
    EXPECT_EQ(tcp_.counters().connections_closed, 1U);
}

TEST_F(TcpRing, ActiveOpenToScriptedPeer) {
    const auto connected = tcp_.connect({.address = peer_ip, .port = 40000}, now_);
    ASSERT_TRUE(connected.has_value());
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->flags, TcpFlags{TcpFlag::Syn});
    EXPECT_EQ(aloe::frames::l4_checksum_residue(*frames[0].ipv4, frames[0].l4), 0U);
    receive(peer_.syn_ack(frames[0]));
    const auto event = tcp_.poll_event();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.connected());
    frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "the deferred handshake ACK";
    EXPECT_EQ(frames[0].tcp->flags, TcpFlags{TcpFlag::Ack});
    EXPECT_EQ(frames[0].ipv4->total_length, 40U);
    peer_.see(frames[0]);
    const auto payload = aloe::frames::pattern(2000);
    EXPECT_EQ((*connected)->send(payload), 2000U);
    frames = collect();
    ASSERT_EQ(frames.size(), 2U) << "1460 and 540";
    EXPECT_EQ(aloe::frames::tcp_payload(frames[0]).size(), 1460U);
    EXPECT_EQ(aloe::frames::tcp_payload(frames[1]).size(), 540U);
    EXPECT_EQ(frames[0].ipv4->total_length, 1500U) << "a full MTU on a real mbuf";
    for (const auto& frame : frames) {
        EXPECT_EQ(aloe::frames::l4_checksum_residue(*frame.ipv4, frame.l4), 0U);
    }
    receive(peer_.ack(frames[1]));
    EXPECT_EQ((*connected)->acknowledged(), (*connected)->committed());
    (*connected)->release();
    std::ignore = collect();
    EXPECT_EQ(wheel_.pending(), 0U);
}
```

- [ ] **Step 2: Run the ring test to see it fail, then pass**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Integration.Tcp.Ring && ctest --preset debug -R '^Aloe[.]Tests[.]Integration[.]Tcp[.]Ring$' --output-on-failure`
Expected: the first build fails only if a header is missing from a link line; otherwise PASS. (The brick exists: this task's red is the missing target, observed at the first configure.)

- [ ] **Step 3: Write the tap manual suite**

`tests/manual_tests/tcp/CMakeLists.txt`:

```cmake
##############################################################################
# The kernel talks TCP to the brick and the runtime over a tap device: needs CAP_NET_ADMIN, run by hand
##############################################################################
add_manual_test(${MANUAL_TESTING_TARGET}.Tcp.Tap
        test_tcp_tap.cpp
)
target_link_libraries(${MANUAL_TESTING_TARGET}.Tcp.Tap
        PRIVATE
        Aloe::Common::Runtime
        Aloe::Common::Tcp
        Aloe::Common::Net
        Aloe::Common::Ethdev
        ${SHARED_TESTING_TARGET}.Ethdev
        ${SHARED_TESTING_TARGET}.Log
        ${TEST_LIBS}
)
##############################################################################
```

Add `add_subdirectory(tcp)` to `tests/manual_tests/CMakeLists.txt`.

`tests/manual_tests/tcp/test_tcp_tap.cpp`:

```cpp
#include <aloe/ethdev>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/runtime>
#include <aloe/stream>
#include <aloe/tcp>
#include <aloe/wire>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <eal_environment.hpp>
#include <gtest/gtest.h>
#include <logging_environment.hpp>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

// Needs CAP_NET_ADMIN: DPDK's tap driver creates a kernel interface, the test addresses it, and kernel sockets
// talk TCP to the brick and to the runtime, in both directions. Run it as root, outside the presets:
//
//     sudo ./build/debug/tests/manual_tests/tcp/Aloe.Tests.Manual.Tcp.Tap
//
// The kernel's SYN carries SACK-permitted, timestamps and window scale, which the option parser skips; the tap
// offers no offloads, so every checksum is software on our side and verified by the kernel.
namespace {

    using namespace std::chrono_literals;
    using Port    = aloe::ethdev::Port;
    using Ipv4    = aloe::net::Ipv4<Port>;
    using Tcp     = aloe::tcp::Stack<Ipv4>;
    using Stack   = aloe::runtime::TcpStack<Port>;
    using Runtime = aloe::runtime::Runtime<Port, Stack>;

    constexpr std::string_view interface = "aloe-tcp";
    constexpr aloe::wire::Ipv4Address kernel_ip{10, 78, 0, 1};
    constexpr aloe::wire::Ipv4Address stack_ip{10, 78, 0, 2};
    constexpr aloe::wire::Ipv4Address netmask{255, 255, 255, 0};
    constexpr std::uint16_t echo_port = 7;
    constexpr auto patience           = 5000ms;

    const auto* const environment =
        ::testing::AddGlobalTestEnvironment(new aloe::testing::EalEnvironment{"net_tap0,iface=aloe-tcp"});
    const auto* const logging = ::testing::AddGlobalTestEnvironment(new aloe::testing::LoggingEnvironment{});

    /// `ip addr add` and `ip link set up`, by ioctl: the kernel end of the tap.
    void configure_interface(const std::string_view name,
                             const aloe::wire::Ipv4Address address,
                             const aloe::wire::Ipv4Address mask) {
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        ASSERT_GE(fd, 0) << std::strerror(errno);
        ifreq request{};
        std::strncpy(request.ifr_name, std::string{name}.c_str(), IFNAMSIZ - 1);
        auto& in      = *reinterpret_cast<sockaddr_in*>(&request.ifr_addr);
        in.sin_family = AF_INET;
        std::memcpy(&in.sin_addr, address.bytes().data(), aloe::wire::Ipv4Address::size);
        EXPECT_EQ(ioctl(fd, SIOCSIFADDR, &request), 0) << "SIOCSIFADDR: " << std::strerror(errno);
        std::memcpy(&in.sin_addr, mask.bytes().data(), aloe::wire::Ipv4Address::size);
        EXPECT_EQ(ioctl(fd, SIOCSIFNETMASK, &request), 0) << "SIOCSIFNETMASK: " << std::strerror(errno);
        EXPECT_EQ(ioctl(fd, SIOCGIFFLAGS, &request), 0) << "SIOCGIFFLAGS: " << std::strerror(errno);
        request.ifr_flags = static_cast<short>(request.ifr_flags | IFF_UP | IFF_RUNNING);
        EXPECT_EQ(ioctl(fd, SIOCSIFFLAGS, &request), 0) << "SIOCSIFFLAGS: " << std::strerror(errno);
        close(fd);
    }

    [[nodiscard]] sockaddr_in address_of(const aloe::wire::Ipv4Address address, const std::uint16_t port) {
        sockaddr_in in{};
        in.sin_family = AF_INET;
        in.sin_port   = htons(port);
        std::memcpy(&in.sin_addr, address.bytes().data(), aloe::wire::Ipv4Address::size);
        return in;
    }

    /// Writes everything or fails; partial writes and EINTR are the kernel's business, not the test's.
    [[nodiscard]] bool send_all(const int fd, std::span<const std::byte> bytes) {
        while (!bytes.empty()) {
            const ssize_t sent = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
            if (sent < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            bytes = bytes.subspan(static_cast<std::size_t>(sent));
        }
        return true;
    }

    /// Reads exactly `out.size()` bytes within `patience`, or returns false.
    [[nodiscard]] bool recv_all(const int fd, std::span<std::byte> out) {
        const auto deadline = std::chrono::steady_clock::now() + patience;
        while (!out.empty()) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left <= 0ms) {
                return false;
            }
            pollfd waiting{.fd = fd, .events = POLLIN, .revents = 0};
            const int ready = poll(&waiting, 1, static_cast<int>(left.count()));
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready <= 0) {
                return false;
            }
            const ssize_t got = ::recv(fd, out.data(), out.size(), 0);
            if (got < 0 && errno == EINTR) {
                continue;
            }
            if (got <= 0) {
                return false;
            }
            out = out.subspan(static_cast<std::size_t>(got));
        }
        return true;
    }

    /// True when the peer closed: a zero-length read within `patience`.
    [[nodiscard]] bool recv_eof(const int fd) {
        std::array<std::byte, 1> one{};
        pollfd waiting{.fd = fd, .events = POLLIN, .revents = 0};
        if (poll(&waiting, 1, static_cast<int>(patience.count())) <= 0) {
            return false;
        }
        return ::recv(fd, one.data(), one.size(), 0) == 0;
    }

    /// The hand-written echo over the bricks on its own thread: server when `listen`, client to `peer` otherwise.
    class BrickLoop {
    public:
        BrickLoop(Port& port, const bool listen, const std::span<const std::byte> to_send)
            : thread_{[this, &port, listen, to_send](const std::stop_token& stop) { run(port, listen, to_send, stop); }} {
        }

        BrickLoop(const BrickLoop&)            = delete;
        BrickLoop& operator=(const BrickLoop&) = delete;

        ~BrickLoop() {
            stop();
        }

        void stop() {
            if (thread_.joinable()) {
                thread_.request_stop();
                thread_.join();
            }
        }

        [[nodiscard]] const aloe::tcp::TcpCounters& counters() const noexcept { return counters_; }
        [[nodiscard]] const std::vector<std::byte>& received() const noexcept { return received_; }
        [[nodiscard]] bool clean_close() const noexcept { return clean_close_; }
        std::atomic<bool> connected{false};

    private:
        void run(Port& port, const bool listen, const std::span<const std::byte> to_send, const std::stop_token& stop) {
            aloe::ethdev::register_thread();
            aloe::loop::ShardCounters shard_counters;
            aloe::loop::ShardQueue<Port> queue{port, 0, 64, shard_counters};
            Ipv4 ip{queue, {.address = stack_ip, .prefix = 24}};
            aloe::loop::TimerWheel wheel{1ms, std::chrono::steady_clock::now()};
            Tcp tcp{ip, wheel, {}};
            std::vector<Port::Packet> burst(64);
            std::size_t offered = 0;
            if (listen) {
                std::ignore = tcp.listen(echo_port);
            } else {
                std::ignore = tcp.connect({.address = kernel_ip, .port = echo_port}, std::chrono::steady_clock::now());
            }
            while (!stop.stop_requested()) {
                const auto now             = std::chrono::steady_clock::now();
                const std::size_t received = queue.receive(burst);
                ip.process(std::span<Port::Packet>{burst}.first(received), now);
                tcp.process(ip.received(aloe::wire::Ipv4Protocol::Tcp), now);
                std::ignore = wheel.advance(now);
                while (const auto event = tcp.poll_event()) {
                    auto& c = *event->connection;
                    if (event->events.connected() || event->events.accepted()) {
                        connected.store(true);
                    }
                    if ((event->events.connected() || event->events.writable()) && !listen) {
                        offered += c.send(to_send.subspan(offered));
                    }
                    if (event->events.readable() || event->events.writable()) {
                        if (listen) {
                            echo(c);
                        } else {
                            for (const std::span<const std::byte> chunk : c.unread()) {
                                received_.insert(received_.end(), chunk.begin(), chunk.end());
                            }
                            c.consume(c.unread().size());
                            if (received_.size() == to_send.size()) {
                                c.close();
                            }
                        }
                    }
                    if (listen && c.peer_closed() && c.unread().empty() && c.state() == aloe::tcp::State::CloseWait) {
                        c.close();
                    }
                    if (event->events.closed()) {
                        clean_close_ = c.state() == aloe::tcp::State::Closed && !event->events.reset();
                        c.release();
                    } else if (event->events.reset() || event->events.timed_out()) {
                        c.release();
                    }
                }
                tcp.flush(now);
                std::ignore = queue.flush();
                if (received == 0) {
                    std::this_thread::yield();
                }
            }
            counters_ = tcp.counters();
            queue.discard();
        }

        static void echo(Tcp::ConnectionType& c) {
            while (!c.unread().empty()) {
                const std::span<const std::byte> chunk = c.unread().front();
                const auto out                         = c.prepare(chunk.size());
                if (!out) {
                    return;
                }
                std::ranges::copy(chunk.first(out->size()), out->begin());
                if (!c.commit(out->size())) {
                    return;
                }
                c.consume(out->size());
            }
        }

        aloe::tcp::TcpCounters counters_{};
        std::vector<std::byte> received_;
        bool clean_close_ = false;
        std::jthread thread_;
    };

    /// The echo as tasks over the runtime.
    aloe::runtime::task<void> echo_task(Stack::StreamType stream) {
        for (;;) {
            const auto readable = co_await stream.readable(1);
            if (!readable) {
                break;
            }
            while (!stream.unread().empty()) {
                const auto chunk = stream.unread().front();
                if (!co_await stream.send(chunk)) {
                    co_return;
                }
                stream.consume(chunk.size());
            }
        }
        co_await stream.close();
    }

    aloe::runtime::task<void> serve_task(Stack::StreamsType* streams, aloe::runtime::Scheduler scheduler, std::atomic<int>* served) {
        for (;;) {
            auto stream = co_await streams->accept(echo_port);
            if (!stream) {
                break;
            }
            served->fetch_add(1);
            scheduler.spawn(echo_task(std::move(*stream)));
        }
    }

    aloe::runtime::task<void> client_task(Stack::StreamsType* streams,
                                          const std::span<const std::byte> to_send,
                                          std::vector<std::byte>* received,
                                          std::atomic<int>* outcome) {
        auto stream = co_await streams->connect({.address = kernel_ip, .port = echo_port});
        if (!stream) {
            outcome->store(-1);
            co_return;
        }
        if (!co_await stream->send(to_send)) {
            outcome->store(-2);
            co_return;
        }
        while (received->size() < to_send.size()) {
            const auto readable = co_await stream->readable(1);
            if (!readable) {
                outcome->store(-3);
                co_return;
            }
            for (const std::span<const std::byte> chunk : stream->unread()) {
                received->insert(received->end(), chunk.begin(), chunk.end());
            }
            stream->consume(stream->unread().size());
        }
        const auto closed = co_await stream->close();
        outcome->store(closed ? 1 : -4);
    }

    [[nodiscard]] std::vector<std::byte> message(const std::size_t size) {
        std::vector<std::byte> out(size);
        for (std::size_t index = 0; index < size; ++index) {
            out[index] = static_cast<std::byte>(index * 7);
        }
        return out;
    }

    /// A kernel client: connects, sends, expects the echo, closes, expects EOF.
    void kernel_client_round_trip(const std::span<const std::byte> payload) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(fd, 0);
        const sockaddr_in server = address_of(stack_ip, echo_port);
        ASSERT_EQ(::connect(fd, reinterpret_cast<const sockaddr*>(&server), sizeof(server)), 0) << std::strerror(errno);
        ASSERT_TRUE(send_all(fd, payload));
        std::vector<std::byte> back(payload.size());
        EXPECT_TRUE(recv_all(fd, back)) << "the echo";
        EXPECT_EQ(back, std::vector<std::byte>(payload.begin(), payload.end()));
        ASSERT_EQ(shutdown(fd, SHUT_WR), 0);
        EXPECT_TRUE(recv_eof(fd)) << "the brick's FIN after ours";
        close(fd);
    }

    /// A kernel server: accepts one connection, echoes until EOF, then closes.
    void kernel_server_echo(const int listener, std::size_t expected) {
        const int fd = accept(listener, nullptr, nullptr);
        ASSERT_GE(fd, 0) << std::strerror(errno);
        std::vector<std::byte> buffer(4096);
        while (expected > 0) {
            const std::size_t want = std::min(expected, buffer.size());
            ASSERT_TRUE(recv_all(fd, std::span<std::byte>{buffer}.first(want)));
            ASSERT_TRUE(send_all(fd, std::span<const std::byte>{buffer}.first(want)));
            expected -= want;
        }
        EXPECT_TRUE(recv_eof(fd)) << "the brick's FIN";
        close(fd);
    }

    [[nodiscard]] int kernel_listener() {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        const int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        const sockaddr_in local = address_of(kernel_ip, echo_port);
        if (bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0 || listen(fd, 1) != 0) {
            close(fd);
            return -1;
        }
        return fd;
    }

    class TcpTap : public ::testing::Test {
    protected:
        void SetUp() override {
            if (geteuid() != 0) {
                GTEST_SKIP() << "needs root for the tap device";
            }
            port_.emplace(aloe::ethdev::PortConfig{.name = aloe::testing::probe_vdev("net_tap", "iface=aloe-tcp"), .queues = 1});
            configure_interface(interface, kernel_ip, netmask);
        }

        std::optional<Port> port_;
    };

}  // namespace

TEST_F(TcpTap, BrickServer) {
    const auto payload = message(5000);  // larger than the MSS, combined with FIN by the kernel at the end
    BrickLoop loop{*port_, true, {}};
    kernel_client_round_trip(payload);
    loop.stop();
    EXPECT_EQ(loop.counters().connections_accepted, 1U);
    EXPECT_EQ(loop.counters().connections_closed, 1U) << "both FINs acknowledged";
    EXPECT_EQ(loop.counters().dropped_bad_checksum, 0U);
    EXPECT_EQ(loop.counters().dropped_bad_header, 0U) << "the kernel's options were skipped";
    EXPECT_TRUE(loop.clean_close());
}

TEST_F(TcpTap, BrickClient) {
    const auto payload = message(3000);
    const int listener = kernel_listener();
    ASSERT_GE(listener, 0);
    BrickLoop loop{*port_, false, payload};
    kernel_server_echo(listener, payload.size());
    EXPECT_TRUE([&] {
        const auto deadline = std::chrono::steady_clock::now() + patience;
        while (std::chrono::steady_clock::now() < deadline && !loop.clean_close()) {
            std::this_thread::sleep_for(1ms);
        }
        return loop.clean_close();
    }());
    loop.stop();
    close(listener);
    EXPECT_EQ(loop.received(), payload);
    EXPECT_EQ(loop.counters().connections_opened, 1U);
    EXPECT_EQ(loop.counters().connections_closed, 1U);
}

TEST_F(TcpTap, RuntimeServer) {
    const auto payload = message(5000);
    std::atomic<int> served{0};
    Runtime runtime{{.shard = {}, .threads = {}, .thread_hook = aloe::ethdev::register_thread},
                    *port_,
                    aloe::net::Ipv4Config{.address = stack_ip, .prefix = 24},
                    aloe::tcp::TcpConfig{}};
    runtime.start();
    runtime.spawn(0, serve_task(&runtime.shard(0).stack().streams(), runtime.scheduler(0), &served));
    kernel_client_round_trip(payload);
    runtime.stop();
    runtime.join();
    EXPECT_EQ(served.load(), 1);
    const auto& counters = runtime.shard(0).stack().tcp().counters();
    EXPECT_EQ(counters.connections_closed, 1U);
    EXPECT_EQ(counters.dropped_bad_header, 0U);
}

TEST_F(TcpTap, RuntimeClient) {
    const auto payload = message(3000);
    const int listener = kernel_listener();
    ASSERT_GE(listener, 0);
    std::vector<std::byte> received;
    std::atomic<int> outcome{0};
    Runtime runtime{{.shard = {}, .threads = {}, .thread_hook = aloe::ethdev::register_thread},
                    *port_,
                    aloe::net::Ipv4Config{.address = stack_ip, .prefix = 24},
                    aloe::tcp::TcpConfig{}};
    runtime.start();
    runtime.spawn(0, client_task(&runtime.shard(0).stack().streams(), payload, &received, &outcome));
    kernel_server_echo(listener, payload.size());
    const auto deadline = std::chrono::steady_clock::now() + patience;
    while (std::chrono::steady_clock::now() < deadline && outcome.load() == 0) {
        std::this_thread::sleep_for(1ms);
    }
    runtime.stop();
    runtime.join();
    close(listener);
    EXPECT_EQ(outcome.load(), 1);
    EXPECT_EQ(received, payload);
    EXPECT_EQ(runtime.shard(0).stack().tcp().counters().connections_closed, 1U);
}
```

- [ ] **Step 4: Build the manual suite and run what the machine allows**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Manual.Tcp.Tap`
Expected: it compiles without privileges. In an environment with root, run `sudo ./build/debug/tests/manual_tests/tcp/Aloe.Tests.Manual.Tcp.Tap` by hand, outside the presets, and record the outcome of the four cases. Without root, record "compiled, not executed" and leave the spec's manual interoperability gate outstanding; do not claim it passed.

- [ ] **Step 5: Document and format**

In `docs/guides/getting-started.md`, next to the net tap test: the TCP tap command, the addresses (kernel 10.78.0.1/24, Aloe 10.78.0.2/24, interface `aloe-tcp`), the prerequisites (root, the tap driver) and that the suite is excluded from every preset. Run `./scripts/check-format.sh`.

- [ ] **Step 6: Commit**

```bash
git add tests/integration_tests tests/manual_tests docs/guides/getting-started.md
git commit -m "test(tcp): exercise real mbufs and Linux interoperability"
```

## Task 14: Living examples, final documentation and completion gates

**Files:**
- Create: `examples/tcp_echo/CMakeLists.txt`, `examples/tcp_echo/tcp_echo.cpp`, `examples/tcp_echo_tasks/CMakeLists.txt`, `examples/tcp_echo_tasks/tcp_echo_tasks.cpp`
- Modify: `examples/CMakeLists.txt`, `README.md`, `AGENTS.md`, `codestyle.md`, `docs/roadmap.md`, `docs/architecture/overview.md`, `tcp.md`, `stream.md`, `wire.md`, `net.md`, `fixtures.md`, `runtime.md`, `loop.md`, `docs/guides/getting-started.md`, `docs/superpowers/specs/2026-10-07-minimal-tcp-design.md`

**Interfaces:**
- Produces: `Aloe.Examples.TcpEcho`, the hand-written loop on one DPDK queue, `tcp_echo <port> <address>/<prefix> [<gateway>] (--listen <port> | --connect <host>:<port>) [-- <EAL arguments...>]`; `Aloe.Examples.TcpEchoTasks`, the same roles as tasks over `Runtime<ethdev::Port, runtime::TcpStack<ethdev::Port>>` on every queue of the port, one task per accepted connection. Both parse and validate on the cold path and print counters after stopping.

- [ ] **Step 1: Write the brick echo**

`examples/tcp_echo/CMakeLists.txt`:

```cmake
##############################################################################
# TCP echo on one DPDK queue by a hand-written loop over the bricks: the tcp module's living example
##############################################################################
add_executable(${EXAMPLES}.TcpEcho
        tcp_echo.cpp
)
target_link_libraries(${EXAMPLES}.TcpEcho
        PRIVATE
        Aloe::Common::Tcp
        Aloe::Common::Net
        Aloe::Common::Loop
        Aloe::Common::Ethdev
)
##############################################################################
```

`examples/tcp_echo/tcp_echo.cpp`:

```cpp
// A TCP echo server or client on one queue of a DPDK port, as a hand-written loop over the bricks: no runtime,
// no coroutine, no sender on the data path. The loop docs/architecture/tcp.md leads with.
//
//     tcp_echo <port> <address>/<prefix> [<gateway>] (--listen <port> | --connect <host>:<port>) [-- <EAL arguments...>]
//
//     sudo tcp_echo net_tap0 10.78.0.2/24 --listen 7 -- --vdev=net_tap0,iface=aloe0
//         then: ip addr add 10.78.0.1/24 dev aloe0 && ip link set aloe0 up && nc 10.78.0.2 7
//     sudo tcp_echo net_tap0 10.78.0.2/24 --connect 10.78.0.1:7 -- --vdev=net_tap0,iface=aloe0
//         with `nc -l 10.78.0.1 7` on the kernel side: the client sends a line, prints the echo, closes.
//
// The echo's copy from the received packet into the prepared one is the application's operation: a program
// producing new messages encodes them straight into `prepare` and never copies.

#include <aloe/ethdev>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/stream>
#include <aloe/tcp>
#include <aloe/wire>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

namespace {

    using Port = aloe::ethdev::Port;
    using Ipv4 = aloe::net::Ipv4<Port>;
    using Tcp  = aloe::tcp::Stack<Ipv4>;

    std::atomic<bool> interrupted{false};
    static_assert(std::atomic<bool>::is_always_lock_free, "a signal handler may only touch a lock-free atomic");

    void on_signal(int /*signal*/) {
        interrupted.store(true);
    }

    struct Arguments {
        std::string port;
        aloe::net::Ipv4Config ip;
        std::optional<std::uint16_t> listen;
        std::optional<aloe::tcp::Endpoint> connect;
        std::vector<std::string> eal;
    };

    [[nodiscard]] std::optional<std::uint16_t> parse_port(const std::string_view text) {
        unsigned value = 0;
        if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{} || value == 0 || value > 65535) {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(value);
    }

    [[nodiscard]] std::optional<Arguments> parse(const std::span<char*> argv) {
        if (argv.size() < 4) {
            return std::nullopt;
        }
        Arguments arguments;
        arguments.port = argv[1];
        const std::string_view cidr{argv[2]};
        const std::size_t slash = cidr.find('/');
        if (slash == std::string_view::npos) {
            return std::nullopt;
        }
        const auto address = aloe::wire::Ipv4Address::parse(cidr.substr(0, slash));
        unsigned prefix    = 0;
        const auto length  = cidr.substr(slash + 1);
        if (!address || std::from_chars(length.data(), length.data() + length.size(), prefix).ec != std::errc{} || prefix > 32) {
            return std::nullopt;
        }
        arguments.ip.address = *address;
        arguments.ip.prefix  = static_cast<std::uint8_t>(prefix);
        std::size_t next     = 3;
        if (std::string_view{argv[next]}.starts_with("--") == false) {
            const auto gateway = aloe::wire::Ipv4Address::parse(argv[next]);
            if (!gateway) {
                return std::nullopt;
            }
            arguments.ip.gateway = *gateway;
            ++next;
        }
        if (next + 1 >= argv.size()) {
            return std::nullopt;
        }
        const std::string_view role{argv[next]};
        const std::string_view target{argv[next + 1]};
        if (role == "--listen") {
            arguments.listen = parse_port(target);
            if (!arguments.listen) {
                return std::nullopt;
            }
        } else if (role == "--connect") {
            const std::size_t colon = target.rfind(':');
            if (colon == std::string_view::npos) {
                return std::nullopt;
            }
            const auto host = aloe::wire::Ipv4Address::parse(target.substr(0, colon));
            const auto port = parse_port(target.substr(colon + 1));
            if (!host || !port) {
                return std::nullopt;
            }
            arguments.connect = aloe::tcp::Endpoint{.address = *host, .port = *port};
        } else {
            return std::nullopt;
        }
        next += 2;
        arguments.eal.emplace_back("tcp_echo");
        if (next < argv.size()) {
            if (std::string_view{argv[next]} != "--") {
                return std::nullopt;
            }
            for (char* argument : argv.subspan(next + 1)) {
                arguments.eal.emplace_back(argument);
            }
        }
        if (arguments.eal.size() == 1) {
            arguments.eal = {"tcp_echo", "--in-memory", "--no-telemetry"};
        }
        return arguments;
    }

    /// The echo: what was read goes back out, in segments of what the connection can take; the rest waits for Writable.
    void echo(Tcp::ConnectionType& c) {
        while (!c.unread().empty()) {
            const std::span<const std::byte> chunk = c.unread().front();
            const auto out                         = c.prepare(chunk.size());
            if (!out) {
                return;
            }
            std::ranges::copy(chunk.first(out->size()), out->begin());
            if (!c.commit(out->size())) {
                return;
            }
            c.consume(out->size());
        }
    }

    constexpr std::string_view hello = "hello from aloe\n";

}  // namespace

int main(int argc, char** argv) {
    const auto arguments = parse(std::span<char*>{argv, static_cast<std::size_t>(argc)});
    if (!arguments) {
        std::println(stderr,
                     "usage: tcp_echo <port> <address>/<prefix> [<gateway>] (--listen <port> | --connect <host>:<port>) "
                     "[-- <EAL arguments...>]");
        return EXIT_FAILURE;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    aloe::ethdev::Eal eal{arguments->eal};
    Port port{{.name = arguments->port, .queues = 1}};
    aloe::loop::ShardCounters counters;
    aloe::loop::ShardQueue<Port> queue{port, 0, 512, counters};
    Ipv4 ip{queue, arguments->ip};
    aloe::loop::TimerWheel wheel{std::chrono::milliseconds{1}, std::chrono::steady_clock::now()};
    Tcp tcp{ip, wheel, {}};
    std::vector<Port::Packet> burst(64);
    std::size_t offered = 0;
    const std::span<const std::byte> greeting{reinterpret_cast<const std::byte*>(hello.data()), hello.size()};

    if (arguments->listen) {
        if (!tcp.listen(*arguments->listen)) {
            std::println(stderr, "cannot listen on {}", *arguments->listen);
            return EXIT_FAILURE;
        }
        std::println("{} ({}) {}: echoing on port {}; interrupt to stop", arguments->port, port.driver_name(), ip.address(), *arguments->listen);
    } else {
        const auto session = tcp.connect(*arguments->connect, std::chrono::steady_clock::now());
        if (!session) {
            std::println(stderr, "connect failed: {}", static_cast<int>(session.error()));
            return EXIT_FAILURE;
        }
        std::println("{} ({}) {}: connecting to {}:{}", arguments->port, port.driver_name(), ip.address(), arguments->connect->address, arguments->connect->port);
    }

    while (!interrupted.load()) {
        const auto now             = std::chrono::steady_clock::now();
        const std::size_t received = queue.receive(burst);
        ip.process(std::span<Port::Packet>{burst}.first(received), now);
        tcp.process(ip.received(aloe::wire::Ipv4Protocol::Tcp), now);
        std::ignore = wheel.advance(now);  // timer events join the receive events before the drain

        while (const auto event = tcp.poll_event()) {
            Tcp::ConnectionType& c           = *event->connection;
            const aloe::stream::Events events = event->events;
            if (events.connected() || (events.writable() && arguments->connect && offered < greeting.size())) {
                offered += c.send(greeting.subspan(offered));  // short sends resume on Writable
            }
            if (events.readable() || events.writable()) {
                if (arguments->listen) {
                    echo(c);
                } else {
                    for (const std::span<const std::byte> chunk : c.unread()) {
                        std::fwrite(chunk.data(), 1, chunk.size(), stdout);
                    }
                    c.consume(c.unread().size());
                    if (c.peer_closed() || offered == greeting.size()) {
                        c.close();
                    }
                }
            }
            if (events.peer_closed() && c.unread().empty() && c.state() == aloe::tcp::State::CloseWait) {
                c.close();
            }
            if (events.closed() || events.reset() || events.timed_out()) {
                c.release();
                if (arguments->connect) {
                    interrupted.store(true);  // the client's one session is over
                }
            }
        }

        tcp.flush(now);
        std::ignore = queue.flush();
    }
    queue.discard();

    const aloe::tcp::TcpCounters& c = tcp.counters();
    std::println("{} segments in, {} data out, {} pure ACKs, {} control, {} resets; {} accepted, {} opened, {} closed, "
                 "{} reset, {} timed out; dropped: {} no connection, {} checksum, {} window, {} resources",
                 c.segments_received, c.data_segments_sent, c.pure_acks_sent, c.control_segments_sent, c.resets_sent,
                 c.connections_accepted, c.connections_opened, c.connections_closed, c.connections_reset,
                 c.connections_timed_out, c.dropped_no_connection, c.dropped_bad_checksum,
                 c.dropped_out_of_window + c.dropped_out_of_order + c.dropped_duplicate,
                 c.dropped_no_slot + c.dropped_no_node + c.dropped_table_full);
    return EXIT_SUCCESS;
}
```

- [ ] **Step 2: Write the task echo**

`examples/tcp_echo_tasks/CMakeLists.txt` mirrors the brick one with target `${EXAMPLES}.TcpEchoTasks`, source `tcp_echo_tasks.cpp`, linking `Aloe::Common::Runtime`, `Aloe::Common::Tcp`, `Aloe::Common::Net`, `Aloe::Common::Ethdev`. Add both subdirectories to `examples/CMakeLists.txt`.

`examples/tcp_echo_tasks/tcp_echo_tasks.cpp`:

```cpp
// The same echo as tasks over the runtime and the ready-made TCP stack, on every queue of the port: one shard
// per queue, one task per connection. Compare with examples/tcp_echo for the loop it wraps.
//
//     tcp_echo_tasks <port> <address>/<prefix> [<gateway>] (--listen <port> | --connect <host>:<port>) [-- <EAL arguments...>]

#include <aloe/ethdev>
#include <aloe/net>
#include <aloe/runtime>
#include <aloe/stream>
#include <aloe/tcp>
#include <aloe/wire>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pthread.h>

namespace {

    using Port    = aloe::ethdev::Port;
    using Stack   = aloe::runtime::TcpStack<Port>;
    using Runtime = aloe::runtime::Runtime<Port, Stack>;

    // `Arguments`, `parse_port` and `parse` are the same as in examples/tcp_echo/tcp_echo.cpp, with "tcp_echo_tasks"
    // as the program name in the EAL arguments; copy them here.

    aloe::runtime::task<void> echo(Stack::StreamType stream) {
        for (;;) {
            const auto readable = co_await stream.readable(1);
            if (!readable) {
                break;
            }
            while (!stream.unread().empty()) {
                const auto chunk = stream.unread().front();
                if (!co_await stream.send(chunk)) {
                    co_return;
                }
                stream.consume(chunk.size());
            }
        }
        co_await stream.close();
    }

    aloe::runtime::task<void> serve(Stack::StreamsType* streams, aloe::runtime::Scheduler scheduler, const std::uint16_t port) {
        for (;;) {
            auto stream = co_await streams->accept(port);
            if (!stream) {
                break;
            }
            scheduler.spawn(echo(std::move(*stream)));
        }
    }

    aloe::runtime::task<void> client(Stack::StreamsType* streams, const aloe::tcp::Endpoint peer, std::atomic<bool>* done) {
        constexpr std::string_view hello = "hello from aloe\n";
        const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(hello.data()), hello.size()};
        auto stream = co_await streams->connect(peer);
        if (stream && co_await stream->send(bytes)) {
            std::size_t got = 0;
            while (got < bytes.size()) {
                const auto readable = co_await stream->readable(1);
                if (!readable) {
                    break;
                }
                for (const std::span<const std::byte> chunk : stream->unread()) {
                    std::fwrite(chunk.data(), 1, chunk.size(), stdout);
                    got += chunk.size();
                }
                stream->consume(stream->unread().size());
            }
            std::ignore = co_await stream->close();
        }
        done->store(true);
    }

    [[nodiscard]] sigset_t block_termination_signals() {
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &signals, nullptr);
        return signals;
    }

}  // namespace

int main(int argc, char** argv) {
    const auto arguments = parse(std::span<char*>{argv, static_cast<std::size_t>(argc)});
    if (!arguments) {
        std::println(stderr, "usage: tcp_echo_tasks <port> <address>/<prefix> [<gateway>] (--listen <port> | --connect <host>:<port>) [-- <EAL arguments...>]");
        return EXIT_FAILURE;
    }
    const sigset_t signals = block_termination_signals();  // before any thread exists, so every shard inherits the mask
    aloe::ethdev::Eal eal{arguments->eal};
    Port port{{.name = arguments->port}};
    Runtime runtime{{.shard = {}, .threads = {}, .thread_hook = aloe::ethdev::register_thread}, port, arguments->ip, aloe::tcp::TcpConfig{}};
    runtime.start();
    std::atomic<bool> done{false};
    if (arguments->listen) {
        for (std::uint16_t index = 0; index < runtime.shard_count(); ++index) {
            runtime.spawn(index, serve(&runtime.shard(index).stack().streams(), runtime.scheduler(index), *arguments->listen));
        }
        std::println("{} queues echoing on port {}; interrupt to stop", runtime.shard_count(), *arguments->listen);
        int signal = 0;
        sigwait(&signals, &signal);
    } else {
        runtime.spawn(0, client(&runtime.shard(0).stack().streams(), *arguments->connect, &done));
        while (!done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    runtime.stop();
    runtime.join();
    for (std::uint16_t index = 0; index < runtime.shard_count(); ++index) {
        const auto& c = runtime.shard(index).stack().tcp().counters();
        std::println("shard {}: {} accepted, {} opened, {} closed, {} data segments, {} pure ACKs, {} dropped without a connection",
                     index, c.connections_accepted, c.connections_opened, c.connections_closed, c.data_segments_sent,
                     c.pure_acks_sent, c.dropped_no_connection);
    }
    return EXIT_SUCCESS;
}
```

`<thread>` and `<chrono>` are needed for the client's wait.

- [ ] **Step 3: Build the examples**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Examples.TcpEcho Aloe.Examples.TcpEchoTasks`
Expected: both link. Where a machine with a tap device and root is available, run brick server and client and task server and client against `nc` with lines shorter and longer than the MSS, and record the exact commands and outcomes in the handoff; otherwise record "built, not run".

- [ ] **Step 4: Reconcile the documentation**

- `docs/architecture/tcp.md`: complete the page begun in Task 9 with the task wrapper second, the counters table, the config table, and the example commands.
- `docs/architecture/stream.md`: final pass against the spec's contract section; the senders table from Task 11.
- `docs/architecture/overview.md`: the loop sketch gets the real names (`poll_event`, `c.unread()`, `prepare`/`commit`, `wheel.advance` before the drain) and the explicit event drain; the status lines name TCP and the senders as existing; "zero-copy view" and "deadlines" read as decided; preserve the ownership-boundaries paragraph already edited.
- `docs/architecture/loop.md`: the TCP loop now exists (a sentence and a link). `net.md`, `fixtures.md`, `wire.md`, `runtime.md`: final pass of the rows added in Tasks 1 to 12.
- `docs/roadmap.md`: phase 1 marks TCP landed with UDP and the benchmark remaining; the error-reporting question resolved (`std::expected` in the value channel) and the abseil question resolved (none); the "designed as a receive half and a transmit half" wording replaced by the ownership boundaries.
- `README.md`: rows for `aloe::stream` (`common/stream/`) and `aloe::tcp` (`common/tcp/`) with their docs links; the quick example may stay the Ethernet one.
- `AGENTS.md`: the layout row gains `stream` and `tcp`; the namespaces list gains `aloe::stream` and `aloe::tcp`; add the traps found in this plan's "Read This First" that are new (`modernize-make-unique` and the passkey, template members defined where first instantiated, one-slot ring for refusals, `Wrong` corrupts only the TCP checksum in `tcp_frame`).
- `codestyle.md`: both `aloe::net::tcp` examples become `aloe::tcp` (lines 49 and 583).
- `docs/guides/getting-started.md`: the examples' commands beside the tap test from Task 13.
- The spec: carry the concrete spellings of the "Concrete resolutions" section into its sketches (the template types, `DeviceT`, `queue()` and `next_hop`, the retry counting, the ring peer at another address, the terminal ACK before `Closed`, the passkey storage) so plan and spec hold no conflicting sketches; mark the "Open questions" that this plan resolved.

Document, in `tcp.md` and the roadmap: no data retransmission, no TIME_WAIT, no out-of-order storage, no deployment or latency claim.

- [ ] **Step 5: Run the full matrix**

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
cmake --preset gcc-debug
cmake --build --preset gcc-debug
ctest --preset gcc-debug --output-on-failure
cmake --preset asan
cmake --build --preset asan
ctest --preset asan --output-on-failure
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan --output-on-failure
./scripts/check-format.sh
git diff --check
```

Expected: every build exits zero, every non-manual CTest passes, sanitizer output is clean, the format and whitespace checks pass. Run sequentially if memory is limited. Confirm the manual and example binaries build; list actual manual runs separately. Then check the boundaries:

```bash
grep -rn 'aloe/execution\|aloe/log\|stdexec\|exec::\|quill' common/tcp common/stream common/wire common/net fixtures   # nothing
grep -rn 'Aloe::Dpdk\|rte_' common --include=CMakeLists.txt --include='*.hpp' --include='*.cpp' | grep -v '^common/ethdev'   # nothing
grep -rn 'using namespace' common fixtures                                                                              # nothing
```

Do not weaken a sanitizer or warning gate to pass it.

- [ ] **Step 6: Review the whole branch against the spec**

Use `superpowers:requesting-code-review` with the spec's "Done when" list and this plan's Review Focus. Beyond those, the reviewer reads for: table and sequence wrap, pool exhaustion, event and operation lifetime, the last ACK before release, zero-window ACK acceptability, the three checksum verdicts, shutdown with control posts in flight, and source/destination orientation in every hash. Fix confirmed findings and rerun the affected presets. The loss-free echo passing is not approval on its own.

- [ ] **Step 7: Commit**

```bash
./scripts/check-format.sh
git add examples README.md AGENTS.md codestyle.md docs
git commit -m "docs(tcp): add brick and task echoes with usage guides"
```

Integration, push and the pull request are the development-branch handoff (`superpowers:finishing-a-development-branch`), not implied here. The PR title is `[WIRE][STREAM][TCP][NET][RUNTIME][FRAMES] Minimal TCP: the brick, the stream contract and the senders`.

## Coverage map

| Spec requirement | Owning tasks and evidence |
|---|---|
| TCP wire parsing, options, sequence arithmetic | 1, 3: fixed byte vectors, malformed options, wrap, the computed checksums |
| Stream concept, zero-copy views, convenient send | 2, 5, 7, 8: stub behaviour, concept pins, span identity into packets, consume across boundaries |
| Config validation, fixed table and pool, stable indices | 5, 6: overflow, full, collision, reuse, destruction with nothing armed |
| Handshake in both directions, control retransmission | 6: scripted peer, the retry schedule, the unresolved poll, unowned timeout |
| In-order data and a byte window independent of packet caps | 7: one-byte credit, fixed edge, wrap, zero window, slot and pool caps |
| Transmit in place, backpressure, ACK policy | 8: pointer-free in-place proof, refusal preserving numbers, one hint, the four ACK exceptions |
| FIN, RST, half-close, release, terminal ACK before Closed | 9: both close orders, simultaneous, combined segments, duplicate FIN, refusal of the final ACK |
| RSS placement and the table's separate hash | 5, 9: hardware and software agreement, conditioned hashes, four queues both ways, Unplaceable |
| Explicit event lifetime | 6, 11, 12: survival across processing and timers, queued completion cancellation, slot reuse |
| Runtime tick, siblings, guarded control posts | 10: hook order and stamps, accepted-or-rejected ownership, failed-hook closure |
| Generic senders, deadlines, scope shutdown | 11: every value and error, two drains queue once, release and cancel of queued work, deadline |
| The ready-made stack and ARP forwarding | 12: same-tick piggyback, zero-window reopening, timer completion timing, forwarded once, steering under TSan, stop races |
| Real mbufs without root | 13: scripted peer over fresh ring ports |
| Linux interoperability, brick and tasks, both directions | 13, 14: the four manual scenarios, the two examples |
| Public docs and every local gate | 14: pages reconciled, the full matrix, the boundary greps |
| Deferred scope stays deferred | Global constraints, `tcp.md`, the roadmap: no data retransmission, out-of-order, TIME_WAIT, two-core connection |

## Plan handoff

Fourteen dependent, testable tasks. Review the "Concrete resolutions" and the lifetime rules at the head of Task 11 before execution; the code in this plan was not compiled except for the two probes in "Verified Beforehand", so the executor runs each task's red and green steps rather than trusting the listings. None of the test outcomes above is a reported result.

Execution method: **subagent-driven** is recommended, because the TCP brick (Tasks 6 to 9) and the runtime adapter (Tasks 10 to 12) each carry tightly coupled lifetime contracts that benefit from a fresh reviewer at every task boundary, and a shipped mistake in the event or slot lifetime surfaces only under load. **Native** execution keeps the whole brick in one implementer's context and costs less; with it, run the Task 9 brick gate and the Task 12 sanitizer gate before moving on, and the whole-branch review at the end. Do not start implementation until the plan has been reviewed and a method chosen.
