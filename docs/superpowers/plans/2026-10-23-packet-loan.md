# Packet Loan, Scripted Impairment and Scenario Runner Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build roadmap phase 2, step 2 (GitHub issue #17): the packet loan on both device backends with the conformance suite covering it, scripted impairment and a clock step in the fabric, a new fixture module `scenario` that runs packetdrill-style scripts against the TCP brick on the fabric, and a first set of fifteen scripts that pass against today's TCP, with the docs brought current.

**Architecture:** The loan is spelled on the packet: `lend()` yields a second handle for the device and `lent()` tells the lender whether that handle is still alive. `ethdev::Packet` backs it with the mbuf's reference count, and `ethdev::Port::receive` copies any mbuf that arrives shared so the ring driver's loopback can never move a lender's offsets; `fabric::Packet` backs it with marks on its block, and `fabric::Port` holds transmitted handles in an in-flight list released on the queue's thread under a `LoanReturn` policy. The device concept gains `reclaim(queue)` and `Capabilities::tx_reclaim`. The fabric gains `Fabric::advance(now)` as its clock step, per-port egress `Impairment` (a seeded model plus a scripted verdict) and `ImpairmentCounters`. The `scenario` module parses the script grammar into a `Script`, keeps the peer's sequence bookkeeping in `Peer`, builds the bricks over a fabric in `Harness`, and `Runner` steps a virtual clock one wheel tick at a time, matching the stack's segments in packetdrill's queue order. TCP does not change.

**Tech Stack:** C++26 on libstdc++, clang 22 and GCC 16, CMake and Ninja, GoogleTest (with a hand-written `main` for the scripts binary, using `::testing::RegisterTest`), the existing vcpkg dependencies, DPDK 26.03 through `Aloe::Dpdk` inside `ethdev` only. No new dependency.

**Spec:** [Packet loan, scripted impairment and a scenario-script runner](../specs/2026-10-22-packet-loan-design.md). Read the spec, `AGENTS.md` and `codestyle.md` before implementation. Both documents live in the git-ignored `docs/superpowers/`; nothing tracked may link to them.

## Global Constraints

- "TCP's behaviour does not change in this step": no file under `common/tcp/` or `common/net/` changes. `commit` still hands the packet to IP and forgets it.
- "The hot paths of `ethdev::Port` and `ethdev::Packet` gain at most a load and a branch per packet." The receive copy runs only when `rte_mbuf_refcnt_read(mbuf) > 1`.
- "A packet on loan is readable, never written": `prepend`, `append`, `trim_front`, `trim_back` and `set_tx` assert `!lent()` on both backends, on the lender and on the handle alike. A device may complete the checksums it was asked to fill inside the lent buffer; the size, the offsets and every other byte stay as lent.
- "One borrowed handle at a time": `lend()` asserts the packet is not empty and not lent.
- "Dropping the lender while lent is allowed: the buffer is freed when the device returns it, exactly once."
- "`reclaim(queue)` runs on the queue's thread, never throws, and returns how many packets the driver let go of. On a device whose `tx_reclaim` is false it returns zero and does nothing."
- Fabric loans: "`OnStep` ... `Fabric::advance(now)` marks every loan transmitted before the call as finished, and a finished handle is released by the queue's next `transmit`, `receive` or `reclaim`"; "`reclaim(queue)` is the eager path: on an `OnStep` port it releases every in-flight handle of its queue"; "`Held`: nothing finishes on its own and `reclaim` returns zero; `Port::return_loans(queue)` ... releases every handle of that queue." The pool and the block marks stay unsynchronised; `advance` touches only the finished counts under the queue's lock.
- Impairment runs "the script first and the random model second, per frame, in a fixed order"; the model draws from `std::mt19937_64` as a 53-bit fraction of the raw output, never through `std::uniform_real_distribution`. A drop still counts as `transmitted` on the source. "A delay is measured from the fabric's current stamp, which is the stamp of the last `advance` (the epoch before the first)."
- The runner: inbound lines and calls act at their nominal time with no tolerance; outbound lines take the head of the queue of sent segments and check its stamp against the window widened by the tolerance on both sides; `accept`, `read` and `recv` with a window are retried once per step; every other call runs once at the window's start. The default tolerance is one wheel resolution (1 ms); `*` is bounded by `RunnerConfig::any_bound` (120 s).
- Matching: flags exactly; relative sequence and length; `ack`, `win` and `urg` when written; options byte for byte when written, anything for `<...>`, none when nothing is written; ports and addresses never. Every outbound segment's IPv4 and TCP checksums are verified in both fabric checksum modes.
- "A call it does not bind fails the script with its name and line, never silently." `close` always maps to `close()`.
- Scripts are Aloe's own; nothing is copied from packetdrill's corpus or the kernel tree. Scripts live under `tests/scenarios/tcp/<area>/<name>.pkt`.
- The new module: `fixtures/scenario/`, target `Aloe::Fixtures::Scenario`, umbrella `#include <aloe/scenario>`, namespace `aloe::scenario`, one class per file, no gtest inside it.
- `AGENTS.md` rules apply unchanged: DPDK names only inside `common/ethdev/` and the tests that already name them; nothing below the runtime names stdexec or quill; `core::TimePoint` and `core::Duration` for every stamp and interval; `std::expected` on hot paths, exceptions on setup paths; public headers define no macros (the one compile definition, `ALOE_SCENARIOS_DIR`, is set on the scripts test target and read in its `main` only); another module's names qualified with its namespace; test helpers in `aloe::testing`.
- `codestyle.md` applies: by-value parameters `const` in definitions and never in bodiless declarations; `[[nodiscard]]` on every value-returning function; a default member initialiser on every struct member (clang's `-Wmissing-designated-field-initializers` is an error); no integer literal suffixes beyond `U` on shift counts; one class in one place; include order and the `detail` namespace for private helpers.
- Completion gates: `debug`, `gcc-debug`, `asan` and `tsan` build and pass; `./scripts/check-format.sh` passes; clang-tidy, which runs inside the clang builds with warnings as errors, is clean; `-DDO_BENCHMARKS=ON` builds and its tests pass; the tap manual test's loan case is run once as root.
- Conventional commit subjects with the module as scope: `feat(device)`, `feat(ethdev)`, `feat(fabric)`, `feat(loop)`, `feat(frames)`, `feat(scenario)`, `test(<module>)`, `docs:`.

## Verified Beforehand

Nothing in this plan was built; the author asked for plans without a prototype. These facts were read from the code and the DPDK 26.03 sources under `vcpkg/buildtrees/dpdk/src/v26.03-*/` on 2026-10-22 and 2026-10-23, some by the builder-check reviewer of the spec.

| Fact | Where it matters |
|---|---|
| `rte_pktmbuf_prefree_seg` (`lib/mbuf/rte_mbuf.h`) returns the mbuf for freeing only when its count is one; with a higher count it decrements and returns `NULL`, so `rte_pktmbuf_free` on a shared mbuf frees nothing. `rte_mbuf_refcnt_read(const rte_mbuf*)` and `rte_mbuf_refcnt_update(rte_mbuf*, int16_t)` are the accessors; a single-segment mbuf needs no `rte_pktmbuf_refcnt_update`. | Task 1 |
| `rte_eth_tx_done_cleanup(port, queue, free_cnt)` returns `-ENOTSUP` when `dev->dev_ops->tx_done_cleanup == NULL` (`lib/ethdev/rte_ethdev.c`), otherwise the count freed, `0` when nothing is pending. `net_ring`, `net_null` and `net_tap` do not implement it; the Intel drivers (e1000, igb, igc, i40e, iavf, ice, ixgbe), hns3, cnxk, ena, netvsc, ngbe, txgbe, r8169, hinic3 and vhost do; mlx5 does not. | Task 2 |
| `eth_null_tx` frees the burst inside the call (`rte_pktmbuf_free_bulk`); `pmd_tx_burst` of tap frees each mbuf after writing it; `eth_ring_tx` enqueues the mbuf pointer (`rte_ring_enqueue_burst`) and never frees: the receiver does. So on CI a loan is back at transmit (null, tap) or when the receiver lets go (ring). | Tasks 2, 5 |
| `rte_pktmbuf_copy(const rte_mbuf*, rte_mempool*, uint32_t offset, uint32_t length)` allocates from the pool (default headroom `RTE_PKTMBUF_HEADROOM` = 128 = `device::packet_headroom`), copies `length` bytes (`UINT32_MAX` for all) and returns `NULL` when the pool is empty. `rte_eth_dev_conf_get(port_id, rte_eth_conf*)` reads the configuration back; `RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE` is `RTE_BIT64(16)`. | Task 2 |
| `port.cpp` masks the driver's transmit offloads with `tx_checksum_offloads` (IPv4, UDP, TCP checksum bits only). `net_ring` advertises `RTE_ETH_TX_OFFLOAD_MULTI_SEGS`, `net_null` `MULTI_SEGS | MT_LOCKFREE`; neither offers fast-free, so the invariant cannot be exercised at run time on CI. | Task 2 |
| `ethdev::Port::receive` today: `rte_eth_rx_burst` into a pointer array, then per mbuf a check `nb_segs != 1 || pkt_len > limit || pkt_len < 14` → free and count `oversized`; it clears `RTE_MBUF_F_TX_OFFLOAD_MASK` and `tx_offload` on the kept mbuf. `counters_[queue].dropped` is never incremented. | Task 2 |
| `fabric::Packet` holds `pool_`, `block_`, `begin_`, `end_`, `rx_`, `tx_`; `release()` gives the block back when `block_ != nullptr`; `Pool::~Pool` asserts `free_.size() == blocks_.size()`. `fabric::Port::Queue` is `{mutex, pending, pool, counters}`; members are destroyed in reverse order of declaration, so a member holding packets must be declared after `pool`. `Port::transmit` calls `fill_checksums(packet)` into the packet's own block before `fabric_.deliver(*this, frame)` copies the bytes to the destination queue, then `packet = Packet{}`. `Port::allocate` takes no lock; `receive`, `transmit`'s counter updates and `counters` take the queue mutex. | Tasks 3, 4, 6 |
| `Fabric::deliver(source, frame)` finds the destination by MAC (unicast to the matching port, the sender included; multicast to every other port) and calls `Port::deliver(frame)`, which takes the destination queue's mutex. The fabric has no mutex and no stamp today. | Tasks 4, 6 |
| `device::IsPacket` requires `empty, data, size, headroom, tailroom, prepend, append, trim_front, trim_back, rx, tx, set_tx`; `IsDevice` requires `Packet`, `queue_count, mac, mtu, link_up, capabilities, steering, allocate, receive, transmit, counters`. Types modelling them: `fabric::Port`, `ethdev::Port`, `testing::TcpTestDevice` (`tests/shared/tcp/tcp_test_device.hpp`), `benchmarks::StampedDevice` (`benchmarks/tick_to_trade/stamped_device.hpp`), `FakeDevice` and `FakePacket` in `tests/unit_tests/common/device/test_concepts.cpp`, `Throttled` in `tests/unit_tests/common/runtime/test_shard.cpp`, `StuckPort` in `tests/unit_tests/common/net/test_net_send.cpp`. `Capabilities` is an aggregate with defaults on every field. | Tasks 2, 5 |
| The conformance suite (`tests/shared/device/device_conformance.hpp`) is a `TYPED_TEST_SUITE_P` over a fixture with `device()`; instantiated by `tests/unit_tests/fixtures/fabric/test_fabric_conformance.cpp` (a fabric port, `EmulatedOffloads::None`) and `tests/integration_tests/ethdev/test_ethdev_ring.cpp` (`RingFixture`, `pool_size` 512). Its helpers: `frame(payload, seed)` (experimental ethertype, to the device's own MAC), `packet_with(bytes)`, `receive_up_to(count, polls)`. `ThePoolRunsOutAndRecovers` counts a pool by allocating until `allocate` fails. | Task 5 |
| `loop::ShardQueue<Device>` has `allocate`, `receive`, `transmit(Packet&&)` (moves only on success), `flush`, `discard`, `pending`, `capacity`, `index`, `mac`, `mtu`, `capabilities`, `steering`, `queue_count`, `device()`; it holds `Device* device_` and `std::uint16_t queue_`. Tests of the loop link only `Aloe::Common::Loop`. | Task 5 |
| `tests/shared/tcp/tcp_fixture.hpp`: `detail::FabricPair{fabric, harness, port}`; `TcpHarness<Device>(fabric::Port& harness, Device& device, const TcpConfig&, const Ipv4Config&, std::size_t ring_capacity)` with `advance(by)` = `now_ += by; tcp_->process({}, now_); wheel_.advance(now_)`. `TcpFixture` constructs `TcpHarness{harness, port}`; `TcpRefusingFixture` constructs `TcpHarness{harness, device, {}, stack_config(), 1}`. The six TCP unit test files use the fixtures and never construct `TcpHarness` themselves. | Task 7 |
| `tests/unit_tests/common/tcp/test_tcp_two_stacks.cpp`: `Brick{counters, queue, ip, wheel, tcp, burst}` with `receive(now)` and `flush(now)`; the fixture `TcpTwoStacks` owns `fabric_`, `server_port_`, `client_port_` (`pool_size` 256), `server_`, `client_`, `now_`; `establish(client)` ticks ten times `now_ += 1ms`; `exchange()` ticks twice; `EchoAcrossMssAndClose` has its own loop. `Tcp::ConnectionType::send(bytes)` is the copying convenience. | Task 7 |
| `frames::TcpSpec{destination_mac, source_mac, source, destination, source_port, destination_port, sequence, acknowledgement, flags, window = 65535, std::optional<std::uint16_t> mss, checksums}`; `tcp_segment` writes the MSS option when `mss` is set; `tcp_frame` wraps it in IPv4 and Ethernet with correct checksums; `parse_frame` gives `ParsedFrame{ethernet, arp, ipv4, icmp, tcp, ipv4_header, l4}` where `l4` is the TCP segment and `tcp->data_offset` the header length; `l4_checksum_residue(ipv4, l4)` is zero for a right checksum; `wire::ipv4_header_checksum(header)` over `ipv4_header` is zero for a right one. `wire::TcpOptions::parse` reads `mss`; `write_mss` writes kind 2, length 4. | Tasks 8, 11, 12 |
| `wire::TcpFlags` is built from an initializer list of `wire::TcpFlag` (`Fin 0x01, Syn 0x02, Rst 0x04, Psh 0x08, Ack 0x10, Urg 0x20`), compares with `==`, `has(flag)`, `raw()`, `from_raw(byte)`. `wire::TcpSequence{value}` has `+ std::uint32_t`, `- TcpSequence` (signed distance), `before`, `after`, `==`. | Tasks 9, 11 |
| `tcp::Stack<Ip>`: `listen(port)` → `std::expected<void, ListenError>`; `unlisten(port)`; `listening(port)`; `connect(Endpoint{address, port}, now)` → `std::expected<ConnectionType*, ConnectError>`; `process(span<net::Datagram<Packet>>, now)`; `poll_event()` → `std::optional<EventType{connection, events}>`; `flush(now)`; `counters()`. `Connection`: `state()`, `unread()` (`size()` in bytes, `empty()`), `consume(n)`, `writable()`, `prepare(n)` → `std::optional<std::span<std::byte>>`, `commit(n)` → `bool`, `peer_closed()`, `close()`, `abort()`, `release()`, `mss()`. `stream::Events` has `connected, accepted, readable, writable, acked, peer_closed, closed, reset, timed_out, unresolved`. | Tasks 12, 13, 14 |
| Today's TCP on the wire, for the scripts: a SYN and SYN-ACK carry the one option `<mss 1460>` (data offset 24); a SYN's window is the byte budget 46720 (32 × 1460); every ACK-bearing segment offers `rcv_nxt + (46720 - unread)`; an ACK of in-order data waits for `flush`; a duplicate, an out-of-order segment and a FIN in FinWait2 are acknowledged at once; a FIN in Established queues the ACK; `consume` queues a window update when the edge grows and the state is Established, FinWait1 or FinWait2; the handshake ACK of an active open is queued and leaves at the flush; control retransmits fire 1, 3, 7, 15 and 31 s after the first send and `TimedOut` at 63 s; a segment with no connection and no listener, or a non-SYN to a listening port with no connection, is answered with RST (`R` at the segment's acknowledgement when it had ACK, else `R.` with seq 0 and ack past the segment); `reply_reset` never answers a RST; after `release` the slot is gone and later segments take that path; a RST in SynSent raises `Reset`; `set_closed` leaves the state `Closed` so `release` sends nothing. | Tasks 13, 14, 15 |
| `net::Ipv4Config` has `arp_reachable` (60 s) and `arp_expire` (120 s); `Ipv4::learn(address, mac, now)` seeds the cache. `loop::TimerWheel{resolution, start}` fires a deadline on the first `advance` whose stamp reaches its tick. `core::TimePoint{}` is the epoch and `core::Duration` is `std::chrono::nanoseconds`. | Tasks 12, 13 |
| Tests are registered one CTest entry per binary by `add_unit_test(target sources...)` → `add_test(NAME target COMMAND target)` with the label `unit`; `${TEST_LIBS}` is `GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main`. `tests/unit_tests/CMakeLists.txt` adds `common`, `fixtures` and, under `DO_BENCHMARKS`, `benchmarks`; `tests/unit_tests/fixtures/CMakeLists.txt` adds `fabric` and `frames`. `fixtures/CMakeLists.txt` sets `FIXTURES` and adds `fabric` and `frames`; a fixture's library is a STATIC target per directory plus `add_combined_library` for the umbrella. | Tasks 5, 6, 9, 15 |
| The repository has no death tests and no `NDEBUG`-guarded tests; asserts are contract checks only. `cert-msc51-cpp` fires on a fixed seed in a test; keep it with `// NOLINT(cert-msc51-cpp)` and the reason. | Tasks 3, 6 |
| packetdrill's grammar (`syntax.md` of `google/packetdrill`): event times `+t`, `t`, `t1~t2`, `+a~+b`, `*`; directions `<` and `>`; flags as letters with `.` for ACK; `first:last(length)`; `ack n`, `win n`, `urg n`; options `mss n`, `nop`, `eol`, `sackOK`, `wscale n`, `sack a:b,...`, `TS val v ecr e`, or `<...>`; call lines `name(args) = result [ERRNO (text)]`; `//` comments; backtick shell lines; `--name=value` option lines before the first event. packetdrill and both script corpora are GPL-2.0. | Tasks 9, 10, 15 |

## Read This First

Baseline commands, on a checkout of `main` at or after commit e084351:

```bash
git switch -c device/packet-loan
cmake --preset debug && cmake --build --preset debug && ctest --preset debug      # 55+ tests pass today
./scripts/check-format.sh
```

Code-base traps that bite this plan in particular; the full list is in `AGENTS.md`:

- A member of `fabric::Port::Queue` that holds `Packet`s must be declared after `pool`, or the pool is destroyed first and the packets give their blocks back to a dead pool.
- `fabric::Port::transmit` does `packet = Packet{}` on every accepted slot; a borrowed handle must be moved into the in-flight list before that line runs for it.
- The ring driver loops a queue's transmit back onto its own receive; a lent packet on it reaches `receive` as the very mbuf, which is why Task 2's copy exists. Test it on `net_ring`, never on `net_null`, which frees at transmit.
- `rte_pktmbuf_pool_create` keeps a per-lcore cache; count a pool by allocating until `allocate` fails, as the conformance suite does, never by `rte_mempool_avail_count` on a port's pool.
- `std::uniform_real_distribution` differs between libstdc++ and libc++; the impairment model turns the engine's raw 64 bits into a double itself.
- `cert-msc51-cpp` fires on a fixed seed in a test; `// NOLINT(cert-msc51-cpp): a fixed seed keeps the test reproducible`.
- clang-tidy's `readability-make-member-function-const` fires on a member that only writes through a pointer; call `core::force_non_const(this)` first in `ethdev::Port::reclaim`, as `allocate` does.
- clang's `-Wmissing-designated-field-initializers` is an error: every new struct member gets a default member initialiser.
- A `std::function` member makes a struct non-trivial; `Impairment` is fine with that, it is a fixture's configuration copied once by `set_impairment`.
- gtest test names registered with `::testing::RegisterTest` must not contain `.`; `/` is fine and is what parameterised names use.
- The scripts test binary defines its own `main`, so it links `GTest::gtest` and `GTest::gmock`, not `${TEST_LIBS}` (which carries `gtest_main`).
- `tcp::Stack` sends its SYN-ACK retransmits from the wheel; a test loop that advances the fabric and the wheel in 1 ms steps sees each retry at its deadline, a loop that jumps a whole second sees one.
- `TcpHarness` is used only through `TcpFixture` and `TcpRefusingFixture`; adding a constructor parameter touches those two constructors and nothing else.

## How to Execute and Review

Use superpowers:subagent-driven-development (preferred) or superpowers:executing-plans. One task at a time, in order; each ends with every listed test passing on `debug` and a commit.

Per task, a fresh reviewer checks:

1. **Spec conformance.** The task's interfaces match this plan's Interfaces blocks exactly; the behaviour matches the spec sections the task cites; nothing under `common/tcp/` or `common/net/` changed; TCP sends the same segments it sent before.
2. **Repository rules.** `AGENTS.md` Rules and `codestyle.md`: namespaces, qualified cross-module names, `const` by-value parameters in definitions only, `[[nodiscard]]`, default member initialisers, no literal suffixes, one class in one place, no macros in headers, no DPDK names outside `common/ethdev/` and the ethdev tests, no `stdexec::`, no `quill::`, nothing logging.
3. **The loan's contract.** No code writes a lent packet; the handle is transmitted or dropped; the lender's offsets never move; a dropped lender frees once; `lent()` is a poll with no event.
4. **Tests.** Each test pins a behaviour that could regress; no test needs root, hugepages, a card or a pinned core; `ctest --preset debug` passes with the new tests, and `-L unit`, `-L integration`, `-L functional` each still pass.
5. **Build.** `cmake --build --preset debug` is warning-free (clang-tidy runs inside it), `./scripts/check-format.sh` passes, and from Task 5 on `cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan` passes too, since the fabric's locking is what tsan watches.

At the end, a whole-branch review against the spec's "Goals" and "Testing", then the completion gates of Task 16, then the pull request titled `[DEVICE][ETHDEV][FABRIC][LOOP][FRAMES][SCENARIO] Packet loan, scripted impairment and a scenario-script runner`.

## Review Focus

Five conditions the spec implies but no task's tests exercise directly, most likely to bite first; each has its pin added to the owning task:

1. **A checksum fill into a lent block under `EmulatedOffloads::Checksums`.** The lender's checksum fields change under it, which the contract allows and a reader of `device.md` must expect. Task 4 pins it with `AChecksumFillWritesIntoTheLentBlock`: the two checksum fields differ after transmit, every other byte is the same, and the receiver's copy verifies.
2. **A script whose time goes backwards, or a `*` line nothing ever satisfies.** Task 13 pins both: an absolute time earlier than the current stamp fails the script at that line; a `*` outbound line fails once `any_bound` has passed, with the bound in the message.
3. **A `write` the peer's window does not cover, and a `read` asking for more than is unread.** Both must fail with the counts, never hang or pass. Task 14 pins them with `WriteBeyondTheWindowFailsWithTheCountAccepted` and `ReadBeyondUnreadFailsWithTheCounts`.
4. **A segment whose checksum is wrong while every stated field matches.** Task 12's `Harness::collect` returns the residues with each frame; Task 13 pins that the runner fails such a segment by feeding a hand-built frame with a corrupted checksum through `verify_checksums`.
5. **`Fabric::advance` with a stamp earlier than the last.** A test loop that runs two harnesses on one fabric can do that. Task 6 pins that an earlier stamp delivers nothing new, leaves `now()` where it was and asserts nothing.

---

## File Structure

| File | Responsibility |
|---|---|
| `common/device/packet/packet.hpp` | `IsPacket` gains `lend()` and `lent()`; the contract in the comment. |
| `common/device/device/device.hpp` | `Capabilities::tx_reclaim`; `IsDevice` gains `reclaim(queue)`; the contract. |
| `common/ethdev/packet/ethdev_packet.hpp` | `lend`, `lent`, the asserts on mutation. |
| `common/ethdev/port/port.hpp`, `port.cpp` | `reclaim`, the `tx_reclaim` probe, the receive copy of a shared mbuf, the fast-free `static_assert`. |
| `fixtures/fabric/packet/fabric_packet.hpp`, `.cpp` | `Block::lent`, `Block::orphaned`, `Packet::borrowed_`, `lend`, `lent`, `borrowed`, the release logic. |
| `fixtures/fabric/fabric/impairment.hpp` | `Action`, `Verdict`, `Egress`, `Impairment`, `ImpairmentCounters`. |
| `fixtures/fabric/fabric/fabric.hpp`, `.cpp` | `LoanReturn`, `PortConfig::loans`, the in-flight list and its release, `reclaim`, `return_loans`, `loans_outstanding`, `set_impairment`, `impairment_counters`, the egress pipeline; `Fabric::advance`, `now`, the delayed-frame queue. |
| `common/loop/queue/shard_queue.hpp` | `reclaim()`. |
| `tests/shared/device/device_conformance.hpp` | Four loan tests and a `pool_count()` helper. |
| `fixtures/frames/frames/tcp_frames.hpp` | `TcpSpec::options` (raw bytes) and `tcp_options_of(ParsedFrame)`. |
| `fixtures/scenario/CMakeLists.txt`, `export/aloe/scenario` | The module's targets and umbrella. |
| `fixtures/scenario/script/script.hpp` | `TimeSpec`, `Direction`, `TcpOption`, `Options`, `PacketLine`, `Argument`, `CallLine`, `Event`, `Script`. |
| `fixtures/scenario/script/parse_script.hpp`, `.cpp` | `ParseError`, `parse_script`. |
| `fixtures/scenario/script/notation.hpp`, `.cpp` | `encode_options`, `describe` for lines and observed segments, `format_time`. |
| `fixtures/scenario/run/peer.hpp`, `.cpp` | `ObservedSegment`, `Peer`: ISNs, ports, relative numbers both ways. |
| `fixtures/scenario/run/match.hpp`, `.cpp` | `match(PacketLine, ObservedSegment)`. |
| `fixtures/scenario/run/harness.hpp`, `.cpp` | `HarnessConfig`, `Harness`: the fabric, the two ports, the bricks, `inject`, `step`, `flush`, `collect`. |
| `fixtures/scenario/run/runner.hpp`, `.cpp` | `RunnerConfig`, `Failure`, `Runner`, `format`. |
| `tests/unit_tests/fixtures/scenario/*.cpp` | Parser, peer, match, harness and runner tests. |
| `tests/unit_tests/scenarios/CMakeLists.txt`, `scenario_main.cpp` | The scripts binary: discovers `tests/scenarios/**/*.pkt`, registers one test per script and checksum mode. |
| `tests/scenarios/tcp/{handshake,data,close,reset}/*.pkt` | The first fifteen scripts. |
| `docs/architecture/{device,fixtures,scenario,loop}.md`, `README.md`, `AGENTS.md`, `docs/roadmap.md`, `docs/guides/getting-started.md` | The documentation. |

---

## Task 1: `ethdev::Packet` lends a handle and reports the loan

**Files:**
- Modify: `common/ethdev/packet/ethdev_packet.hpp`
- Test: `tests/integration_tests/ethdev/test_ethdev_packet.cpp`

**Interfaces:**
- Consumes: `rte_mbuf_refcnt_read`, `rte_mbuf_refcnt_update` from `<rte_mbuf.h>` (already included); `core::force_non_const` from `<aloe/core>` (already included).
- Produces: `[[nodiscard]] Packet Packet::lend() noexcept` and `[[nodiscard]] bool Packet::lent() const noexcept` on `aloe::ethdev::Packet`; the asserts `!lent()` in `prepend`, `append`, `trim_front`, `trim_back` and `set_tx`. Task 5 adds both to the concept; Tasks 2 and 5 test them through a port.

- [ ] **Step 1: Write the failing tests**

Append to `tests/integration_tests/ethdev/test_ethdev_packet.cpp`:

```cpp
TEST_F(EthdevPacket, LendsASecondHandleOverTheSameMbuf) {
    const unsigned before       = rte_mempool_avail_count(pool_);
    aloe::ethdev::Packet packet = allocate();
    ASSERT_TRUE(packet.append(10).has_value());
    const aloe::device::TxMetadata tx{.l2_length = 14, .l3_length = 20};
    packet.set_tx(tx);
    EXPECT_FALSE(packet.lent());

    aloe::ethdev::Packet handle = packet.lend();
    EXPECT_TRUE(packet.lent());
    EXPECT_EQ(handle.get(), packet.get()) << "one mbuf, two handles";
    EXPECT_EQ(rte_mbuf_refcnt_read(packet.get()), 2);
    EXPECT_EQ(handle.size(), 10);
    EXPECT_EQ(handle.tx(), tx) << "the handle carries the transmit metadata as of the lend";
    EXPECT_EQ(rte_mempool_avail_count(pool_), before - 1);

    handle = aloe::ethdev::Packet{};
    EXPECT_FALSE(packet.lent()) << "back once the handle is gone";
    EXPECT_EQ(rte_mbuf_refcnt_read(packet.get()), 1);
    EXPECT_EQ(rte_mempool_avail_count(pool_), before - 1) << "dropping the handle frees nothing";
    EXPECT_EQ(packet.size(), 10);

    packet = aloe::ethdev::Packet{};
    EXPECT_EQ(rte_mempool_avail_count(pool_), before) << "the lender frees the mbuf when it is the last handle";
}

TEST_F(EthdevPacket, ADroppedLenderFreesWhenTheLastHandleGoes) {
    const unsigned before = rte_mempool_avail_count(pool_);
    aloe::ethdev::Packet handle;
    {
        aloe::ethdev::Packet packet = allocate();
        handle                      = packet.lend();
    }
    EXPECT_EQ(rte_mempool_avail_count(pool_), before - 1) << "the handle keeps the mbuf alive";
    EXPECT_EQ(rte_mbuf_refcnt_read(handle.get()), 1);
    handle = aloe::ethdev::Packet{};
    EXPECT_EQ(rte_mempool_avail_count(pool_), before) << "freed exactly once, by the last handle";
}

TEST_F(EthdevPacket, AHandleCanBeLentAgainOnceItIsBack) {
    aloe::ethdev::Packet packet = allocate();
    {
        const aloe::ethdev::Packet first = packet.lend();
        EXPECT_TRUE(packet.lent());
    }
    EXPECT_FALSE(packet.lent());
    const aloe::ethdev::Packet second = packet.lend();
    EXPECT_TRUE(packet.lent());
    EXPECT_EQ(rte_mbuf_refcnt_read(packet.get()), 2);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset debug --target Aloe.Tests.Integration.Ethdev.Null`
Expected: FAIL to compile, `no member named 'lend'`.

- [ ] **Step 3: Add `lend` and `lent` to `ethdev::Packet`**

In `common/ethdev/packet/ethdev_packet.hpp`, after `set_tx` and before `get()`:

```cpp
        /**
         * @brief A second handle over the same mbuf, for the device to transmit: the loan.
         *
         * The packet is lent until that handle is gone, whether it was transmitted and freed by the
         * driver or dropped untransmitted. Called on a packet that is not empty and not lent, after
         * the frame is complete and `set_tx` has run. Raises the mbuf's reference count; nothing
         * above ethdev sees the number.
         */
        [[nodiscard]] Packet lend() noexcept {
            assert(mbuf_ != nullptr && "lend of an empty packet");
            assert(!lent() && "one loan at a time");
            core::force_non_const(this);  // writes the mbuf's count, which a const member could still do
            rte_mbuf_refcnt_update(mbuf_, 1);
            return Packet{mbuf_};
        }

        /// True while a handle from `lend()` is alive somewhere. A poll: no backend has an event for the return.
        [[nodiscard]] bool lent() const noexcept {
            return mbuf_ != nullptr && rte_mbuf_refcnt_read(mbuf_) > 1;
        }
```

Add `assert(!lent() && "a lent packet is not written");` as the first line of `prepend`, `append`, `trim_front`, `trim_back` and `set_tx` (before the `mbuf_ == nullptr` checks; `lent()` is false on an empty packet so the order is harmless). Update the class comment: "A packet can lend a second handle to the device and asks `lent()` whether the device is done; see `device::IsPacket`."

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset debug --target Aloe.Tests.Integration.Ethdev.Null && ./build/debug/tests/integration_tests/ethdev/Aloe.Tests.Integration.Ethdev.Null --gtest_filter='EthdevPacket.*'`
Expected: PASS, 8 tests.

- [ ] **Step 5: Commit**

```bash
git add common/ethdev/packet/ethdev_packet.hpp tests/integration_tests/ethdev/test_ethdev_packet.cpp
git commit -m "feat(ethdev): lend a second handle over an mbuf and report the loan"
```

---

## Task 2: `ethdev::Port`: the reclaim, the shared-mbuf copy on receive, fast-free kept off

**Files:**
- Modify: `common/device/device/device.hpp` (`Capabilities::tx_reclaim`, the `dropped` comment)
- Modify: `common/ethdev/port/port.hpp`, `common/ethdev/port/port.cpp`
- Test: `tests/integration_tests/ethdev/test_ethdev_ring.cpp`, `tests/integration_tests/ethdev/test_ethdev_null.cpp`
- Test (manual): `tests/manual_tests/ethdev/test_ethdev_tap.cpp`

**Interfaces:**
- Consumes: Task 1's `lend`/`lent`; `rte_eth_tx_done_cleanup`, `rte_eth_dev_conf_get`, `RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE` from `<rte_ethdev.h>`; `rte_pktmbuf_copy`, `rte_mbuf_refcnt_read` from `<rte_mbuf.h>`.
- Produces: `bool device::Capabilities::tx_reclaim = false`; `[[nodiscard]] std::size_t ethdev::Port::reclaim(std::uint16_t queue) noexcept`; `Port::receive` delivering a copy of any shared mbuf; the `static_assert` on `tx_checksum_offloads`. Task 5 puts `reclaim` into `IsDevice` and the fakes.

- [ ] **Step 1: Write the failing tests**

Append to `tests/integration_tests/ethdev/test_ethdev_ring.cpp` (inside the file's existing includes add `<rte_mbuf.h>` if `rte_mbuf_refcnt_read` is used; the tests below use only the packet API):

```cpp
TEST(EthdevRing, AReceivedSharedMbufIsACopyAndTheLoanIsBack) {
    aloe::ethdev::Port port{
        {.name = aloe::testing::probe_vdev("net_ring"), .queues = 1, .pool_size = 256}
    };
    const auto frame = aloe::frames::ethernet_frame(
        port.mac(), aloe::testing::peer, aloe::frames::ethertype_experimental, aloe::frames::pattern(60, 5));
    auto lender = port.allocate(0);
    ASSERT_TRUE(lender.has_value());
    ASSERT_TRUE(aloe::frames::fill(*lender, frame));

    std::array<aloe::ethdev::Packet, 1> burst{lender->lend()};
    ASSERT_EQ(port.transmit(0, burst), 1);
    EXPECT_TRUE(burst[0].empty());
    EXPECT_TRUE(lender->lent()) << "the ring holds the very mbuf until the reader takes it";

    std::array<aloe::ethdev::Packet, 2> received;
    ASSERT_EQ(port.receive(0, received), 1);
    EXPECT_NE(received[0].get(), lender->get()) << "the reader gets a copy";
    EXPECT_FALSE(lender->lent()) << "and the receive dropped the shared reference";
    EXPECT_EQ(aloe::frames::bytes_of(received[0]), frame);
    EXPECT_EQ(received[0].headroom(), aloe::device::packet_headroom);
    EXPECT_EQ(received[0].tx(), aloe::device::TxMetadata{});

    received[0].trim_front(14);
    EXPECT_EQ(lender->size(), frame.size()) << "a trim on the copy moves nothing of the lender's";
    EXPECT_EQ(aloe::frames::bytes_of(*lender), frame);
    EXPECT_EQ(port.counters(0).received, 1);
    EXPECT_EQ(port.counters(0).dropped, 0);
}

TEST(EthdevRing, AnUnsharedMbufIsNotCopied) {
    aloe::ethdev::Port port{
        {.name = aloe::testing::probe_vdev("net_ring"), .queues = 1, .pool_size = 256}
    };
    const auto frame = aloe::frames::ethernet_frame(
        port.mac(), aloe::testing::peer, aloe::frames::ethertype_experimental, aloe::frames::pattern(40, 1));
    auto packet = port.allocate(0);
    ASSERT_TRUE(packet.has_value());
    ASSERT_TRUE(aloe::frames::fill(*packet, frame));
    rte_mbuf* const raw = packet->get();
    std::array<aloe::ethdev::Packet, 1> burst{std::move(*packet)};
    ASSERT_EQ(port.transmit(0, burst), 1);
    std::array<aloe::ethdev::Packet, 1> received;
    ASSERT_EQ(port.receive(0, received), 1);
    EXPECT_EQ(received[0].get(), raw) << "the ring hands the mbuf itself; nothing to protect";
}

TEST(EthdevRing, ReclaimIsUnsupportedByTheRingDriver) {
    aloe::ethdev::Port port{
        {.name = aloe::testing::probe_vdev("net_ring"), .queues = 1, .pool_size = 256}
    };
    EXPECT_FALSE(port.capabilities().tx_reclaim);
    EXPECT_EQ(port.reclaim(0), 0);
}

TEST(EthdevRing, AReceiveWhoseCopyFindsNoMbufDropsAndCounts) {
    aloe::ethdev::Port port{
        {.name = aloe::testing::probe_vdev("net_ring"), .queues = 1, .pool_size = 64}
    };
    const auto frame = aloe::frames::ethernet_frame(
        port.mac(), aloe::testing::peer, aloe::frames::ethertype_experimental, aloe::frames::pattern(40, 2));
    auto lender = port.allocate(0);
    ASSERT_TRUE(lender.has_value());
    ASSERT_TRUE(aloe::frames::fill(*lender, frame));
    std::array<aloe::ethdev::Packet, 1> burst{lender->lend()};
    ASSERT_EQ(port.transmit(0, burst), 1);

    std::vector<aloe::ethdev::Packet> held;
    while (auto packet = port.allocate(0)) {
        held.push_back(std::move(*packet));
    }
    ASSERT_FALSE(held.empty());

    std::array<aloe::ethdev::Packet, 1> received;
    EXPECT_EQ(port.receive(0, received), 0) << "no mbuf for the copy: the frame is dropped";
    EXPECT_EQ(port.counters(0).dropped, 1);
    EXPECT_FALSE(lender->lent()) << "the shared reference was dropped with it";
    held.clear();
    EXPECT_EQ(port.receive(0, received), 0) << "the frame is gone, not retried";
}
```

The ring tests already include `<aloe/frames>`, `<array>`, `<vector>`; add `<utility>` and `<rte_mbuf.h>` (for `rte_mbuf*`) if missing.

Append to `tests/integration_tests/ethdev/test_ethdev_null.cpp` (add `#include <rte_ethdev.h>` to its includes):

```cpp
TEST(EthdevNull, TheNullDriverReturnsALoanAtTransmit) {
    aloe::ethdev::Port port{
        {.name = aloe::testing::probe_vdev("net_null"), .queues = 1, .pool_size = 256}
    };
    auto lender = port.allocate(0);
    ASSERT_TRUE(lender.has_value());
    ASSERT_TRUE(lender->append(60).has_value());
    std::array<aloe::ethdev::Packet, 1> burst{lender->lend()};
    EXPECT_TRUE(lender->lent());
    ASSERT_EQ(port.transmit(0, burst), 1);
    EXPECT_FALSE(lender->lent()) << "eth_null_tx frees the burst inside the call";
    EXPECT_FALSE(port.capabilities().tx_reclaim);
    EXPECT_EQ(port.reclaim(0), 0);
}

TEST(EthdevNull, FastFreeStaysOff) {
    const aloe::ethdev::Port port{
        {.name = aloe::testing::probe_vdev("net_null"), .queues = 1, .pool_size = 256}
    };
    rte_eth_conf conf{};
    ASSERT_EQ(rte_eth_dev_conf_get(port.port_id(), &conf), 0);
    EXPECT_EQ(conf.txmode.offloads & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE, 0U)
        << "fast-free assumes one reference per transmitted mbuf; a lent packet has two";
}
```

Append to `tests/manual_tests/ethdev/test_ethdev_tap.cpp`, modelled on `FramesCrossBetweenTheKernelAndThePort` (same port construction and `PacketSocket`):

```cpp
TEST(EthdevTap, ALoanIsBackOnceTheTapHasWrittenTheFrame) {
    // Build the port and the kernel socket exactly as FramesCrossBetweenTheKernelAndThePort does, then:
    const auto frame = aloe::frames::ethernet_frame(peer, port.mac(), aloe::frames::ethertype_experimental, aloe::frames::pattern(60, 3));
    auto lender = port.allocate(0);
    ASSERT_TRUE(lender.has_value());
    ASSERT_TRUE(aloe::frames::fill(*lender, frame));
    std::array<aloe::ethdev::Packet, 1> burst{lender->lend()};
    ASSERT_EQ(port.transmit(0, burst), 1);
    EXPECT_FALSE(lender->lent()) << "the tap driver frees the mbuf after writing it to the kernel";
    EXPECT_TRUE(kernel_receives(kernel, frame));
    EXPECT_EQ(aloe::frames::bytes_of(*lender), frame);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset debug --target Aloe.Tests.Integration.Ethdev.Ring Aloe.Tests.Integration.Ethdev.Null`
Expected: FAIL to compile, `no member named 'reclaim'` and `no member named 'tx_reclaim'`.

- [ ] **Step 3: Add `tx_reclaim` to `Capabilities` and update the `dropped` comment**

In `common/device/device/device.hpp`, after `tx_l4_checksum`:

```cpp
        bool tx_reclaim = false;  ///< `reclaim(queue)` asks the driver to free finished transmits; false: it returns zero.
```

Change `QueueCounters::dropped`'s comment to `///< Arrived with no room in the receive queue, or no buffer to copy a shared one into.`

- [ ] **Step 4: Add `reclaim`, the probe, the copy and the `static_assert` to `ethdev::Port`**

`port.hpp`, after `transmit`:

```cpp
        /// Asks the driver to free transmitted mbufs it still holds; how many it let go of. Zero, and nothing done,
        /// when `capabilities().tx_reclaim` is false: none of the ring, null and tap drivers implements it.
        [[nodiscard]] std::size_t reclaim(std::uint16_t queue) noexcept;
```

`port.cpp`:

- Beside `tx_checksum_offloads` in the anonymous namespace:

```cpp
        // Fast-free frees a transmitted mbuf straight to its pool assuming it has one reference. A lent packet has
        // two, so the offload stays off whatever the driver offers; the mask is the invariant.
        static_assert((tx_checksum_offloads & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE) == 0);
```

- In the constructor, after `started_ = true;` and before the MTU read:

```cpp
            capabilities_.tx_reclaim = rte_eth_tx_done_cleanup(port_id_, 0, 0) >= 0;  // -ENOTSUP from a driver without it
```

- `reclaim`:

```cpp
    std::size_t Port::reclaim(const std::uint16_t queue) noexcept {
        assert(queue < queues_);
        core::force_non_const(this);  // the driver frees mbufs, which a const member could still ask for
        const int freed = rte_eth_tx_done_cleanup(port_id_, queue, 0);
        return freed > 0 ? static_cast<std::size_t>(freed) : 0;
    }
```

- In `receive`, at the top of the per-mbuf loop, before the existing size check:

```cpp
            rte_mbuf* mbuf = burst[index];
            if (rte_mbuf_refcnt_read(mbuf) > 1) {
                // Only a loopback driver delivers a shared mbuf: a packet some lender still holds. The reader gets
                // its own copy, so a trim here never moves the lender's offsets; a card never takes this branch.
                rte_mbuf* const copy = rte_pktmbuf_copy(mbuf, pools_[queue], 0, UINT32_MAX);
                rte_pktmbuf_free(mbuf);  // drops this reference; the lender keeps the mbuf
                if (copy == nullptr) {
                    ++counters_[queue].dropped;
                    continue;
                }
                mbuf = copy;
            }
```

The existing `nb_segs`/length check and the flag clearing then run on `mbuf` as before. `<cstdint>` for `UINT32_MAX` is already included.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset debug --target Aloe.Tests.Integration.Ethdev.Ring Aloe.Tests.Integration.Ethdev.Null && ctest --preset debug -R 'Ethdev'`
Expected: PASS. Then build the manual tap test (`--target Aloe.Tests.Manual.Ethdev.Tap`); it is run as root at Task 16's gates.

- [ ] **Step 6: Commit**

```bash
git add common/device/device/device.hpp common/ethdev/port/port.hpp common/ethdev/port/port.cpp \
    tests/integration_tests/ethdev/test_ethdev_ring.cpp tests/integration_tests/ethdev/test_ethdev_null.cpp \
    tests/manual_tests/ethdev/test_ethdev_tap.cpp
git commit -m "feat(ethdev): reclaim finished transmits, copy a shared mbuf on receive, keep fast-free off"
```

---

## Task 3: `fabric::Packet` lends a handle; a dropped lender orphans its block

**Files:**
- Modify: `fixtures/fabric/packet/fabric_packet.hpp`, `fixtures/fabric/packet/fabric_packet.cpp`
- Test: `tests/unit_tests/fixtures/fabric/test_fabric_packet.cpp`

**Interfaces:**
- Produces: `detail::Block{storage, capacity, bool lent = false, bool orphaned = false}`; `[[nodiscard]] Packet Packet::lend() noexcept`; `[[nodiscard]] bool Packet::lent() const noexcept`; `[[nodiscard]] bool Packet::borrowed() const noexcept` (true on a handle from `lend()`); the release logic below. Task 4's port keeps borrowed handles in flight.

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit_tests/fixtures/fabric/test_fabric_packet.cpp`:

```cpp
TEST(FabricPacket, LendsAHandleAndReportsTheLoan) {
    aloe::fabric::Pool pool{1, capacity};
    auto packet = pool.allocate();
    ASSERT_TRUE(packet.has_value());
    ASSERT_TRUE(packet->append(5).has_value());
    packet->set_tx(aloe::device::TxMetadata{.l2_length = 14});
    EXPECT_FALSE(packet->lent());
    EXPECT_FALSE(packet->borrowed());

    aloe::fabric::Packet handle = packet->lend();
    EXPECT_TRUE(packet->lent());
    EXPECT_TRUE(handle.borrowed());
    EXPECT_FALSE(packet->borrowed());
    EXPECT_EQ(handle.size(), 5);
    EXPECT_EQ(handle.data().data(), packet->data().data()) << "the same bytes";
    EXPECT_EQ(handle.tx().l2_length, 14) << "the transmit metadata as of the lend";
    EXPECT_EQ(pool.available(), 0);

    handle = aloe::fabric::Packet{};
    EXPECT_FALSE(packet->lent()) << "back once the handle is gone";
    EXPECT_EQ(pool.available(), 0) << "dropping the handle gives nothing back";
    *packet = aloe::fabric::Packet{};
    EXPECT_EQ(pool.available(), 1);
}

TEST(FabricPacket, ADroppedLenderGivesTheBlockBackWhenTheHandleGoes) {
    aloe::fabric::Pool pool{1, capacity};
    aloe::fabric::Packet handle;
    {
        auto packet = pool.allocate();
        ASSERT_TRUE(packet.has_value());
        handle = packet->lend();
    }
    EXPECT_EQ(pool.available(), 0) << "the handle keeps the block";
    EXPECT_FALSE(handle.empty());
    handle = aloe::fabric::Packet{};
    EXPECT_EQ(pool.available(), 1) << "given back once, by the last handle";
}

TEST(FabricPacket, MovingAHandleKeepsItBorrowed) {
    aloe::fabric::Pool pool{1, capacity};
    auto packet = pool.allocate();
    ASSERT_TRUE(packet.has_value());
    aloe::fabric::Packet handle = packet->lend();
    aloe::fabric::Packet moved{std::move(handle)};
    EXPECT_TRUE(handle.empty());  // NOLINT(bugprone-use-after-move): the moved-from state is the point
    EXPECT_TRUE(moved.borrowed());
    EXPECT_TRUE(packet->lent());
    moved = aloe::fabric::Packet{};
    EXPECT_FALSE(packet->lent());
    EXPECT_EQ(pool.available(), 0);
}

TEST(FabricPacket, APacketCanBeLentAgainOnceItIsBack) {
    aloe::fabric::Pool pool{1, capacity};
    auto packet = pool.allocate();
    ASSERT_TRUE(packet.has_value());
    {
        const aloe::fabric::Packet first = packet->lend();
        EXPECT_TRUE(packet->lent());
    }
    EXPECT_FALSE(packet->lent());
    const aloe::fabric::Packet second = packet->lend();
    EXPECT_TRUE(packet->lent());
    EXPECT_TRUE(second.borrowed());
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Fabric`
Expected: FAIL to compile, `no member named 'lend'`.

- [ ] **Step 3: Implement the marks and the handle**

`fabric_packet.hpp`:

- `detail::Block` gains `bool lent = false;  ///< A handle from lend() is alive.` and `bool orphaned = false;  ///< The lender is gone; the handle gives the block back.`
- `Packet` gains the public members, after `set_tx`:

```cpp
        /// A second handle over the same block, for the port to transmit: the loan. One at a time; not empty.
        [[nodiscard]] Packet lend() noexcept;
        /// True while a handle from `lend()` is alive: in a ring, or in the port's in-flight list.
        [[nodiscard]] bool lent() const noexcept {
            return block_ != nullptr && block_->lent;
        }
        /// True on the handle `lend()` returned, false on the lender.
        [[nodiscard]] bool borrowed() const noexcept {
            return borrowed_;
        }
```

- A private member `bool borrowed_ = false;` after `tx_`, exchanged in the move constructor and move assignment like the others.
- The class comment gains: "A packet can lend a second handle to the port and asks `lent()` whether the port is done; a lender dropped while lent orphans its block, which goes back to the pool with the handle."

`fabric_packet.cpp`:

```cpp
    Packet Packet::lend() noexcept {
        assert(block_ != nullptr && "lend of an empty packet");
        assert(!block_->lent && "one loan at a time");
        block_->lent = true;
        Packet handle{*pool_, *block_};
        handle.begin_    = begin_;
        handle.end_      = end_;
        handle.tx_       = tx_;
        handle.borrowed_ = true;
        return handle;
    }

    void Packet::release() noexcept {
        if (block_ == nullptr) {
            return;
        }
        if (borrowed_) {
            block_->lent = false;
            if (block_->orphaned) {
                block_->orphaned = false;
                pool_->give_back(*block_);
            }
        } else if (block_->lent) {
            block_->orphaned = true;  // the handle gives the block back when it goes
        } else {
            pool_->give_back(*block_);
        }
        pool_     = nullptr;
        block_    = nullptr;
        begin_    = 0;
        end_      = 0;
        borrowed_ = false;
    }
```

Add `assert(!lent() && "a lent packet is not written");` as the first line of `prepend`, `append`, `trim_front`, `trim_back` (in the `.cpp`) and `set_tx` (in the header). `Pool::allocate` constructs `Packet{*this, *block}` as before; a block handed out has both marks false, which `give_back` can assert.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Fabric && ./build/debug/tests/unit_tests/fixtures/fabric/Aloe.Tests.Unit.Fabric --gtest_filter='FabricPacket.*'`
Expected: PASS, 11 tests.

- [ ] **Step 5: Commit**

```bash
git add fixtures/fabric/packet/fabric_packet.hpp fixtures/fabric/packet/fabric_packet.cpp tests/unit_tests/fixtures/fabric/test_fabric_packet.cpp
git commit -m "feat(fabric): lend a second handle over a block and orphan it when the lender goes first"
```

---

## Task 4: `fabric::Port` holds transmitted loans; the loan policy; `Fabric::advance`

**Files:**
- Modify: `fixtures/fabric/fabric/fabric.hpp`, `fixtures/fabric/fabric/fabric.cpp`
- Create: `tests/unit_tests/fixtures/fabric/test_fabric_loans.cpp`
- Modify: `tests/unit_tests/fixtures/fabric/CMakeLists.txt` (add the file to `Aloe.Tests.Unit.Fabric`, and link `Aloe::Fixtures::Frames`, which the test uses for frames)

**Interfaces:**
- Consumes: Task 3's `lend`, `lent`, `borrowed`; Task 2's `Capabilities::tx_reclaim`.
- Produces: `enum class aloe::fabric::LoanReturn : std::uint8_t { OnStep, Held }`; `PortConfig::loans = LoanReturn::OnStep`; `[[nodiscard]] std::size_t Port::reclaim(std::uint16_t queue) noexcept`; `void Port::return_loans(std::uint16_t queue) noexcept`; `[[nodiscard]] std::size_t Port::loans_outstanding(std::uint16_t queue) const noexcept`; `Capabilities::tx_reclaim == true` on every fabric port; `void Fabric::advance(core::TimePoint now)`; `[[nodiscard]] core::TimePoint Fabric::now() const noexcept`. Task 6 extends `advance` with delayed frames.

- [ ] **Step 1: Write the failing tests**

`tests/unit_tests/fixtures/fabric/test_fabric_loans.cpp`:

```cpp
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using aloe::fabric::LoanReturn;
    using aloe::fabric::Packet;
    using TimePoint = aloe::core::TimePoint;

    constexpr aloe::wire::MacAddress alpha{0x02, 0, 0, 0, 0, 0x0a};
    constexpr aloe::wire::MacAddress beta{0x02, 0, 0, 0, 0, 0x0b};
    constexpr aloe::wire::MacAddress gamma{0x02, 0, 0, 0, 0, 0x0c};

    aloe::fabric::PortConfig port(const aloe::wire::MacAddress& mac, const LoanReturn loans = LoanReturn::OnStep) {
        return {.mac = mac, .queues = 1, .pool_size = 4, .loans = loans};
    }

    std::vector<std::byte> frame_to(const aloe::wire::MacAddress& destination, const std::uint8_t seed = 0) {
        return aloe::frames::ethernet_frame(destination, alpha, aloe::frames::ethertype_experimental, aloe::frames::pattern(30, seed));
    }

    /// Lends a packet holding `frame` and transmits the handle; returns the lender.
    Packet lend_and_transmit(aloe::fabric::Port& source, std::span<const std::byte> frame) {
        auto lender = source.allocate(0);
        EXPECT_TRUE(lender.has_value());
        if (!lender) {
            return Packet{};
        }
        EXPECT_TRUE(aloe::frames::fill(*lender, frame));
        std::array<Packet, 1> burst{lender->lend()};
        EXPECT_EQ(source.transmit(0, burst), 1);
        EXPECT_TRUE(burst[0].empty());
        return std::move(*lender);
    }

    std::vector<std::vector<std::byte>> drain(aloe::fabric::Port& port) {
        std::vector<std::vector<std::byte>> frames;
        std::array<Packet, 8> burst;
        for (std::size_t count = port.receive(0, burst); count > 0; count = port.receive(0, burst)) {
            for (std::size_t index = 0; index < count; ++index) {
                frames.push_back(aloe::frames::bytes_of(burst[index]));
                burst[index] = Packet{};
            }
        }
        return frames;
    }

    /// How many packets the port's queue 0 can hand out right now.
    std::size_t pool_count(aloe::fabric::Port& port) {
        std::vector<Packet> held;
        while (auto packet = port.allocate(0)) {
            held.push_back(std::move(*packet));
        }
        return held.size();
    }

}  // namespace

TEST(FabricLoans, ATransmittedHandleIsHeldUntilTheStepAndReleasedByTheQueuesNextCall) {
    aloe::fabric::Fabric fabric;
    auto& a = fabric.add_port(port(alpha));
    auto& b = fabric.add_port(port(beta));
    EXPECT_TRUE(a.capabilities().tx_reclaim);

    const Packet lender = lend_and_transmit(a, frame_to(beta));
    EXPECT_TRUE(lender.lent());
    EXPECT_EQ(a.loans_outstanding(0), 1);
    EXPECT_EQ(drain(b).size(), 1) << "the receiver got its copy at once";
    EXPECT_TRUE(lender.lent()) << "delivery does not return the loan";

    fabric.advance(TimePoint{} + 1ms);
    EXPECT_EQ(fabric.now(), TimePoint{} + 1ms);
    EXPECT_TRUE(lender.lent()) << "advance finishes the loan; the queue's next call releases it";
    std::array<Packet, 1> none;
    EXPECT_EQ(a.receive(0, none), 0);
    EXPECT_FALSE(lender.lent());
    EXPECT_EQ(a.loans_outstanding(0), 0);
}

TEST(FabricLoans, ReclaimReleasesAnOnStepLoanAtOnce) {
    aloe::fabric::Fabric fabric;
    auto& a = fabric.add_port(port(alpha));
    std::ignore = fabric.add_port(port(beta));
    const Packet lender = lend_and_transmit(a, frame_to(beta));
    EXPECT_EQ(a.reclaim(0), 1);
    EXPECT_FALSE(lender.lent());
    EXPECT_EQ(a.reclaim(0), 0);
    EXPECT_EQ(a.loans_outstanding(0), 0);
}

TEST(FabricLoans, AdvanceFinishesOnlyWhatWasTransmittedBefore) {
    aloe::fabric::Fabric fabric;
    auto& a = fabric.add_port(port(alpha));
    std::ignore = fabric.add_port(port(beta));
    const Packet first = lend_and_transmit(a, frame_to(beta, 1));
    fabric.advance(TimePoint{} + 1ms);
    const Packet second = lend_and_transmit(a, frame_to(beta, 2));  // this transmit releases the finished first
    EXPECT_FALSE(first.lent());
    EXPECT_TRUE(second.lent());
    std::array<Packet, 1> none;
    EXPECT_EQ(a.receive(0, none), 0);
    EXPECT_TRUE(second.lent()) << "not finished yet";
    fabric.advance(TimePoint{} + 2ms);
    EXPECT_EQ(a.receive(0, none), 0);
    EXPECT_FALSE(second.lent());
}

TEST(FabricLoans, AHeldPortReturnsOnlyOnRequest) {
    aloe::fabric::Fabric fabric;
    auto& held = fabric.add_port(port(gamma, LoanReturn::Held));
    std::ignore = fabric.add_port(port(beta));
    const Packet lender = lend_and_transmit(held, frame_to(beta));
    fabric.advance(TimePoint{} + 1ms);
    std::array<Packet, 1> none;
    EXPECT_EQ(held.receive(0, none), 0);
    EXPECT_TRUE(lender.lent()) << "a held port never finishes on its own";
    EXPECT_TRUE(held.capabilities().tx_reclaim);
    EXPECT_EQ(held.reclaim(0), 0);
    EXPECT_TRUE(lender.lent());
    held.return_loans(0);
    EXPECT_FALSE(lender.lent());
    EXPECT_EQ(held.loans_outstanding(0), 0);
}

TEST(FabricLoans, ADroppedLenderGivesItsBlockBackAtTheReturn) {
    aloe::fabric::Fabric fabric;
    auto& a = fabric.add_port(port(alpha));
    std::ignore = fabric.add_port(port(beta));
    { const Packet lender = lend_and_transmit(a, frame_to(beta)); }
    EXPECT_EQ(a.loans_outstanding(0), 1);
    EXPECT_EQ(pool_count(a), 3) << "the orphaned block is still in flight";
    EXPECT_EQ(a.reclaim(0), 1);
    EXPECT_EQ(pool_count(a), 4) << "given back once, at the return";
}

TEST(FabricLoans, AHandleDroppedWithoutTransmitReturnsAtOnce) {
    aloe::fabric::Fabric fabric;
    auto& a = fabric.add_port(port(alpha));
    auto lender = a.allocate(0);
    ASSERT_TRUE(lender.has_value());
    ASSERT_TRUE(aloe::frames::fill(*lender, frame_to(beta)));
    {
        const Packet handle = lender->lend();
        EXPECT_TRUE(lender->lent());
    }
    EXPECT_FALSE(lender->lent());
    EXPECT_EQ(a.loans_outstanding(0), 0);
}

TEST(FabricLoans, AnUnlentPacketTransmitsAsBefore) {
    aloe::fabric::Fabric fabric;
    auto& a = fabric.add_port(port(alpha));
    auto& b = fabric.add_port(port(beta));
    auto packet = a.allocate(0);
    ASSERT_TRUE(packet.has_value());
    ASSERT_TRUE(aloe::frames::fill(*packet, frame_to(beta)));
    std::array<Packet, 1> burst{std::move(*packet)};
    ASSERT_EQ(a.transmit(0, burst), 1);
    EXPECT_EQ(a.loans_outstanding(0), 0);
    EXPECT_EQ(drain(b).size(), 1);
    EXPECT_EQ(pool_count(a), 4);
}

TEST(FabricLoans, AChecksumFillWritesIntoTheLentBlock) {
    aloe::fabric::Fabric fabric;
    auto& a = fabric.add_port({.mac = alpha, .pool_size = 4, .offloads = aloe::fabric::EmulatedOffloads::Checksums});
    auto& b = fabric.add_port({.mac = beta, .pool_size = 4, .offloads = aloe::fabric::EmulatedOffloads::Checksums});
    const auto frame = aloe::frames::ipv4_frame(
        {.destination_mac  = beta,
         .source_mac       = alpha,
         .source           = aloe::wire::Ipv4Address{10, 0, 0, 1},
         .destination      = aloe::wire::Ipv4Address{10, 0, 0, 2},
         .source_port      = 1,
         .destination_port = 2,
         .protocol         = aloe::wire::Ipv4Protocol::Udp,
         .checksums        = aloe::frames::Checksums::Seeded},
        aloe::frames::pattern(16));
    auto lender = a.allocate(0);
    ASSERT_TRUE(lender.has_value());
    ASSERT_TRUE(aloe::frames::fill(*lender, frame));
    lender->set_tx({.l2_length          = 14,
                    .l3_length          = 20,
                    .fill_ipv4_checksum = true,
                    .fill_l4_checksum   = aloe::device::L4Checksum::Udp});
    std::array<Packet, 1> burst{lender->lend()};
    ASSERT_EQ(a.transmit(0, burst), 1);

    const auto after = aloe::frames::bytes_of(*lender);
    EXPECT_NE(after, frame) << "the fill completed the checksums in the lent block";
    for (std::size_t index = 0; index < frame.size(); ++index) {
        const bool checksum_byte = index == 24 || index == 25 || index == 40 || index == 41;  // IPv4 at 14+10, UDP at 14+20+6
        if (!checksum_byte) {
            EXPECT_EQ(after[index], frame[index]) << "byte " << index;
        }
    }
    const auto got = drain(b);
    ASSERT_EQ(got.size(), 1);
    EXPECT_EQ(got[0], after) << "the receiver's copy is the filled frame";
    EXPECT_EQ(a.reclaim(0), 1);
}
```

Add `test_fabric_loans.cpp` to `Aloe.Tests.Unit.Fabric` in `tests/unit_tests/fixtures/fabric/CMakeLists.txt` and `Aloe::Fixtures::Frames` to its link list.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Fabric`
Expected: FAIL to compile, `no member named 'loans'` in `PortConfig`.

- [ ] **Step 3: Implement the policy, the in-flight list and the step**

`fabric.hpp`:

```cpp
    /// When a port lets go of a transmitted loan.
    enum class LoanReturn : std::uint8_t {
        OnStep,  ///< Finished by `Fabric::advance`, released by the queue's next transmit, receive or reclaim; `reclaim` releases at once.
        Held,    ///< Only `Port::return_loans` releases; `reclaim` returns zero. The driver that will not let go.
    };
```

`PortConfig` gains `LoanReturn loans = LoanReturn::OnStep;` after `offloads`. `Port` gains, after `counters`:

```cpp
        /// Releases every loan of the queue on an OnStep port and returns how many; zero on a Held port. Queue thread.
        [[nodiscard]] std::size_t reclaim(std::uint16_t queue) noexcept;
        /// Releases every loan of the queue whatever the policy. Queue thread; the test's hand on a Held port.
        void return_loans(std::uint16_t queue) noexcept;
        /// Handles transmitted and not yet released.
        [[nodiscard]] std::size_t loans_outstanding(std::uint16_t queue) const noexcept;
```

`Port::Queue` gains, after `counters`: `std::vector<Packet> in_flight;  ///< Borrowed handles, in transmit order; after pool, so they die first.` and `std::size_t finished = 0;  ///< The front of in_flight that advance marked finished.` A private `void release_finished(Queue& queue) noexcept;` (caller holds the lock) and `friend class Fabric` already exists. `Fabric` gains:

```cpp
        /// The fixture's clock step: marks every OnStep loan transmitted before the call finished, and from Task 6
        /// on delivers the frames whose delay is due. Safe from any thread. An earlier stamp than the last changes nothing.
        void advance(core::TimePoint now);
        [[nodiscard]] core::TimePoint now() const noexcept;
```

with `mutable std::mutex mutex_; core::TimePoint now_{};` as members (`<mutex>` and `<aloe/core>` or `<clock.hpp>` included).

`fabric.cpp`:

- `capabilities_of` sets `.tx_reclaim = true`.
- `release_finished(queue)`: on `OnStep`, erase the first `queue.finished` entries of `in_flight` (destroying them releases the loans) and set `finished = 0`; on `Held`, nothing.
- `receive`: after taking the lock, `release_finished(source)`.
- `transmit`: at the top, once per call, `{ const std::lock_guard lock{source.mutex}; release_finished(source); }`. In the loop, for a packet that passed the size check and was delivered: if `packet.borrowed()`, lock, `source.in_flight.push_back(std::move(packet))`, `++transmitted`; else as today. The refused (oversized) path drops the handle as today, which returns the loan.
- `reclaim(queue)`: lock; `OnStep`: `n = in_flight.size(); in_flight.clear(); finished = 0; return n;`; `Held`: `return 0`.
- `return_loans(queue)`: lock; `in_flight.clear(); finished = 0;`.
- `loans_outstanding(queue)`: lock; `in_flight.size()`.
- `Fabric::advance(now)`: `{ lock mutex_; if (now < now_) return; now_ = now; }` then for every port and every queue: lock the queue; if the port's policy is `OnStep`, `queue.finished = queue.in_flight.size()`. `Fabric::now()`: lock and return `now_`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Fabric && ctest --preset debug -R Fabric`
Expected: PASS, including the unchanged conformance and delivery tests.

- [ ] **Step 5: Commit**

```bash
git add fixtures/fabric/fabric/fabric.hpp fixtures/fabric/fabric/fabric.cpp tests/unit_tests/fixtures/fabric/test_fabric_loans.cpp tests/unit_tests/fixtures/fabric/CMakeLists.txt
git commit -m "feat(fabric): hold transmitted loans under a return policy, and a clock step"
```

---

## Task 5: The concepts, the conformance suite, `ShardQueue::reclaim` and every device fake

**Files:**
- Modify: `common/device/packet/packet.hpp`, `common/device/device/device.hpp`
- Modify: `common/loop/queue/shard_queue.hpp`
- Modify: `tests/shared/device/device_conformance.hpp`
- Modify: `tests/shared/tcp/tcp_test_device.hpp`, `benchmarks/tick_to_trade/stamped_device.hpp`, `tests/unit_tests/common/device/test_concepts.cpp`, `tests/unit_tests/common/runtime/test_shard.cpp`, `tests/unit_tests/common/net/test_net_send.cpp`
- Create: `tests/unit_tests/common/loop/test_shard_queue.cpp`
- Modify: `tests/unit_tests/common/loop/CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 1 to 4.
- Produces: `IsPacket` requiring `{ packet.lend() } -> std::same_as<P>` and `{ const_packet.lent() } -> std::same_as<bool>`; `IsDevice` requiring `{ device.reclaim(queue) } -> std::same_as<std::size_t>`; `[[nodiscard]] std::size_t loop::ShardQueue<Device>::reclaim() noexcept`; four conformance tests and the fixture helper `std::size_t pool_count()`; `reclaim` on `TcpTestDevice`, `StampedDevice`, `FakeDevice`, `Throttled`, `StuckPort`; `lend`/`lent` on `FakePacket`.

- [ ] **Step 1: Write the failing tests**

In `tests/shared/device/device_conformance.hpp`, add to the fixture class after `receive_up_to`:

```cpp
        /// How many packets queue 0 can hand out right now: allocate until it refuses, then let them all go.
        std::size_t pool_count() {
            std::vector<Packet> held;
            while (auto packet = device().allocate(0)) {
                held.push_back(std::move(*packet));
            }
            return held.size();
        }
```

Add the tests before `REGISTER_TYPED_TEST_SUITE_P` and their names to the registration list:

```cpp
    TYPED_TEST_P(DeviceConformance, ALentPacketIsReadableAndBackOnceTheDeviceIsDone) {
        const auto frame                        = this->frame(80, 9);
        typename TestFixture::Packet lender     = this->packet_with(frame);
        EXPECT_FALSE(lender.lent());
        std::array<typename TestFixture::Packet, 1> burst{lender.lend()};
        EXPECT_TRUE(lender.lent());
        ASSERT_EQ(this->device().transmit(0, burst), 1);
        EXPECT_TRUE(burst[0].empty()) << "the handle is what the device took";
        EXPECT_EQ(frames::bytes_of(lender), frame) << "the lender reads its frame while lent";

        auto received = this->receive_up_to(1);
        ASSERT_EQ(received.size(), 1);
        EXPECT_EQ(frames::bytes_of(received[0]), frame);
        received[0].trim_front(wire::EthernetHeader::size);
        EXPECT_EQ(lender.size(), frame.size()) << "a trim on the received packet moves nothing of the lender's";
        received.clear();
        std::ignore = this->device().reclaim(0);
        EXPECT_FALSE(lender.lent()) << "back once the device is done: the reader freed its copy, or the reclaim ran";
        EXPECT_EQ(frames::bytes_of(lender), frame);
    }

    TYPED_TEST_P(DeviceConformance, AHandleDroppedWithoutTransmitReturnsTheLoanAtOnce) {
        const auto frame                    = this->frame(20, 4);
        typename TestFixture::Packet lender = this->packet_with(frame);
        {
            const typename TestFixture::Packet handle = lender.lend();
            EXPECT_TRUE(lender.lent());
        }
        EXPECT_FALSE(lender.lent());
        std::array<typename TestFixture::Packet, 1> burst{std::move(lender)};
        ASSERT_EQ(this->device().transmit(0, burst), 1) << "and the packet still transmits as any other";
        auto received = this->receive_up_to(1);
        ASSERT_EQ(received.size(), 1);
        EXPECT_EQ(frames::bytes_of(received[0]), frame);
    }

    TYPED_TEST_P(DeviceConformance, ADroppedLenderIsFreedOnceWhenTheDeviceReturnsIt) {
        const std::size_t before = this->pool_count();
        {
            typename TestFixture::Packet lender = this->packet_with(this->frame(20, 6));
            std::array<typename TestFixture::Packet, 1> burst{lender.lend()};
            ASSERT_EQ(this->device().transmit(0, burst), 1);
        }  // the lender is gone while the device still has the handle
        auto received = this->receive_up_to(1);
        ASSERT_EQ(received.size(), 1);
        received.clear();
        std::ignore = this->device().reclaim(0);
        EXPECT_EQ(this->pool_count(), before) << "the buffer came back exactly once";
    }

    TYPED_TEST_P(DeviceConformance, ReclaimAgreesWithItsCapability) {
        if (!this->device().capabilities().tx_reclaim) {
            EXPECT_EQ(this->device().reclaim(0), 0) << "a device without the call does nothing and says so";
            return;
        }
        typename TestFixture::Packet lender = this->packet_with(this->frame(20, 8));
        std::array<typename TestFixture::Packet, 1> burst{lender.lend()};
        ASSERT_EQ(this->device().transmit(0, burst), 1);
        this->receive_up_to(1).clear();
        std::ignore = this->device().reclaim(0);
        EXPECT_FALSE(lender.lent()) << "a device with the call lets go of a finished transmit";
    }
```

`tests/unit_tests/common/loop/test_shard_queue.cpp`:

```cpp
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/loop>
#include <aloe/wire>
#include <cstddef>
#include <optional>
#include <utility>

#include <gtest/gtest.h>

namespace {

    constexpr aloe::wire::MacAddress mac{0x02, 0, 0, 0, 0, 0x01};

    struct QueueFixture : ::testing::Test {
        aloe::fabric::Fabric fabric;
        aloe::fabric::Port& port = fabric.add_port({.mac = mac, .pool_size = 4});
        aloe::loop::ShardCounters counters;
        aloe::loop::ShardQueue<aloe::fabric::Port> queue{port, 0, 4, counters};

        /// A packet of the queue holding a frame to the port's own MAC.
        std::optional<aloe::fabric::Packet> frame_packet() {
            auto packet = queue.allocate();
            if (packet) {
                EXPECT_TRUE(aloe::frames::fill(
                    *packet,
                    aloe::frames::ethernet_frame(mac, mac, aloe::frames::ethertype_experimental, aloe::frames::pattern(20))));
            }
            return packet;
        }
    };

}  // namespace

TEST_F(QueueFixture, ReclaimReachesTheDevicesQueue) {
    auto lender = frame_packet();
    ASSERT_TRUE(lender.has_value());
    ASSERT_TRUE(queue.transmit(lender->lend()));
    EXPECT_TRUE(lender->lent()) << "in the ring";
    EXPECT_EQ(queue.flush(), 1);
    EXPECT_TRUE(lender->lent()) << "in the device's in-flight list";
    EXPECT_EQ(queue.reclaim(), 1);
    EXPECT_FALSE(lender->lent());
}

TEST_F(QueueFixture, DiscardReturnsALoan) {
    auto lender = frame_packet();
    ASSERT_TRUE(lender.has_value());
    ASSERT_TRUE(queue.transmit(lender->lend()));
    queue.discard();
    EXPECT_FALSE(lender->lent()) << "a dropped handle returns the loan at once";
    EXPECT_EQ(counters.transmit_refused, 1);
}
```

`tests/unit_tests/common/loop/CMakeLists.txt` gains:

```cmake
##############################################################################
# Test Loop: the shard queue over a fabric port, for the loan's reclaim
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Loop.Queue
        test_shard_queue.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Loop.Queue
        PRIVATE
        Aloe::Common::Loop
        Aloe::Fixtures::Fabric
        Aloe::Fixtures::Frames
        ${TEST_LIBS}
)
##############################################################################
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Fabric Aloe.Tests.Unit.Loop.Queue`
Expected: FAIL to compile, `no member named 'reclaim'` in `ShardQueue` and in the conformance suite.

- [ ] **Step 3: Extend the concepts**

`packet.hpp`: add to the `IsPacket` requires-expression, after `set_tx`:

```cpp
                           { packet.lend() } -> std::same_as<P>;
                           { const_packet.lent() } -> std::same_as<bool>;
```

and to the class comment:

```
 * The loan: `lend()` returns a second handle over the same buffer, for the device to transmit, and
 * `lent()` is true while that handle is alive, in a transmit ring, in the driver, or dropped nowhere
 * yet. The lender keeps its handle and never gives it up, so there is no ticket to redeem. The
 * contract: `lend()` on a packet that is not empty and not lent, after the frame is complete and
 * `set_tx` has run, one handle at a time; while lent, the lender reads (`data()`, `size()`, `rx()`,
 * `tx()`) and never grows, trims or sets metadata, and neither does anyone holding the handle; the
 * handle is transmitted or dropped, and dropping it untransmitted returns the loan at once; dropping
 * the lender while lent is allowed, and the buffer is freed exactly once when the last handle goes.
 * A device may complete the checksums it was asked to fill inside the lent buffer; the size, the
 * offsets and every other byte stay as lent. `lent()` is a poll: no backend has an event for the return.
```

`device.hpp`: add to the `IsDevice` requires-expression, after `transmit`: `{ device.reclaim(queue) } -> std::same_as<std::size_t>;` and to the contract list in the class comment: "`reclaim(queue)` asks the driver to free transmitted packets it still holds and returns how many it let go of; on a device whose `capabilities().tx_reclaim` is false it returns zero and does nothing. Queue thread, like every per-queue operation."

- [ ] **Step 4: Add `ShardQueue::reclaim` and update the fakes**

`shard_queue.hpp`, after `discard`:

```cpp
        /// Asks the device to free finished transmits on this queue; how many it let go of. Zero on a device without the call.
        [[nodiscard]] std::size_t reclaim() noexcept {
            core::force_non_const(this);  // the stack's mutable view of the device, though only the pointee is written
            return device_->reclaim(queue_);
        }
```

`tcp_test_device.hpp`, after `transmit`: `[[nodiscard]] std::size_t reclaim(const std::uint16_t queue) const noexcept { return port_->reclaim(queue); }`.
`stamped_device.hpp`, after `transmit`: `[[nodiscard]] std::size_t reclaim(const std::uint16_t queue) noexcept { return device_->reclaim(queue); }`.
`test_shard.cpp`'s `Throttled` and `test_net_send.cpp`'s `StuckPort`: the same forwarding member (`StuckPort` calls `aloe::core::force_non_const(this)` first, as its `allocate` does).
`test_concepts.cpp`: `FakePacket` gains `bool lent_ = false;`, `[[nodiscard]] FakePacket lend() noexcept { lent_ = true; return FakePacket{}; }` and `[[nodiscard]] bool lent() const noexcept { return lent_; }`; `FakeDevice` gains `[[nodiscard]] static std::size_t reclaim(std::uint16_t /*queue*/) noexcept { return 0; }`. The existing `static_assert`s are the test.

- [ ] **Step 5: Run everything**

Run: `cmake --build --preset debug && ctest --preset debug`
Expected: PASS: the conformance suite on the fabric (unit) and on the ring (integration), the new loop binary, the TCP, net and runtime tests unchanged. Then `cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan` (benchmarks off there) and `cmake --build --preset debug --target Aloe.Benchmarks.TickToTrade` for `StampedDevice`.

- [ ] **Step 6: Commit**

```bash
git add common/device common/loop/queue/shard_queue.hpp tests/shared tests/unit_tests/common benchmarks/tick_to_trade/stamped_device.hpp
git commit -m "feat(device): the loan and the reclaim in the concepts, covered by the conformance suite"
```

---

## Task 6: Scripted impairment and the fabric's clock

**Files:**
- Create: `fixtures/fabric/fabric/impairment.hpp`
- Modify: `fixtures/fabric/fabric/fabric.hpp`, `fixtures/fabric/fabric/fabric.cpp`, `fixtures/fabric/export/aloe/fabric`, `fixtures/fabric/CMakeLists.txt`
- Create: `tests/unit_tests/fixtures/fabric/test_fabric_impairment.cpp`
- Modify: `tests/unit_tests/fixtures/fabric/test_fabric_threads.cpp`, `tests/unit_tests/fixtures/fabric/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 4's `Fabric::advance`, `now`, the port's locks.
- Produces, in `impairment.hpp`:

```cpp
namespace aloe::fabric {
    enum class Action : std::uint8_t { Deliver, Drop, Duplicate, Reorder, Delay };
    struct Verdict { Action action = Action::Deliver; core::Duration delay{}; };
    struct Egress { std::uint64_t index = 0; std::span<const std::byte> frame; };
    struct Impairment {
        double loss = 0; double duplicate = 0; double reorder = 0;
        core::Duration delay{}; core::Duration jitter{};
        std::uint64_t seed = 1;
        std::function<std::optional<Verdict>(const Egress&)> script;
    };
    struct ImpairmentCounters {
        std::uint64_t dropped = 0, duplicated = 0, reordered = 0, delayed = 0;
        friend constexpr bool operator==(const ImpairmentCounters&, const ImpairmentCounters&) noexcept = default;
    };
}
```

  and on `Port`: `void set_impairment(const Impairment& impairment)`; `[[nodiscard]] ImpairmentCounters impairment_counters() const noexcept`. `Fabric::advance(now)` delivers due delayed frames in `(due, sequence)` order, releases reorder holds, then finishes loans; an earlier stamp than the last changes nothing.

- [ ] **Step 1: Write the failing tests**

`tests/unit_tests/fixtures/fabric/test_fabric_impairment.cpp`:

```cpp
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/wire>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using aloe::fabric::Action;
    using aloe::fabric::Egress;
    using aloe::fabric::Impairment;
    using aloe::fabric::Packet;
    using aloe::fabric::Verdict;
    using TimePoint = aloe::core::TimePoint;

    constexpr aloe::wire::MacAddress alpha{0x02, 0, 0, 0, 0, 0x0a};
    constexpr aloe::wire::MacAddress beta{0x02, 0, 0, 0, 0, 0x0b};

    /// A fabric with a source, a sink, and a way to send numbered frames and read the numbers back.
    struct Link : ::testing::Test {
        aloe::fabric::Fabric fabric;
        aloe::fabric::Port& source = fabric.add_port({.mac = alpha, .pool_size = 8, .queue_depth = 1024});
        aloe::fabric::Port& sink   = fabric.add_port({.mac = beta, .pool_size = 8, .queue_depth = 1024});

        void send(const std::uint8_t number) {
            auto packet = source.allocate(0);
            ASSERT_TRUE(packet.has_value());
            ASSERT_TRUE(aloe::frames::fill(
                *packet, aloe::frames::ethernet_frame(beta, alpha, aloe::frames::ethertype_experimental, aloe::frames::pattern(1, number))));
            std::array<Packet, 1> burst{std::move(*packet)};
            ASSERT_EQ(source.transmit(0, burst), 1);
        }

        void send_many(const std::uint8_t count) {
            for (std::uint8_t number = 0; number < count; ++number) {
                send(number);
            }
        }

        /// The frame numbers the sink holds, in arrival order.
        std::vector<std::uint8_t> received() {
            std::vector<std::uint8_t> numbers;
            std::array<Packet, 16> burst;
            for (std::size_t count = sink.receive(0, burst); count > 0; count = sink.receive(0, burst)) {
                for (std::size_t index = 0; index < count; ++index) {
                    numbers.push_back(std::to_integer<std::uint8_t>(burst[index].data().back()));
                    burst[index] = Packet{};
                }
            }
            return numbers;
        }
    };

    /// The verdict for one egress index and nothing for the rest.
    Impairment only(const std::uint64_t index, const Verdict verdict) {
        Impairment impairment;
        impairment.script = [index, verdict](const Egress& egress) -> std::optional<Verdict> {
            if (egress.index == index) {
                return verdict;
            }
            return std::nullopt;
        };
        return impairment;
    }

}  // namespace

TEST_F(Link, NoImpairmentChangesNothing) {
    source.set_impairment(Impairment{});
    send_many(3);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{0, 1, 2}));
    EXPECT_EQ(source.impairment_counters(), aloe::fabric::ImpairmentCounters{});
    EXPECT_EQ(source.counters(0).transmitted, 3);
}

TEST_F(Link, LossDropsSomeFramesAndCountsThem) {
    source.set_impairment({.loss = 0.5, .seed = 7});
    send_many(100);
    const auto got = received();
    EXPECT_GT(got.size(), 20U);
    EXPECT_LT(got.size(), 80U);
    EXPECT_EQ(source.impairment_counters().dropped, 100 - got.size());
    EXPECT_EQ(source.counters(0).transmitted, 100) << "a dropped frame still left the port";
    EXPECT_TRUE(std::ranges::is_sorted(got)) << "loss keeps the order of what survives";
}

TEST_F(Link, TheSameSeedDropsTheSameFrames) {
    source.set_impairment({.loss = 0.3, .seed = 11});
    send_many(50);
    const auto first = received();
    Link second;
    second.source.set_impairment({.loss = 0.3, .seed = 11});
    second.send_many(50);
    EXPECT_EQ(second.received(), first);
    Link third;
    third.source.set_impairment({.loss = 0.3, .seed = 12});
    third.send_many(50);
    EXPECT_NE(third.received(), first) << "another seed, another run";
}

TEST_F(Link, DuplicateDeliversTwiceInARow) {
    source.set_impairment({.duplicate = 1.0});
    send_many(2);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{0, 0, 1, 1}));
    EXPECT_EQ(source.impairment_counters().duplicated, 2);
}

TEST_F(Link, AReorderedFrameFollowsTheNextOne) {
    source.set_impairment(only(0, Verdict{.action = Action::Reorder}));
    send(0);
    EXPECT_TRUE(received().empty()) << "held";
    send(1);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{1, 0}));
    EXPECT_EQ(source.impairment_counters().reordered, 1);
}

TEST_F(Link, AReorderHoldIsReleasedByAdvanceWhenNothingFollows) {
    source.set_impairment(only(0, Verdict{.action = Action::Reorder}));
    send(0);
    EXPECT_TRUE(received().empty());
    fabric.advance(TimePoint{} + 1ms);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{0}));
}

TEST_F(Link, ASecondHoldDeliversTheFirst) {
    source.set_impairment({.reorder = 1.0});
    send_many(3);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{0, 1})) << "each new hold lets the previous one go";
    fabric.advance(TimePoint{} + 1ms);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{2}));
    EXPECT_EQ(source.impairment_counters().reordered, 3);
}

TEST_F(Link, DelayDeliversAtAdvanceAndNotBefore) {
    fabric.advance(TimePoint{} + 100ms);
    source.set_impairment({.delay = 10ms});
    send(0);
    EXPECT_TRUE(received().empty());
    fabric.advance(TimePoint{} + 105ms);
    EXPECT_TRUE(received().empty()) << "due at 110 ms";
    fabric.advance(TimePoint{} + 110ms);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{0}));
    EXPECT_EQ(source.impairment_counters().delayed, 1);
}

TEST_F(Link, EqualDelaysKeepTheOrder) {
    source.set_impairment({.delay = 10ms});
    send_many(20);
    fabric.advance(TimePoint{} + 10ms);
    const auto got = received();
    EXPECT_EQ(got.size(), 20U);
    EXPECT_TRUE(std::ranges::is_sorted(got));
}

TEST_F(Link, JitterStaysWithinItsWindow) {
    source.set_impairment({.delay = 10ms, .jitter = 5ms, .seed = 3});
    send_many(20);
    fabric.advance(TimePoint{} + 9ms);
    EXPECT_TRUE(received().empty());
    fabric.advance(TimePoint{} + 15ms);
    const auto got = received();
    EXPECT_EQ(got.size(), 20U);
    EXPECT_FALSE(std::ranges::is_sorted(got)) << "jitter reorders";
    EXPECT_EQ(std::set<std::uint8_t>(got.begin(), got.end()).size(), 20U);
}

TEST_F(Link, TheScriptOverridesTheModel) {
    Impairment impairment = only(1, Verdict{});
    impairment.loss       = 1.0;
    source.set_impairment(impairment);
    send_many(3);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{1}));
    EXPECT_EQ(source.impairment_counters().dropped, 2);
}

TEST_F(Link, AScriptedDelayTakesItsOwnDuration) {
    source.set_impairment(only(0, Verdict{.action = Action::Delay, .delay = 20ms}));
    send_many(2);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{1}));
    fabric.advance(TimePoint{} + 10ms);
    EXPECT_TRUE(received().empty());
    fabric.advance(TimePoint{} + 20ms);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{0}));
}

TEST_F(Link, AScriptedDropIsCounted) {
    source.set_impairment(only(0, Verdict{.action = Action::Drop}));
    send_many(2);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{1}));
    EXPECT_EQ(source.impairment_counters().dropped, 1);
}

TEST_F(Link, CountersAgreeWithTheReceiver) {
    source.set_impairment({.loss = 0.3, .duplicate = 0.3, .seed = 5});
    send_many(200);
    const auto got                           = received();
    const aloe::fabric::ImpairmentCounters c = source.impairment_counters();
    EXPECT_EQ(got.size(), 200 - c.dropped + c.duplicated);
    EXPECT_GT(c.dropped, 0U);
    EXPECT_GT(c.duplicated, 0U);
}

TEST_F(Link, AdvanceWithAnEarlierStampChangesNothing) {
    fabric.advance(TimePoint{} + 10ms);
    source.set_impairment({.delay = 5ms});
    send(0);
    fabric.advance(TimePoint{} + 3ms);
    EXPECT_EQ(fabric.now(), TimePoint{} + 10ms);
    EXPECT_TRUE(received().empty());
    fabric.advance(TimePoint{} + 15ms);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{0}));
}
```

In `tests/unit_tests/fixtures/fabric/test_fabric_threads.cpp`, add a second test, `FramesFromManyThreadsArriveExactlyOnceUnderDelay`, which is the existing test with these changes: every source port calls `set_impairment({.delay = 1ms})` before the threads start; a `std::jthread ticker` runs `TimePoint now{}; while (producers_done.load() != producers) { now += 1ms; fabric.advance(now); std::this_thread::yield(); } fabric.advance(now + 2ms); delivered_all.store(true);` with `std::atomic<bool> delivered_all{false}`; the consumers read `delivered_all` instead of `producers_done == producers` as their stop condition; the assertions at the end are the same. Factor the body into a helper taking `const std::optional<aloe::fabric::Impairment>&` if that keeps the file readable; both tests must stay.

Add `test_fabric_impairment.cpp` to `Aloe.Tests.Unit.Fabric` in the CMake file.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Fabric`
Expected: FAIL to compile, `impairment.hpp` has no `Impairment`, `set_impairment` missing.

- [ ] **Step 3: Write `impairment.hpp` and extend the port and the fabric**

`fixtures/fabric/fabric/impairment.hpp`: the types of the Interfaces block, each documented in one line as the spec's model section says (a drop still counts as transmitted; a duplicate is delivered twice in a row; a reorder is held until the port's next frame has been delivered or the next `advance`; a delay is measured from the fabric's current stamp; the script's verdict wins; the model draws in the order loss, duplicate, reorder, delay). Add it to the umbrella and to the `${FABRIC}.Fabric` target's sources.

`fabric.hpp`: `Port` gains the two public members, the private members `Impairment impairment_;`, `std::mt19937_64 engine_;`, `ImpairmentCounters impairment_counters_;`, `std::optional<std::vector<std::byte>> hold_;`, `std::uint64_t egress_index_ = 0;`, `mutable std::mutex impairment_mutex_;`, and the private functions `Verdict judge(std::span<const std::byte> frame)`, `void egress(std::span<const std::byte> frame)`, `void emit(std::span<const std::byte> frame)`, `void release_hold()`, `double draw() noexcept`. `Fabric` gains a private `struct DelayedFrame { core::TimePoint due; std::uint64_t sequence = 0; Port* source = nullptr; std::vector<std::byte> bytes; };`, `std::vector<DelayedFrame> delayed_;`, `std::uint64_t delayed_sequence_ = 0;` and `void delay(Port& source, std::span<const std::byte> frame, core::Duration by)` (friend `Port` calls it).

`fabric.cpp`:

- `set_impairment`: lock `impairment_mutex_`, copy, `engine_.seed(impairment.seed)`.
- `draw()`: the engine's output with the low eleven bits discarded, scaled to `[0, 1)`: `std::ldexp(static_cast<double>(engine_() >> discarded_bits), -mantissa_bits)` with `constexpr unsigned discarded_bits = 11; constexpr int mantissa_bits = 53;` in the anonymous namespace. Never `std::uniform_real_distribution`.
- `judge(frame)`: under `impairment_mutex_`: `Egress egress{.index = egress_index_++, .frame = frame}`; if `impairment_.script` and it returns a verdict, that verdict; else `if (impairment_.loss > 0 && draw() < impairment_.loss) return Drop; if (duplicate > 0 && draw() < duplicate) return Duplicate; if (reorder > 0 && draw() < reorder) return Reorder; if (delay > 0 || jitter > 0) return Delay{delay + jitter * draw()}; return Deliver`. The jitter product: `std::chrono::duration_cast<core::Duration>(impairment_.jitter * draw())`.
- `egress(frame)`: `judge`, then by action: `Deliver` → `emit(frame)`; `Drop` → `++dropped`; `Duplicate` → `emit(frame); emit(frame); ++duplicated`; `Reorder` → `++reordered`, then under the mutex swap the frame into `hold_`, and if there was a previous hold, deliver it (`fabric_.deliver(*this, previous)`) after releasing the mutex; `Delay` → `++delayed; fabric_.delay(*this, frame, verdict.delay)`.
- `emit(frame)`: `fabric_.deliver(*this, frame)`, then `release_hold()`.
- `release_hold()`: under the mutex take `hold_` if present, release the mutex, deliver it.
- `transmit`: replace `fabric_.deliver(*this, frame)` with `egress(frame)`. The counters update stays as it is: a dropped frame counts as transmitted.
- `Fabric::delay(source, frame, by)`: under `mutex_`, push `{now_ + by, delayed_sequence_++, &source, bytes}`.
- `Fabric::advance(now)`: under `mutex_`, return if `now < now_`; set `now_`; move every entry with `due <= now_` into a local vector; release the lock; sort the local vector by `(due, sequence)`; `deliver(*entry.source, entry.bytes)` for each; then for every port `release_hold()`; then the loan finishing of Task 4. Never deliver while holding `mutex_` or `impairment_mutex_`: `deliver` takes destination queue locks.
- `impairment_counters()`: under the mutex, a copy.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Fabric Aloe.Tests.Unit.Fabric.Threads && ctest --preset debug -R Fabric`
Expected: PASS. Then `cmake --build --preset tsan --target Aloe.Tests.Unit.Fabric.Threads && ./build/tsan/tests/unit_tests/fixtures/fabric/Aloe.Tests.Unit.Fabric.Threads`
Expected: PASS with no race report; a report here means a lock was dropped around the hold or the delayed queue.

- [ ] **Step 5: Commit**

```bash
git add fixtures/fabric tests/unit_tests/fixtures/fabric
git commit -m "feat(fabric): scripted impairment on a port's egress, and delayed frames on the clock step"
```

---

## Task 7: The harnesses step the fabric; two stacks under impairment

**Files:**
- Modify: `tests/shared/tcp/tcp_fixture.hpp`
- Modify: `tests/unit_tests/common/tcp/test_tcp_two_stacks.cpp`

**Interfaces:**
- Consumes: Task 6's `Fabric::advance`, `Port::set_impairment`, `impairment_counters`, `Egress`, `Verdict`, `Action`.
- Produces: `TcpHarness(fabric::Fabric& fabric, fabric::Port& harness, Device& device, const tcp::TcpConfig& config = {}, const net::Ipv4Config& ip_config = stack_config(), std::size_t ring_capacity = 16)` whose `advance(by)` also calls `fabric.advance(now_)`; `TcpFixture` and `TcpRefusingFixture` pass `fabric` first. The six TCP unit test files compile unchanged.

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit_tests/common/tcp/test_tcp_two_stacks.cpp` (add `#include <optional>` and the fabric's names):

```cpp
TEST_P(TcpTwoStacks, ALostSynIsRetransmittedAndTheConnectionEstablishes) {
    client_port_.set_impairment({.script = [](const aloe::fabric::Egress& egress) -> std::optional<aloe::fabric::Verdict> {
        if (egress.index == 0) {
            return aloe::fabric::Verdict{.action = aloe::fabric::Action::Drop};
        }
        return std::nullopt;
    }});
    server_.ip.learn(client_ip, client_mac, now_);
    client_.ip.learn(server_ip, server_mac, now_);
    ASSERT_TRUE(server_.tcp.listen(7).has_value());
    const auto session = client_.tcp.connect({.address = server_ip, .port = 7}, now_);
    ASSERT_TRUE(session.has_value());
    Tcp::ConnectionType* client = *session;
    client_.flush(now_);
    EXPECT_EQ(client_port_.impairment_counters().dropped, 1) << "the wire ate the first SYN";

    Tcp::ConnectionType* server = nullptr;
    for (int tick = 0; tick < 15 && client->state() != aloe::tcp::State::Established; ++tick) {
        now_ += 100ms;
        fabric_.advance(now_);
        client_.receive(now_);
        while (client_.tcp.poll_event()) {}
        client_.flush(now_);
        server_.receive(now_);
        while (const auto event = server_.tcp.poll_event()) {
            if (event->events.accepted()) {
                server = event->connection;
            }
        }
        server_.flush(now_);
    }
    EXPECT_EQ(client->state(), aloe::tcp::State::Established);
    ASSERT_NE(server, nullptr);
    EXPECT_EQ(client_.tcp.counters().retransmits, 1) << "the SYN again, one second after the first";
    EXPECT_EQ(client_port_.impairment_counters().dropped, 1) << "only the first SYN was lost";
    client->release();
    server->release();
}

TEST_P(TcpTwoStacks, ADuplicatedSegmentIsCountedAndReadOnce) {
    Tcp::ConnectionType* client = nullptr;
    Tcp::ConnectionType* server = establish(client);
    ASSERT_NE(server, nullptr);
    client_port_.set_impairment({.duplicate = 1.0});
    const auto payload = aloe::frames::pattern(100);
    ASSERT_EQ(client->send(payload), 100U);
    exchange();
    client_port_.set_impairment({});
    EXPECT_EQ(client_port_.impairment_counters().duplicated, 1);
    EXPECT_EQ(server_.tcp.counters().dropped_duplicate, 1);
    EXPECT_EQ(server->unread().size(), 100U) << "read once";
    client->release();
    server->release();
}

TEST_P(TcpTwoStacks, DelayedSegmentsArriveInOrderAfterTheStep) {
    Tcp::ConnectionType* client = nullptr;
    Tcp::ConnectionType* server = establish(client);
    ASSERT_NE(server, nullptr);
    client_port_.set_impairment({.delay = 5ms});
    for (std::uint8_t seed = 0; seed < 3; ++seed) {
        ASSERT_EQ(client->send(aloe::frames::pattern(1000, seed)), 1000U);
    }
    client_.flush(now_);
    EXPECT_EQ(client_port_.impairment_counters().delayed, 3);

    now_ += 1ms;
    fabric_.advance(now_);
    server_.receive(now_);
    EXPECT_EQ(server->unread().size(), 0U) << "nothing before the delay is due";

    now_ += 5ms;
    fabric_.advance(now_);
    server_.receive(now_);
    EXPECT_EQ(server->unread().size(), 3000U);
    EXPECT_EQ(server_.tcp.counters().dropped_out_of_order, 0);
    while (server_.tcp.poll_event()) {}
    server_.flush(now_);
    client_port_.set_impairment({});
    client->release();
    server->release();
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Tcp && ./build/debug/tests/unit_tests/common/tcp/Aloe.Tests.Unit.Tcp --gtest_filter='*TcpTwoStacks.A*:*TcpTwoStacks.Delayed*'`
Expected: `DelayedSegmentsArriveInOrderAfterTheStep` passes already (the test steps the fabric itself) or fails only if `establish` left the fabric's stamp behind; the other two pass too, since they step the fabric themselves. If all three pass, that is fine: the failing part of this task is Step 3's harness change, verified by the existing TCP tests still passing after it. Record what happened.

- [ ] **Step 3: Step the fabric from the harnesses**

`tcp_fixture.hpp`: `TcpHarness` gains a first constructor parameter `fabric::Fabric& fabric` stored in `fabric::Fabric* fabric_`; `advance(by)` becomes:

```cpp
        void advance(const core::Duration by) {
            now_ += by;
            tcp_->process({}, now_);
            std::ignore = wheel_.advance(now_);
            fabric_->advance(now_);  // delayed frames and finished loans, for the tests that use them
        }
```

`TcpFixture`: `TcpHarness{fabric, harness, port}`; `TcpRefusingFixture`: `TcpHarness{fabric, harness, device, {}, stack_config(), 1}`. The class comment of `TcpHarness` gains "`advance` also steps the fabric".

`test_tcp_two_stacks.cpp`: after every `now_ += 1ms;` in `establish`, `exchange` and `EchoAcrossMssAndClose`, add `fabric_.advance(now_);`.

- [ ] **Step 4: Run the TCP tests**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Tcp Aloe.Tests.Unit.Stream && ctest --preset debug -R 'Tcp|Stream'`
Expected: PASS, every existing TCP test unchanged in outcome, plus the three new ones.

- [ ] **Step 5: Commit**

```bash
git add tests/shared/tcp/tcp_fixture.hpp tests/unit_tests/common/tcp/test_tcp_two_stacks.cpp
git commit -m "test(tcp): step the fabric from the harnesses; two stacks under loss, duplication and delay"
```

---

## Task 8: Raw TCP options on `frames::TcpSpec`

**Files:**
- Modify: `fixtures/frames/frames/tcp_frames.hpp`
- Test: `tests/unit_tests/fixtures/frames/test_tcp_frames.cpp`

**Interfaces:**
- Produces: `std::vector<std::byte> TcpSpec::options` (written verbatim after the header when non-empty; `mss` must then be absent; a multiple of 4, at most 40 bytes, asserted); `[[nodiscard]] inline std::span<const std::byte> tcp_options_of(const ParsedFrame& frame)` (the bytes between the fixed header and the data offset, empty for a frame that is not TCP). Tasks 11 and 13 build inbound frames and read outbound options with them.

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit_tests/fixtures/frames/test_tcp_frames.cpp`:

```cpp
TEST(TcpFrames, RawOptionsAreWrittenVerbatimAndReadBack) {
    TcpSpec spec = peer_to_stack();
    spec.mss.reset();
    spec.options = bytes({2, 4, 0x05, 0xb4, 1, 1, 4, 2});  // mss 1460, nop, nop, sackOK
    spec.flags   = TcpFlags{TcpFlag::Syn};
    const auto frame  = aloe::frames::tcp_frame(spec, {});
    const auto parsed = aloe::frames::parse_frame(frame).value();
    ASSERT_TRUE(parsed.tcp.has_value());
    EXPECT_EQ(parsed.tcp->data_offset, 28);
    const auto options = aloe::frames::tcp_options_of(parsed);
    EXPECT_EQ(std::vector<std::byte>(options.begin(), options.end()), spec.options);
    EXPECT_EQ(aloe::frames::l4_checksum_residue(*parsed.ipv4, parsed.l4), 0U);
    const auto known = aloe::wire::TcpOptions::parse(options);
    ASSERT_TRUE(known.has_value());
    EXPECT_EQ(known->mss, 1460);
}

TEST(TcpFrames, NoOptionsMeansAnEmptyOptionSpan) {
    TcpSpec spec = peer_to_stack();
    spec.mss.reset();
    const auto parsed = aloe::frames::parse_frame(aloe::frames::tcp_frame(spec, text("x"))).value();
    EXPECT_EQ(parsed.tcp->data_offset, 20);
    EXPECT_TRUE(aloe::frames::tcp_options_of(parsed).empty());
    EXPECT_EQ(aloe::frames::tcp_payload(parsed), std::span<const std::byte>{text("x")});
}
```

(The second test compares spans; if the file lacks a span comparison helper, compare `std::vector<std::byte>(payload.begin(), payload.end())` with `text("x")`.)

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Frames`
Expected: FAIL to compile, `no member named 'options'`.

- [ ] **Step 3: Implement**

`TcpSpec` gains, after `mss`: `std::vector<std::byte> options;  ///< Raw option bytes, written as they are when non-empty; then `mss` stays absent.` In `tcp_segment`: `const std::size_t option_bytes = spec.options.empty() ? (spec.mss ? wire::TcpOptions::mss_size : 0) : spec.options.size();` with `assert(spec.options.empty() || !spec.mss)` and `assert(option_bytes % 4 == 0 && option_bytes <= wire::TcpHeader::max_size - wire::TcpHeader::size)`; the header is `wire::TcpHeader::size + option_bytes`; after the header write, copy `spec.options` into `segment` at offset 20 when non-empty, else write the MSS option as today. After `tcp_payload`:

```cpp
    /// The option bytes between the fixed header and the data offset of a parsed frame; empty when the frame is not TCP.
    [[nodiscard]] inline std::span<const std::byte> tcp_options_of(const ParsedFrame& frame) {
        if (!frame.tcp) {
            return {};
        }
        return std::span<const std::byte>{frame.l4}.subspan(wire::TcpHeader::size,
                                                            frame.tcp->data_offset - wire::TcpHeader::size);
    }
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Frames && ./build/debug/tests/unit_tests/fixtures/frames/Aloe.Tests.Unit.Frames`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add fixtures/frames/frames/tcp_frames.hpp tests/unit_tests/fixtures/frames/test_tcp_frames.cpp
git commit -m "feat(frames): raw TCP options on a spec, and the option bytes of a parsed frame"
```

---

## Task 9: The `scenario` module: the script model and the parser for times and packet lines

**Files:**
- Create: `fixtures/scenario/CMakeLists.txt`, `fixtures/scenario/export/aloe/scenario`
- Create: `fixtures/scenario/script/script.hpp`, `fixtures/scenario/script/parse_script.hpp`, `fixtures/scenario/script/parse_script.cpp`
- Modify: `fixtures/CMakeLists.txt` (add `scenario`), `tests/unit_tests/fixtures/CMakeLists.txt` (add `scenario`)
- Create: `tests/unit_tests/fixtures/scenario/CMakeLists.txt`, `tests/unit_tests/fixtures/scenario/test_parse_script.cpp`

**Interfaces:**
- Consumes: `wire::TcpFlags`, `wire::TcpFlag`, `core::Duration`.
- Produces, in `aloe::scenario` (`script.hpp`):

```cpp
    struct TimeSpec {
        enum class Kind : std::uint8_t { Relative, Absolute, Any };
        Kind kind = Kind::Relative;
        core::Duration low{};   ///< The window's start; `+t` and `t` have low == high. Unused for Any.
        core::Duration high{};
        friend bool operator==(const TimeSpec&, const TimeSpec&) = default;
    };
    enum class Direction : std::uint8_t { Inbound, Outbound };
    struct SackBlock { std::uint32_t first = 0; std::uint32_t last = 0; friend bool operator==(const SackBlock&, const SackBlock&) = default; };
    struct TcpOption {
        enum class Kind : std::uint8_t { Nop, Eol, Mss, WindowScale, SackPermitted, Sack, Timestamps };
        Kind kind = Kind::Nop;
        std::uint16_t mss = 0;            ///< Mss.
        std::uint8_t shift = 0;           ///< WindowScale.
        std::uint32_t value = 0;          ///< Timestamps: val.
        std::uint32_t echo = 0;           ///< Timestamps: ecr.
        std::vector<SackBlock> blocks;    ///< Sack, in relative numbers.
        friend bool operator==(const TcpOption&, const TcpOption&) = default;
    };
    struct Options { bool any = false; std::vector<TcpOption> list; friend bool operator==(const Options&, const Options&) = default; };  // `any` is `<...>`
    struct PacketLine {
        Direction direction = Direction::Inbound;
        wire::TcpFlags flags{};
        std::uint32_t first = 0; std::uint32_t last = 0; std::uint32_t length = 0;  ///< Relative sequence numbers.
        std::optional<std::uint32_t> ack; std::optional<std::uint16_t> window; std::optional<std::uint16_t> urgent;
        Options options;
        friend bool operator==(const PacketLine&, const PacketLine&) = default;
    };
    struct Argument {
        enum class Kind : std::uint8_t { Integer, Ellipsis, Word, Bracketed, String };
        Kind kind = Kind::Ellipsis; std::int64_t integer = 0; std::string text;
        friend bool operator==(const Argument&, const Argument&) = default;
    };
    struct CallLine { std::string name; std::vector<Argument> arguments; std::int64_t result = 0; std::string error; std::string note; friend bool operator==(const CallLine&, const CallLine&) = default; };
    struct Event { TimeSpec time; std::size_t line = 0; std::variant<PacketLine, CallLine> action; };
    struct Script { std::optional<core::Duration> tolerance; std::vector<Event> events; };
```

  and (`parse_script.hpp`): `struct ParseError { std::size_t line = 0; std::size_t column = 0; std::string message; };` and `[[nodiscard]] std::expected<Script, ParseError> parse_script(std::string_view text);`. In this task a call line parses as `ParseError{"call lines are not parsed yet"}`; Task 10 replaces that. The CMake targets: `Aloe.Fixtures.Scenario.Script` (STATIC: `script/`), the combined `Aloe.Fixtures.Scenario` with alias `Aloe::Fixtures::Scenario`; the test target `Aloe.Tests.Unit.Scenario`.

- [ ] **Step 1: Write the failing tests**

`tests/unit_tests/fixtures/scenario/test_parse_script.cpp`:

```cpp
#include <aloe/scenario>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using aloe::scenario::Direction;
    using aloe::scenario::PacketLine;
    using aloe::scenario::parse_script;
    using aloe::scenario::Script;
    using aloe::scenario::TcpOption;
    using aloe::scenario::TimeSpec;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;

    const PacketLine& packet(const Script& script, const std::size_t index) {
        return std::get<PacketLine>(script.events.at(index).action);
    }

}  // namespace

TEST(ParseScript, ParsesEveryTimeForm) {
    const auto script = parse_script("+0 < S 0:0(0) win 65535 <mss 1460>\n"
                                     "+.01 > S. 0:0(0) ack 1 <mss 1460>\n"
                                     "0.5 < . 1:1(0) ack 1 win 257\n"
                                     "+1~+2 > . 1:1(0) ack 2\n"
                                     "3~4.25 > F. 1:1(0) ack 2\n"
                                     "* > R 2:2(0)\n");
    ASSERT_TRUE(script.has_value()) << script.error().message;
    ASSERT_EQ(script->events.size(), 6);
    EXPECT_EQ(script->events[0].time, (TimeSpec{.kind = TimeSpec::Kind::Relative, .low = 0s, .high = 0s}));
    EXPECT_EQ(script->events[1].time, (TimeSpec{.kind = TimeSpec::Kind::Relative, .low = 10ms, .high = 10ms}));
    EXPECT_EQ(script->events[2].time, (TimeSpec{.kind = TimeSpec::Kind::Absolute, .low = 500ms, .high = 500ms}));
    EXPECT_EQ(script->events[3].time, (TimeSpec{.kind = TimeSpec::Kind::Relative, .low = 1s, .high = 2s}));
    EXPECT_EQ(script->events[4].time, (TimeSpec{.kind = TimeSpec::Kind::Absolute, .low = 3s, .high = 4250ms}));
    EXPECT_EQ(script->events[5].time.kind, TimeSpec::Kind::Any);
    EXPECT_EQ(script->events[0].line, 1);
    EXPECT_EQ(script->events[5].line, 6);
    EXPECT_FALSE(script->tolerance.has_value());
}

TEST(ParseScript, ParsesFlagsNumbersAndFields) {
    const auto script = parse_script("+0 > P. 1:1001(1000) ack 1 win 46720 urg 7\n"
                                     "+0 < . 1:1(0) ack 1001\n"
                                     "+0 > R 5:5(0)\n"
                                     "+0 < - 1:1(0)\n"
                                     "+0 < F. 1001:1001(0) ack 1 win 257\n"
                                     "+0 < S 0:0(0) win 65535\n");
    ASSERT_TRUE(script.has_value()) << script.error().message;
    const PacketLine& data = packet(*script, 0);
    EXPECT_EQ(data.direction, Direction::Outbound);
    EXPECT_EQ(data.flags, (TcpFlags{TcpFlag::Psh, TcpFlag::Ack}));
    EXPECT_EQ(data.first, 1);
    EXPECT_EQ(data.last, 1001);
    EXPECT_EQ(data.length, 1000);
    EXPECT_EQ(data.ack, 1);
    EXPECT_EQ(data.window, 46720);
    EXPECT_EQ(data.urgent, 7);
    EXPECT_FALSE(data.options.any);
    EXPECT_TRUE(data.options.list.empty());

    EXPECT_EQ(packet(*script, 1).direction, Direction::Inbound);
    EXPECT_EQ(packet(*script, 1).flags, (TcpFlags{TcpFlag::Ack}));
    EXPECT_FALSE(packet(*script, 1).window.has_value());
    EXPECT_EQ(packet(*script, 2).flags, (TcpFlags{TcpFlag::Rst}));
    EXPECT_FALSE(packet(*script, 2).ack.has_value());
    EXPECT_EQ(packet(*script, 3).flags, TcpFlags{});
    EXPECT_EQ(packet(*script, 4).flags, (TcpFlags{TcpFlag::Fin, TcpFlag::Ack}));
    EXPECT_EQ(packet(*script, 5).flags, (TcpFlags{TcpFlag::Syn}));
}

TEST(ParseScript, ParsesEveryOption) {
    const auto script = parse_script(
        "+0 < S 0:0(0) win 65535 <mss 1000,sackOK,nop,nop,nop,wscale 7,TS val 100 ecr 200,sack 1:2,3:4,eol>\n"
        "+0 > S. 0:0(0) ack 1 <...>\n");
    ASSERT_TRUE(script.has_value()) << script.error().message;
    const auto& options = packet(*script, 0).options;
    EXPECT_FALSE(options.any);
    ASSERT_EQ(options.list.size(), 9);
    EXPECT_EQ(options.list[0], (TcpOption{.kind = TcpOption::Kind::Mss, .mss = 1000}));
    EXPECT_EQ(options.list[1].kind, TcpOption::Kind::SackPermitted);
    EXPECT_EQ(options.list[2].kind, TcpOption::Kind::Nop);
    EXPECT_EQ(options.list[5], (TcpOption{.kind = TcpOption::Kind::WindowScale, .shift = 7}));
    EXPECT_EQ(options.list[6], (TcpOption{.kind = TcpOption::Kind::Timestamps, .value = 100, .echo = 200}));
    EXPECT_EQ(options.list[7].kind, TcpOption::Kind::Sack);
    EXPECT_EQ(options.list[7].blocks, (std::vector<aloe::scenario::SackBlock>{{.first = 1, .last = 2}, {.first = 3, .last = 4}}));
    EXPECT_EQ(options.list[8].kind, TcpOption::Kind::Eol);
    EXPECT_TRUE(packet(*script, 1).options.any);
    EXPECT_TRUE(packet(*script, 1).options.list.empty());
}

TEST(ParseScript, SkipsCommentsAndBlankLinesAndKeepsLineNumbers) {
    const auto script = parse_script("// A comment first.\n"
                                     "\n"
                                     "+0 < S 0:0(0) win 65535 <mss 1460>   // the SYN\n"
                                     "   \n"
                                     "// Another.\n"
                                     "+0 > S. 0:0(0) ack 1 <mss 1460>\n");
    ASSERT_TRUE(script.has_value()) << script.error().message;
    ASSERT_EQ(script->events.size(), 2);
    EXPECT_EQ(script->events[0].line, 3);
    EXPECT_EQ(script->events[1].line, 6);
}

TEST(ParseScript, ReportsErrorsWithTheirLine) {
    const auto length = parse_script("+0 < S 0:0(0)\n+0 < P. 1:1001(999) ack 1\n");
    ASSERT_FALSE(length.has_value());
    EXPECT_EQ(length.error().line, 2);
    EXPECT_NE(length.error().message.find("length"), std::string_view::npos);

    const auto shell = parse_script("`../common/defaults.sh`\n+0 < S 0:0(0)\n");
    ASSERT_FALSE(shell.has_value());
    EXPECT_EQ(shell.error().line, 1);
    EXPECT_NE(shell.error().message.find("shell"), std::string_view::npos);

    const auto udp = parse_script("+0 < udp (1000)\n");
    ASSERT_FALSE(udp.has_value());
    EXPECT_NE(udp.error().message.find("not supported"), std::string_view::npos);

    const auto ip_info = parse_script("+0 < [ect0] S 0:0(0)\n");
    ASSERT_FALSE(ip_info.has_value());
    EXPECT_NE(ip_info.error().message.find("not supported"), std::string_view::npos);

    const auto bad_flag = parse_script("+0 < X 0:0(0)\n");
    ASSERT_FALSE(bad_flag.has_value());
    EXPECT_NE(bad_flag.error().message.find("flag"), std::string_view::npos);

    const auto bad_option = parse_script("+0 < S 0:0(0) <mss>\n");
    ASSERT_FALSE(bad_option.has_value());
    EXPECT_EQ(bad_option.error().line, 1);

    const auto no_time = parse_script("< S 0:0(0)\n");
    ASSERT_FALSE(no_time.has_value());
    EXPECT_NE(no_time.error().message.find("time"), std::string_view::npos);

    const auto trailing = parse_script("+0 < S 0:0(0) win 65535 junk\n");
    ASSERT_FALSE(trailing.has_value());
    EXPECT_NE(trailing.error().message.find("junk"), std::string_view::npos);
}

TEST(ParseScript, AnEmptyScriptHasNoEvents) {
    const auto script = parse_script("// nothing\n\n");
    ASSERT_TRUE(script.has_value());
    EXPECT_TRUE(script->events.empty());
}
```

- [ ] **Step 2: Add the build skeleton and run the test to verify it fails**

`fixtures/CMakeLists.txt` gains `add_subdirectory(scenario)`. `fixtures/scenario/CMakeLists.txt`:

```cmake
set(SCENARIO ${FIXTURES}.Scenario)

##############################################################################
# The script language: the model, the parser and the notation
##############################################################################
add_library(${SCENARIO}.Script STATIC
        script/script.hpp
        script/parse_script.hpp
        script/parse_script.cpp
)
target_include_directories(${SCENARIO}.Script PUBLIC
        script/
)
target_link_libraries(${SCENARIO}.Script PUBLIC
        Aloe::Common::Core
        Aloe::Common::Wire
)
##############################################################################

##############################################################################
# Scenario exported library
##############################################################################
add_combined_library(${SCENARIO}
        DIRECTORIES
        export
        SOURCES
        export/aloe/scenario
        LIBRARIES
        ${SCENARIO}.Script
)

add_library(Aloe::Fixtures::Scenario ALIAS ${SCENARIO})
##############################################################################
```

Check the core and wire alias names with `grep -n ALIAS common/core/CMakeLists.txt common/wire/CMakeLists.txt`. `fixtures/scenario/export/aloe/scenario`:

```cpp
#pragma once

/**
 * Public umbrella for the Aloe scenario module: packetdrill-style scripts run against the TCP brick on
 * the fabric. Consumers link Aloe::Fixtures::Scenario and write `#include <aloe/scenario>`.
 */

#include <parse_script.hpp>
#include <script.hpp>
```

`tests/unit_tests/fixtures/CMakeLists.txt` gains `add_subdirectory(scenario)`. `tests/unit_tests/fixtures/scenario/CMakeLists.txt`:

```cmake
##############################################################################
# Test Scenario: the parser, the peer's numbers, the matcher, the harness and the runner
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Scenario
        test_parse_script.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Scenario
        PRIVATE
        Aloe::Fixtures::Scenario
        ${TEST_LIBS}
)
##############################################################################
```

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Scenario`
Expected: FAIL to compile, `parse_script` undeclared (write the headers first if you prefer a link failure; either is the red step).

- [ ] **Step 3: Write `script.hpp` and `parse_script`**

`script.hpp` holds the types of the Interfaces block with a one-line comment each; `parse_script.hpp` declares `ParseError` and `parse_script`. `parse_script.cpp` is a line-by-line parser; the one decision it makes that the tests do not: a private `detail::Cursor` over one line with `skip_spaces`, `peek`, `take_word` (letters, digits, `_`, `|`, `.` for `S.`), `take_number` (an unsigned integer, or a decimal with an optional leading `.`), `take_until(char)`, `at_end` (after stripping a trailing `//` comment), so every error carries the column. The grammar per line:

- Strip a `//` comment; skip an empty line.
- A line starting with a backtick: `ParseError{"shell commands are not supported: the harness configures what defaults.sh would"}`.
- A line starting with `--`: an option line; this task leaves it to Task 10 and reports `"option lines are not parsed yet"`.
- The time: `*` → `Any`; `+t` or `+a~+b` → `Relative`; `t` or `t1~t2` → `Absolute`; `t` is digits with an optional fraction, or a fraction with a leading `.`, in seconds, converted to `core::Duration` with microsecond precision (`std::chrono::duration<double>` then `duration_cast`, rounded to the nearest microsecond). A missing time: `"expected a time"`.
- Then `<` or `>`: a packet line. Else, in this task, `"call lines are not parsed yet"`.
- After the direction: a word that is `udp`, `icmp`, `gre`, `mpls` or starts with `[` or `(`: `"... is not supported"` (name it). Else the flags: each character of the word is `F`, `S`, `R`, `P`, `U` or `.`; a lone `-` is no flags; anything else `"unknown flag 'X'"`.
- `first:last(length)` with `last - first == length` else `"length ... does not match first:last"`.
- Optional `ack n`, `win n`, `urg n` in any order, each at most once.
- Optional `<` option list `>`: `...` alone sets `any`; else comma-separated items: `nop`, `eol`, `sackOK`, `mss n`, `wscale n`, `TS val v ecr e`, `sack a:b[,c:d...]` (the sack blocks run until an item that is not `a:b`); an item without its number: `"option X needs a value"`.
- Anything left on the line: `"unexpected 'junk'"` with the text.

`Event::line` is the 1-based source line.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Scenario && ./build/debug/tests/unit_tests/fixtures/scenario/Aloe.Tests.Unit.Scenario`
Expected: PASS, 6 tests.

- [ ] **Step 5: Commit**

```bash
git add fixtures/CMakeLists.txt fixtures/scenario tests/unit_tests/fixtures/CMakeLists.txt tests/unit_tests/fixtures/scenario
git commit -m "feat(scenario): the script model and the parser for times and packet lines"
```

---

## Task 10: The parser: call lines and option lines

**Files:**
- Modify: `fixtures/scenario/script/parse_script.cpp`
- Test: `tests/unit_tests/fixtures/scenario/test_parse_script.cpp`

**Interfaces:**
- Produces: call lines parsed into `CallLine`; `--tolerance_usecs=N` into `Script::tolerance`; errors for an unknown option name and for an option line after the first event.

- [ ] **Step 1: Write the failing tests**

Append to `test_parse_script.cpp`:

```cpp
TEST(ParseScript, ParsesCallLines) {
    const auto script = parse_script("0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n"
                                     "+0 setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0\n"
                                     "+0 fcntl(3, F_SETFL, O_RDWR|O_NONBLOCK) = 0\n"
                                     "+0 connect(3, ..., ...) = -1 EINPROGRESS (Operation now in progress)\n"
                                     "+0 write(4, ..., 1000) = 1000\n"
                                     "+32~+32.01 read(3, ..., 1) = -1 ETIMEDOUT\n"
                                     "+0 close(4) = 0\n");
    ASSERT_TRUE(script.has_value()) << script.error().message;
    ASSERT_EQ(script->events.size(), 7);
    using aloe::scenario::Argument;
    using aloe::scenario::CallLine;
    const auto& socket = std::get<CallLine>(script->events[0].action);
    EXPECT_EQ(socket.name, "socket");
    ASSERT_EQ(socket.arguments.size(), 3);
    EXPECT_EQ(socket.arguments[0].kind, Argument::Kind::Ellipsis);
    EXPECT_EQ(socket.arguments[1], (Argument{.kind = Argument::Kind::Word, .text = "SOCK_STREAM"}));
    EXPECT_EQ(socket.result, 3);
    EXPECT_TRUE(socket.error.empty());
    EXPECT_EQ(script->events[0].time.kind, TimeSpec::Kind::Absolute);

    const auto& setsockopt = std::get<CallLine>(script->events[1].action);
    ASSERT_EQ(setsockopt.arguments.size(), 5);
    EXPECT_EQ(setsockopt.arguments[0], (Argument{.kind = Argument::Kind::Integer, .integer = 3}));
    EXPECT_EQ(setsockopt.arguments[3], (Argument{.kind = Argument::Kind::Bracketed, .text = "1"}));

    EXPECT_EQ(std::get<CallLine>(script->events[2].action).arguments[2],
              (Argument{.kind = Argument::Kind::Word, .text = "O_RDWR|O_NONBLOCK"}));

    const auto& connect = std::get<CallLine>(script->events[3].action);
    EXPECT_EQ(connect.result, -1);
    EXPECT_EQ(connect.error, "EINPROGRESS");
    EXPECT_EQ(connect.note, "Operation now in progress");

    const auto& read = std::get<CallLine>(script->events[5].action);
    EXPECT_EQ(read.result, -1);
    EXPECT_EQ(read.error, "ETIMEDOUT");
    EXPECT_TRUE(read.note.empty());
    EXPECT_EQ(script->events[5].time, (TimeSpec{.kind = TimeSpec::Kind::Relative, .low = 32s, .high = 32010ms}));

    const auto& close = std::get<CallLine>(script->events[6].action);
    EXPECT_EQ(close.name, "close");
    ASSERT_EQ(close.arguments.size(), 1);
    EXPECT_EQ(close.arguments[0].integer, 4);
}

TEST(ParseScript, ParsesAQuotedStringArgument) {
    const auto script = parse_script("+0 write(4, \"hello\", 5) = 5\n");
    ASSERT_TRUE(script.has_value()) << script.error().message;
    const auto& write = std::get<aloe::scenario::CallLine>(script->events[0].action);
    EXPECT_EQ(write.arguments[1], (aloe::scenario::Argument{.kind = aloe::scenario::Argument::Kind::String, .text = "hello"}));
}

TEST(ParseScript, ParsesTheToleranceOption) {
    const auto script = parse_script("--tolerance_usecs=2000\n\n0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n");
    ASSERT_TRUE(script.has_value()) << script.error().message;
    EXPECT_EQ(script->tolerance, 2ms);
    EXPECT_EQ(script->events.size(), 1);
}

TEST(ParseScript, ReportsCallLineErrors) {
    const auto no_result = parse_script("+0 close(4)\n");
    ASSERT_FALSE(no_result.has_value());
    EXPECT_NE(no_result.error().message.find("="), std::string_view::npos);

    const auto unknown_option = parse_script("--mss=1000\n0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n");
    ASSERT_FALSE(unknown_option.has_value());
    EXPECT_EQ(unknown_option.error().line, 1);
    EXPECT_NE(unknown_option.error().message.find("mss"), std::string_view::npos);

    const auto late_option = parse_script("0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n--tolerance_usecs=1\n");
    ASSERT_FALSE(late_option.has_value());
    EXPECT_EQ(late_option.error().line, 2);

    const auto unclosed = parse_script("+0 close(4 = 0\n");
    ASSERT_FALSE(unclosed.has_value());
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Scenario && ./build/debug/tests/unit_tests/fixtures/scenario/Aloe.Tests.Unit.Scenario --gtest_filter='ParseScript.Parses*Call*:ParseScript.*Tolerance*:ParseScript.ReportsCallLineErrors:ParseScript.ParsesAQuoted*'`
Expected: FAIL, "call lines are not parsed yet".

- [ ] **Step 3: Parse call lines and option lines**

In `parse_script.cpp`: an option line `--name=value` is accepted only before the first event (else `"an option line after the first event"`); `tolerance_usecs` sets `Script::tolerance` to `std::chrono::microseconds{N}`; any other name: `"unknown option '--name'"`. A call line, after the time: a word (the name), `(`, a comma-separated argument list, `)`, `=`, a signed integer result, then optionally a word of capitals and digits (the errno word) and optionally `(` text to the matching `)` `)` as the note. Arguments: `...` → `Ellipsis`; a signed integer → `Integer`; `[` text `]` → `Bracketed` with the inner text; `"` text `"` → `String` with the inner text; else a word of letters, digits, `_` and `|` → `Word`. A missing `=` reports `"expected '=' after the call"`; a missing `)` reports the column.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `./build/debug/tests/unit_tests/fixtures/scenario/Aloe.Tests.Unit.Scenario`
Expected: PASS, 10 tests.

- [ ] **Step 5: Commit**

```bash
git add fixtures/scenario/script/parse_script.cpp tests/unit_tests/fixtures/scenario/test_parse_script.cpp
git commit -m "feat(scenario): parse call lines and the tolerance option"
```

---

## Task 11: The peer's numbers, the notation and the matcher

**Files:**
- Create: `fixtures/scenario/run/peer.hpp`, `fixtures/scenario/run/peer.cpp`, `fixtures/scenario/run/match.hpp`, `fixtures/scenario/run/match.cpp`, `fixtures/scenario/script/notation.hpp`, `fixtures/scenario/script/notation.cpp`
- Modify: `fixtures/scenario/CMakeLists.txt` (a `Run` STATIC target over `run/`, linking `Aloe::Fixtures::Frames` and the `Script` target; the combined library lists both), `fixtures/scenario/export/aloe/scenario`
- Create: `tests/unit_tests/fixtures/scenario/test_peer.cpp`, `test_notation.cpp`, `test_match.cpp`; add them to the test target, which now also links `Aloe::Fixtures::Frames`

**Interfaces:**
- Consumes: Task 8's `tcp_options_of`; `frames::ParsedFrame`; `wire::TcpSequence`, `wire::TcpOptions`.
- Produces, in `aloe::scenario`:

```cpp
    // peer.hpp
    /// A segment the stack sent, in the script's relative numbers.
    struct ObservedSegment {
        wire::TcpFlags flags{};
        std::uint32_t first = 0; std::uint32_t last = 0; std::uint32_t length = 0;
        std::uint32_t ack = 0;      ///< Relative to the peer's ISN; meaningful with the Ack flag.
        std::uint16_t window = 0; std::uint16_t urgent = 0;
        std::vector<std::byte> options;
        friend bool operator==(const ObservedSegment&, const ObservedSegment&) = default;
    };
    class Peer {
    public:
        Peer(wire::TcpSequence isn, std::uint16_t peer_port, std::uint16_t stack_port) noexcept;
        [[nodiscard]] wire::TcpSequence isn() const noexcept;
        [[nodiscard]] std::optional<wire::TcpSequence> stack_isn() const noexcept;
        [[nodiscard]] std::uint16_t peer_port() const noexcept;
        [[nodiscard]] std::uint16_t stack_port() const noexcept;     ///< The listen port, or the ephemeral port adopted from the stack's SYN.
        /// A frame the stack sent: the first SYN fixes the stack's ISN and its port. Nothing for a frame that is not TCP.
        [[nodiscard]] std::optional<ObservedSegment> observe(const frames::ParsedFrame& frame);
        /// The absolute number for a script's inbound relative sequence: the peer's ISN plus it.
        [[nodiscard]] wire::TcpSequence sequence(std::uint32_t relative) const noexcept;
        /// The absolute number for a script's inbound relative acknowledgement: the stack's ISN plus it once known, else the number as written.
        [[nodiscard]] wire::TcpSequence acknowledgement(std::uint32_t relative) const noexcept;
        /// The base outbound sack blocks and inbound sack blocks are relative to: the peer's ISN, and the stack's or zero.
    };

    // notation.hpp
    /// The option list as header bytes, in the order written; sack blocks get `sack_base` added. `any` has no encoding (asserted).
    [[nodiscard]] std::vector<std::byte> encode_options(const Options& options, wire::TcpSequence sack_base);
    /// Header bytes back into the script's words, sack blocks minus `sack_base`; an unknown kind as `kind N`; malformed bytes as `bad options`.
    [[nodiscard]] std::string describe_options(std::span<const std::byte> options, wire::TcpSequence sack_base);
    /// As written in a script: "> S. 0:0(0) ack 1 win 46720 <mss 1460>".
    [[nodiscard]] std::string describe(const PacketLine& line, wire::TcpSequence sack_base);
    /// The same notation for a segment the stack sent: always outbound.
    [[nodiscard]] std::string describe(const ObservedSegment& segment, wire::TcpSequence sack_base);
    /// "+0.010000" for ten milliseconds since the start: a sign, seconds, six decimals.
    [[nodiscard]] std::string format_time(core::Duration since_start);

    // match.hpp
    /// Nothing when `seen` satisfies `line`; else the first difference, one of "flags differ", "sequence differs",
    /// "ack differs", "window differs", "urgent differs", "options differ". `sack_base` is for the line's sack blocks.
    [[nodiscard]] std::optional<std::string> match(const PacketLine& line, const ObservedSegment& seen, wire::TcpSequence sack_base);
```

  `observe` computes `first = header.sequence - stack_isn` when the stack's ISN is known, else the raw value; `length` is the payload length; `last = first + length`; `ack = header.acknowledgement - isn` when the Ack flag is set; `options` is `tcp_options_of(frame)`. The first frame with the Syn flag sets `stack_isn = header.sequence` and `stack_port = header.source_port`.

- [ ] **Step 1: Write the failing tests**

`test_peer.cpp`:

```cpp
#include <aloe/frames>
#include <aloe/scenario>
#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using aloe::scenario::ObservedSegment;
    using aloe::scenario::Peer;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;
    using aloe::wire::TcpSequence;

    constexpr aloe::wire::MacAddress stack_mac{0x02, 0, 0, 0, 0, 0x01};
    constexpr aloe::wire::MacAddress peer_mac{0x02, 0, 0, 0, 0, 0x02};
    constexpr aloe::wire::Ipv4Address stack_ip{10, 0, 0, 2};
    constexpr aloe::wire::Ipv4Address peer_ip{10, 0, 0, 1};
    constexpr TcpSequence peer_isn{1'000'000};
    constexpr TcpSequence stack_isn{5000};

    /// A frame as the stack sends it: from the stack's port to the peer's.
    aloe::frames::ParsedFrame from_stack(const TcpFlags flags,
                                         const TcpSequence sequence,
                                         const TcpSequence acknowledgement,
                                         const std::uint16_t stack_port,
                                         const std::vector<std::byte>& payload = {},
                                         const bool with_mss                   = false) {
        aloe::frames::TcpSpec spec{.destination_mac  = peer_mac,
                                   .source_mac       = stack_mac,
                                   .source           = stack_ip,
                                   .destination      = peer_ip,
                                   .source_port      = stack_port,
                                   .destination_port = 40000,
                                   .sequence         = sequence,
                                   .acknowledgement  = acknowledgement,
                                   .flags            = flags,
                                   .window           = 46720};
        if (with_mss) {
            spec.mss = 1460;
        }
        return aloe::frames::parse_frame(aloe::frames::tcp_frame(spec, payload)).value();
    }

}  // namespace

TEST(Peer, ASynAckFixesTheStackIsnAndTheNumbersBecomeRelative) {
    Peer peer{peer_isn, 40000, 8080};
    EXPECT_FALSE(peer.stack_isn().has_value());
    EXPECT_EQ(peer.sequence(1), TcpSequence{1'000'001});
    EXPECT_EQ(peer.acknowledgement(7), TcpSequence{7}) << "absolute until the stack's ISN is seen";

    const auto seen = peer.observe(from_stack({TcpFlag::Syn, TcpFlag::Ack}, stack_isn, peer_isn + 1, 8080, {}, true));
    ASSERT_TRUE(seen.has_value());
    EXPECT_EQ(seen->flags, (TcpFlags{TcpFlag::Syn, TcpFlag::Ack}));
    EXPECT_EQ(seen->first, 0);
    EXPECT_EQ(seen->last, 0);
    EXPECT_EQ(seen->length, 0);
    EXPECT_EQ(seen->ack, 1);
    EXPECT_EQ(seen->window, 46720);
    EXPECT_EQ(seen->options, (std::vector<std::byte>{std::byte{2}, std::byte{4}, std::byte{0x05}, std::byte{0xb4}}));
    EXPECT_EQ(peer.stack_isn(), stack_isn);
    EXPECT_EQ(peer.stack_port(), 8080);
    EXPECT_EQ(peer.acknowledgement(1), TcpSequence{5001});

    const auto data = peer.observe(from_stack({TcpFlag::Psh, TcpFlag::Ack}, stack_isn + 1, peer_isn + 1, 8080, std::vector<std::byte>(1000)));
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(data->first, 1);
    EXPECT_EQ(data->last, 1001);
    EXPECT_EQ(data->length, 1000);
    EXPECT_TRUE(data->options.empty());
}

TEST(Peer, TheStacksSynAdoptsItsEphemeralPort) {
    Peer peer{peer_isn, 40000, 8080};
    const auto seen = peer.observe(from_stack({TcpFlag::Syn}, stack_isn, TcpSequence{}, 33333, {}, true));
    ASSERT_TRUE(seen.has_value());
    EXPECT_EQ(peer.stack_port(), 33333);
    EXPECT_EQ(peer.stack_isn(), stack_isn);
    const auto later = peer.observe(from_stack({TcpFlag::Syn}, TcpSequence{9}, TcpSequence{}, 44444, {}, true));
    ASSERT_TRUE(later.has_value());
    EXPECT_EQ(peer.stack_isn(), stack_isn) << "the first SYN fixes it; a retransmit is the same SYN";
    EXPECT_EQ(peer.stack_port(), 33333);
}

TEST(Peer, AResetBeforeAnyIsnIsAbsolute) {
    Peer peer{peer_isn, 40000, 8080};
    const auto seen = peer.observe(from_stack({TcpFlag::Rst, TcpFlag::Ack}, TcpSequence{0}, peer_isn + 1, 8080));
    ASSERT_TRUE(seen.has_value());
    EXPECT_EQ(seen->first, 0);
    EXPECT_EQ(seen->ack, 1);
    EXPECT_FALSE(peer.stack_isn().has_value());
}

TEST(Peer, ANonTcpFrameIsNotObserved) {
    Peer peer{peer_isn, 40000, 8080};
    const auto arp = aloe::frames::parse_frame(aloe::frames::arp_frame(
        aloe::frames::arp_request(stack_mac, stack_ip, peer_ip), aloe::wire::MacAddress::broadcast()));
    ASSERT_TRUE(arp.has_value());
    EXPECT_FALSE(peer.observe(*arp).has_value());
}
```

`test_notation.cpp`:

```cpp
#include <aloe/scenario>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using aloe::scenario::describe;
    using aloe::scenario::describe_options;
    using aloe::scenario::encode_options;
    using aloe::scenario::format_time;
    using aloe::scenario::PacketLine;
    using aloe::scenario::parse_script;
    using aloe::wire::TcpSequence;

    PacketLine line_of(const char* text) {
        const auto script = parse_script(text);
        EXPECT_TRUE(script.has_value()) << script.error().message;
        return std::get<PacketLine>(script->events.at(0).action);
    }

    std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
        std::vector<std::byte> out;
        for (const unsigned value : values) {
            out.push_back(std::byte{static_cast<std::uint8_t>(value)});
        }
        return out;
    }

}  // namespace

TEST(Notation, EncodesEveryOptionKind) {
    const PacketLine line = line_of("+0 < S 0:0(0) <mss 1460,nop,eol,sackOK,wscale 7,TS val 1 ecr 2,sack 1:2,3:4>\n");
    EXPECT_EQ(encode_options(line.options, TcpSequence{1000}),
              bytes({2, 4, 0x05, 0xb4, 1, 0, 4, 2, 3, 3, 7, 8, 10, 0, 0, 0, 1, 0, 0, 0, 2,
                     5, 18, 0, 0, 3, 0xe9, 0, 0, 3, 0xea, 0, 0, 3, 0xeb, 0, 0, 3, 0xec}));
}

TEST(Notation, DescribesOptionBytesInTheScriptsWords) {
    EXPECT_EQ(describe_options(bytes({2, 4, 0x05, 0xb4, 1, 1, 4, 2}), TcpSequence{}), "mss 1460,nop,nop,sackOK");
    EXPECT_EQ(describe_options(bytes({3, 3, 7}), TcpSequence{}), "wscale 7");
    EXPECT_EQ(describe_options(bytes({8, 10, 0, 0, 0, 1, 0, 0, 0, 2}), TcpSequence{}), "TS val 1 ecr 2");
    EXPECT_EQ(describe_options(bytes({5, 10, 0, 0, 3, 0xe9, 0, 0, 3, 0xea}), TcpSequence{1000}), "sack 1:2");
    EXPECT_EQ(describe_options(bytes({30, 2}), TcpSequence{}), "kind 30");
    EXPECT_EQ(describe_options(bytes({2, 4, 0x05}), TcpSequence{}), "bad options");
    EXPECT_EQ(describe_options({}, TcpSequence{}), "");
}

TEST(Notation, DescribesALineAsWritten) {
    EXPECT_EQ(describe(line_of("+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>\n"), TcpSequence{}), "> S. 0:0(0) ack 1 win 46720 <mss 1460>");
    EXPECT_EQ(describe(line_of("+0 < P. 1:1001(1000) ack 1\n"), TcpSequence{}), "< P. 1:1001(1000) ack 1");
    EXPECT_EQ(describe(line_of("+0 > R 2:2(0)\n"), TcpSequence{}), "> R 2:2(0)");
    EXPECT_EQ(describe(line_of("+0 > . 1:1(0) ack 1 <...>\n"), TcpSequence{}), "> . 1:1(0) ack 1 <...>");
    EXPECT_EQ(describe(line_of("+0 < - 1:1(0) urg 3\n"), TcpSequence{}), "< - 1:1(0) urg 3");
}

TEST(Notation, DescribesAnObservedSegment) {
    const aloe::scenario::ObservedSegment seen{.flags   = {aloe::wire::TcpFlag::Syn, aloe::wire::TcpFlag::Ack},
                                               .first   = 0,
                                               .last    = 0,
                                               .length  = 0,
                                               .ack     = 1,
                                               .window  = 46720,
                                               .urgent  = 0,
                                               .options = bytes({2, 4, 0x05, 0xb4})};
    EXPECT_EQ(describe(seen, TcpSequence{}), "> S. 0:0(0) ack 1 win 46720 <mss 1460>");
    aloe::scenario::ObservedSegment plain = seen;
    plain.flags                           = {aloe::wire::TcpFlag::Ack};
    plain.options.clear();
    EXPECT_EQ(describe(plain, TcpSequence{}), "> . 0:0(0) ack 1 win 46720");
}

TEST(Notation, FormatsTimes) {
    EXPECT_EQ(format_time(10ms), "+0.010000");
    EXPECT_EQ(format_time(63s), "+63.000000");
    EXPECT_EQ(format_time(0s), "+0.000000");
}
```

`test_match.cpp`:

```cpp
#include <aloe/scenario>
#include <aloe/wire>
#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using aloe::scenario::match;
    using aloe::scenario::ObservedSegment;
    using aloe::scenario::PacketLine;
    using aloe::scenario::parse_script;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpSequence;

    PacketLine line_of(const char* text) {
        return std::get<PacketLine>(parse_script(text)->events.at(0).action);
    }

    ObservedSegment syn_ack() {
        return {.flags   = {TcpFlag::Syn, TcpFlag::Ack},
                .first   = 0,
                .last    = 0,
                .length  = 0,
                .ack     = 1,
                .window  = 46720,
                .urgent  = 0,
                .options = {std::byte{2}, std::byte{4}, std::byte{0x05}, std::byte{0xb4}}};
    }

}  // namespace

TEST(Match, EveryStatedFieldMustAgree) {
    EXPECT_EQ(match(line_of("+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>\n"), syn_ack(), TcpSequence{}), std::nullopt);
    EXPECT_EQ(match(line_of("+0 > S 0:0(0) <mss 1460>\n"), syn_ack(), TcpSequence{}), "flags differ");
    EXPECT_EQ(match(line_of("+0 > S. 1:1(0) ack 1 <mss 1460>\n"), syn_ack(), TcpSequence{}), "sequence differs");
    EXPECT_EQ(match(line_of("+0 > S. 0:0(0) ack 2 <mss 1460>\n"), syn_ack(), TcpSequence{}), "ack differs");
    EXPECT_EQ(match(line_of("+0 > S. 0:0(0) ack 1 win 100 <mss 1460>\n"), syn_ack(), TcpSequence{}), "window differs");
    EXPECT_EQ(match(line_of("+0 > S. 0:0(0) ack 1 urg 1 <mss 1460>\n"), syn_ack(), TcpSequence{}), "urgent differs");
    EXPECT_EQ(match(line_of("+0 > S. 0:0(0) ack 1 <mss 1000>\n"), syn_ack(), TcpSequence{}), "options differ");
    EXPECT_EQ(match(line_of("+0 > S. 0:0(0) ack 1\n"), syn_ack(), TcpSequence{}), "options differ") << "nothing written means no options";
}

TEST(Match, UnstatedFieldsAreNotCompared) {
    EXPECT_EQ(match(line_of("+0 > S. 0:0(0) ack 1 <...>\n"), syn_ack(), TcpSequence{}), std::nullopt) << "any options";
    EXPECT_EQ(match(line_of("+0 > S. 0:0(0) ack 1 <mss 1460>\n"), syn_ack(), TcpSequence{}), std::nullopt) << "no window written";
    ObservedSegment urgent = syn_ack();
    urgent.urgent          = 5;
    EXPECT_EQ(match(line_of("+0 > S. 0:0(0) ack 1 <mss 1460>\n"), urgent, TcpSequence{}), std::nullopt);
}

TEST(Match, LengthAndRangeBothCount) {
    ObservedSegment data{.flags = {TcpFlag::Psh, TcpFlag::Ack}, .first = 1, .last = 1001, .length = 1000, .ack = 1, .window = 46720, .urgent = 0, .options = {}};
    EXPECT_EQ(match(line_of("+0 > P. 1:1001(1000) ack 1\n"), data, TcpSequence{}), std::nullopt);
    EXPECT_EQ(match(line_of("+0 > P. 1:501(500) ack 1\n"), data, TcpSequence{}), "sequence differs");
}
```

Add the three files to `Aloe.Tests.Unit.Scenario` and `Aloe::Fixtures::Frames` to its link list.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Scenario`
Expected: FAIL to compile, `peer.hpp` and `notation.hpp` missing from the umbrella.

- [ ] **Step 3: Implement the peer, the notation and the matcher**

`fixtures/scenario/CMakeLists.txt` gains a `${SCENARIO}.Run` STATIC target (`run/peer.hpp`, `run/peer.cpp`, `run/match.hpp`, `run/match.cpp`) with `target_include_directories(... PUBLIC run/)` linking `${SCENARIO}.Script` and `Aloe::Fixtures::Frames`; `notation.hpp`/`.cpp` join the `Script` target, which now also links `Aloe::Fixtures::Frames` for nothing (keep it on `Script`'s own: notation needs only `wire`); the combined library lists both targets; the umbrella includes `notation.hpp`, `peer.hpp`, `match.hpp`.

`notation.cpp`: `encode_options` writes kind and length bytes per RFC 793 and 7323 and 2018 (`mss` 2/4, `nop` 1, `eol` 0, `sackOK` 4/2, `wscale` 3/3, `TS` 8/10, `sack` 5/(2 + 8n) with `first + sack_base` and `last + sack_base` as big-endian 32-bit); `describe_options` walks the bytes the way `wire::TcpOptions::parse` does and prints each item, `kind N` for an unknown kind, `bad options` for a malformed list; `describe(PacketLine)` prints direction, flags (`S`, `F`, `R`, `P`, `U` in that order then `.` for Ack, `-` for none), `first:last(length)`, `ack n` / `win n` / `urg n` when present, and `<...>` or `<` the described options `>` when any or non-empty; `describe(ObservedSegment)` prints `>`, the flags, the range, `ack n` when the Ack flag is set, `win n` always, `urg n` when non-zero, and the options when non-empty; `format_time` prints `+` then seconds with six decimals.

`peer.cpp`: as the Interfaces block says. `match.cpp`: compares in the order flags, sequence (`first`, `last`, `length`), ack, window, urgent, options, and returns the first difference.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Scenario && ./build/debug/tests/unit_tests/fixtures/scenario/Aloe.Tests.Unit.Scenario`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add fixtures/scenario tests/unit_tests/fixtures/scenario
git commit -m "feat(scenario): the peer's relative numbers, the script notation and the segment matcher"
```

---

## Task 12: The harness: the bricks over a fabric, with a peer port

**Files:**
- Create: `fixtures/scenario/run/harness.hpp`, `fixtures/scenario/run/harness.cpp`
- Modify: `fixtures/scenario/CMakeLists.txt` (the `Run` target gains the files and links `Aloe::Common::Tcp`, `Aloe::Common::Net`, `Aloe::Common::Loop`, `Aloe::Fixtures::Fabric`), `fixtures/scenario/export/aloe/scenario`
- Create: `tests/unit_tests/fixtures/scenario/test_harness.cpp`; add it to the test target

**Interfaces:**
- Consumes: Task 7's fabric step; `net::Ipv4<fabric::Port>`, `tcp::Stack`, `loop::ShardQueue`, `loop::TimerWheel`, `frames::parse_frame`, `frames::l4_checksum_residue`, `wire::ipv4_header_checksum`.
- Produces, in `aloe::scenario`:

```cpp
    struct HarnessConfig {
        wire::MacAddress stack_mac{0x02, 0, 0, 0, 0, 0x01};
        wire::MacAddress peer_mac{0x02, 0, 0, 0, 0, 0x02};
        wire::Ipv4Address stack_address{10, 0, 0, 2};
        wire::Ipv4Address peer_address{10, 0, 0, 1};
        std::uint8_t prefix = 24;
        std::uint16_t stack_port = 8080;        ///< The port `bind` binds and `listen` listens on.
        std::uint16_t peer_port = 40000;        ///< The peer's port: the source of inbound frames, the target of `connect`.
        wire::TcpSequence peer_isn{1'000'000};  ///< Not zero, so a stack that assumed zero would fail.
        fabric::EmulatedOffloads offloads = fabric::EmulatedOffloads::None;
        tcp::TcpConfig tcp{};
        core::Duration resolution = std::chrono::milliseconds{1};   ///< The wheel's, and the runner's step.
        std::size_t pool_size = 256;
        core::Duration arp_lifetime = std::chrono::hours{1};         ///< `arp_reachable`; `arp_expire` is twice it.
    };

    /// One frame the peer's port received, parsed, with the verdict on each checksum the stack wrote.
    struct Collected { frames::ParsedFrame frame; bool ipv4_checksum_ok = false; bool tcp_checksum_ok = false; };

    class Harness {
    public:
        using Packet = fabric::Packet;
        using Ip = net::Ipv4<fabric::Port>;
        using Tcp = tcp::Stack<Ip>;
        using Connection = Tcp::ConnectionType;
        using Event = Tcp::EventType;

        explicit Harness(const HarnessConfig& config);   // both MACs learned; nothing listening
        [[nodiscard]] const HarnessConfig& config() const noexcept;
        [[nodiscard]] fabric::Fabric& fabric() noexcept;
        [[nodiscard]] fabric::Port& stack_port() noexcept;
        [[nodiscard]] fabric::Port& peer_port() noexcept;
        [[nodiscard]] loop::ShardQueue<fabric::Port>& queue() noexcept;
        [[nodiscard]] Ip& ip() noexcept;
        [[nodiscard]] loop::TimerWheel& wheel() noexcept;
        [[nodiscard]] Tcp& tcp() noexcept;
        /// Transmits `frame` from the peer's port and runs IP and TCP over what the stack's port received, at `now`.
        void inject(std::span<const std::byte> frame, core::TimePoint now);
        /// An empty process at `now`, the wheel, then the fabric: the first half of a tick with nothing injected.
        void step(core::TimePoint now);
        /// TCP's flush, then the queue's.
        void flush(core::TimePoint now);
        /// Every frame the peer's port received since the last call, oldest first, parsed and checked.
        [[nodiscard]] std::vector<Collected> collect();
    };
```

  Member order: `config_`, `fabric_`, `fabric::Port* peer_port_`, `fabric::Port* stack_port_` (both from `add_port`, the peer first), `counters_`, `queue_{*stack_port_, 0, 64, counters_}`, `ip_{queue_, Ipv4Config{.address = stack_address, .prefix = prefix, .arp_reachable = arp_lifetime, .arp_expire = 2 * arp_lifetime}}`, `wheel_{resolution, core::TimePoint{}}`, `tcp_{ip_, wheel_, config.tcp}`, `std::vector<Packet> burst_` of 64. The constructor calls `ip_.learn(peer_address, peer_mac, core::TimePoint{})`. `collect` sets `ipv4_checksum_ok = wire::ipv4_header_checksum(frame.ipv4_header) == 0` hmm: use `wire::internet_checksum(ipv4_header) == 0` as the fabric's own verdict does, and `tcp_checksum_ok = frames::l4_checksum_residue(*ipv4, l4) == 0`, both false for a frame without IPv4 or TCP.

- [ ] **Step 1: Write the failing tests**

`test_harness.cpp`:

```cpp
#include <aloe/frames>
#include <aloe/scenario>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using aloe::scenario::Harness;
    using aloe::scenario::HarnessConfig;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;
    using TimePoint = aloe::core::TimePoint;

    /// The peer's SYN towards the harness's listen port, built from its configuration.
    std::vector<std::byte> peer_syn(const HarnessConfig& config) {
        return aloe::frames::tcp_frame({.destination_mac  = config.stack_mac,
                                        .source_mac       = config.peer_mac,
                                        .source           = config.peer_address,
                                        .destination      = config.stack_address,
                                        .source_port      = config.peer_port,
                                        .destination_port = config.stack_port,
                                        .sequence         = config.peer_isn,
                                        .flags            = TcpFlags{TcpFlag::Syn},
                                        .mss              = 1460},
                                       {});
    }

    class HarnessTest : public ::testing::TestWithParam<aloe::fabric::EmulatedOffloads> {};

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             HarnessTest,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None, aloe::fabric::EmulatedOffloads::Checksums));

}  // namespace

TEST_P(HarnessTest, ASynGetsASynAckWithGoodChecksums) {
    HarnessConfig config;
    config.offloads = GetParam();
    Harness harness{config};
    ASSERT_TRUE(harness.tcp().listen(config.stack_port).has_value());
    harness.inject(peer_syn(config), TimePoint{});
    harness.flush(TimePoint{});
    const auto collected = harness.collect();
    ASSERT_EQ(collected.size(), 1);
    ASSERT_TRUE(collected[0].frame.tcp.has_value());
    EXPECT_EQ(collected[0].frame.tcp->flags, (TcpFlags{TcpFlag::Syn, TcpFlag::Ack}));
    EXPECT_EQ(collected[0].frame.tcp->acknowledgement, config.peer_isn + 1);
    EXPECT_TRUE(collected[0].ipv4_checksum_ok);
    EXPECT_TRUE(collected[0].tcp_checksum_ok);
    EXPECT_TRUE(harness.collect().empty()) << "collected once";
}

TEST_P(HarnessTest, StepFiresTheWheelAndTheRetransmitComesOut) {
    HarnessConfig config;
    config.offloads = GetParam();
    Harness harness{config};
    ASSERT_TRUE(harness.tcp().listen(config.stack_port).has_value());
    harness.inject(peer_syn(config), TimePoint{});
    harness.flush(TimePoint{});
    ASSERT_EQ(harness.collect().size(), 1);
    TimePoint now{};
    for (int tick = 0; tick < 999; ++tick) {
        now += 1ms;
        harness.step(now);
        harness.flush(now);
        ASSERT_TRUE(harness.collect().empty()) << "nothing before one second, at tick " << tick;
    }
    now += 1ms;
    harness.step(now);
    harness.flush(now);
    const auto collected = harness.collect();
    ASSERT_EQ(collected.size(), 1) << "the SYN-ACK again at one second";
    EXPECT_EQ(collected[0].frame.tcp->flags, (TcpFlags{TcpFlag::Syn, TcpFlag::Ack}));
    EXPECT_EQ(harness.tcp().counters().retransmits, 1);
}

TEST_P(HarnessTest, TheArpCacheOutlivesAnyScript) {
    // `net::Ipv4` exposes no config accessor; the lifetimes show through behaviour: five minutes in, past the
    // cache's default 60 s reachable and 120 s expiry, a reply still needs no ARP request.
    HarnessConfig config;
    config.offloads = GetParam();
    Harness harness{config};
    EXPECT_EQ(harness.wheel().resolution(), 1ms);
    ASSERT_TRUE(harness.tcp().listen(config.stack_port).has_value());
    const TimePoint later = TimePoint{} + std::chrono::minutes{5};
    harness.step(later);
    harness.inject(peer_syn(config), later);
    harness.flush(later);
    const auto collected = harness.collect();
    ASSERT_EQ(collected.size(), 1);
    EXPECT_EQ(collected[0].frame.tcp->flags, (TcpFlags{TcpFlag::Syn, TcpFlag::Ack}));
    EXPECT_EQ(harness.ip().counters().arp_requests_sent, 0);
    EXPECT_EQ(harness.ip().counters().send_unresolved, 0);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Scenario`
Expected: FAIL to compile, `harness.hpp` missing.

- [ ] **Step 3: Implement the harness**

As the Interfaces block says. `inject`: allocate on the peer port, `frames::fill`, transmit, then `queue_.receive(burst_)`, `ip_.process(first(n), now)`, `tcp_.process(ip_.received(wire::Ipv4Protocol::Tcp), now)`. `step`: `tcp_.process({}, now)`, `std::ignore = wheel_.advance(now)`, `fabric_.advance(now)`. `flush`: `tcp_.flush(now)`, `std::ignore = queue_.flush()`. `collect`: receive on the peer port in bursts of 16 until empty, `parse_frame` each (skip a frame the parser refuses), compute the two verdicts.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Scenario && ./build/debug/tests/unit_tests/fixtures/scenario/Aloe.Tests.Unit.Scenario --gtest_filter='*Harness*'`
Expected: PASS, 6 tests (three per checksum mode).

- [ ] **Step 5: Commit**

```bash
git add fixtures/scenario tests/unit_tests/fixtures/scenario
git commit -m "feat(scenario): the harness, the bricks over a fabric with a peer port"
```

---

## Task 13: The runner: the clock, inbound lines, outbound matching in queue order

**Files:**
- Create: `fixtures/scenario/run/runner.hpp`, `fixtures/scenario/run/runner.cpp`
- Modify: `fixtures/scenario/CMakeLists.txt` (the `Run` target gains the files), `fixtures/scenario/export/aloe/scenario`
- Create: `tests/unit_tests/fixtures/scenario/test_runner.cpp`; add it to the test target

**Interfaces:**
- Consumes: Tasks 9 to 12.
- Produces, in `aloe::scenario`:

```cpp
    struct RunnerConfig {
        core::Duration tolerance = std::chrono::milliseconds{1};  ///< Widens outbound windows both ways; the script's --tolerance_usecs overrides.
        core::Duration any_bound = std::chrono::seconds{120};     ///< How long a `*` line waits before it is missing.
    };
    struct Failure { std::size_t line = 0; std::string message; };
    class Runner {
    public:
        Runner(Harness& harness, const Script& script, const RunnerConfig& config = {});
        /// Runs the script to its end, or to the first failure. Call once.
        [[nodiscard]] std::expected<void, Failure> run();
    };
    /// "<path>:<line>: <message>" for a report.
    [[nodiscard]] std::string format(const Failure& failure, std::string_view script_path);
```

  In this task the runner binds the calls `socket`, `setsockopt` (`SO_REUSEADDR`, `TCP_NODELAY`), `fcntl`, `bind` and `listen`; every other call fails with `unbound call <name>`. Task 14 binds the rest.

  The runner's state: `now_` (starts at the epoch), `previous_` (the previous event's actual time, starts at the epoch), a `Peer` built from the harness's config (`peer_isn`, `peer_port`, `stack_port`), a `std::deque<Seen>` of sent segments where `Seen{ObservedSegment segment; core::TimePoint stamp; bool checksums_ok;}`, and a descriptor table (Task 14). One **step** is: `now_ += resolution; harness.step(now_); drain_events(); harness.flush(now_); collect();` where `collect()` appends every `Collected` from `harness.collect()` to the queue with `stamp = now_` after `peer.observe` (a non-TCP frame from the stack is a failure: `unexpected non-TCP frame`). The **window** of an event: `Relative` → `[previous_ + low, previous_ + high]`; `Absolute` → `[epoch + low, epoch + high]`; `Any` → `[now_, now_ + any_bound]`.

  An **inbound line** or a **call**: if the time is `Absolute` and `lo < now_`, fail `time goes backwards: <lo> is before <now>`; otherwise step until `now_ >= lo`; act; then `drain_events(); harness.flush(now_); collect();` at the same stamp; `previous_ = now_`. An inbound line builds its frame with `frames::tcp_frame` from `TcpSpec{peer MACs and addresses from the config, source_port = peer.peer_port(), destination_port = peer.stack_port(), sequence = peer.sequence(first), acknowledgement = peer.acknowledgement(*ack) when written (else zero), flags, window = *window when written (else 65535), options = encode_options(options, peer.acknowledgement(0))}` (the stack's ISN once known, else zero, is what the peer's sack blocks are relative to) and a payload of `length` bytes of `frames::pattern(length, static_cast<std::uint8_t>(first))`, then `harness.inject(frame, now_)`. An inbound line with `<...>` fails: `inbound options must be written`.

  An **outbound line**: `[lo, hi]` widened by the tolerance; step while the queue is empty and `now_ <= hi + tolerance`; an empty queue then fails `missing: expected "<line>" in window [<lo>..<hi>]` (for `Any`, `... within the bound of <any_bound>`). With a head: a false `checksums_ok` fails `bad checksum: the stack sent "<seen>" at <stamp>`; `match` returning a reason fails `expected "<line>" at <window>; the stack sent "<seen>" at <stamp>: <reason>`; a stamp before `lo - tolerance` fails `early: ...` and after `hi + tolerance` fails `late: ...` with the same shape; else pop the head and set `previous_ = stamp`. Times are printed with `format_time(stamp - epoch)`.

  **End of script:** a non-empty queue fails `unexpected: the stack sent "<seen>" at <stamp> after the script's end` with the line of the last event. `Failure::line` is the event's source line throughout.

- [ ] **Step 1: Write the failing tests**

`test_runner.cpp`:

```cpp
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/scenario>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using aloe::scenario::Failure;
    using aloe::scenario::Harness;
    using aloe::scenario::HarnessConfig;
    using aloe::scenario::parse_script;
    using aloe::scenario::Runner;
    using aloe::scenario::RunnerConfig;

    constexpr std::string_view listen_preamble = "0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n"
                                                 "+0 setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0\n"
                                                 "+0 bind(3, ..., ...) = 0\n"
                                                 "+0 listen(3, 1) = 0\n";

    /// Parses and runs `text` on a fresh harness in the given checksum mode.
    std::expected<void, Failure> run_text(const std::string& text,
                                          const aloe::fabric::EmulatedOffloads offloads = aloe::fabric::EmulatedOffloads::None,
                                          const RunnerConfig& runner_config              = {},
                                          HarnessConfig config                           = {}) {
        const auto script = parse_script(text);
        EXPECT_TRUE(script.has_value()) << script.error().message;
        if (!script) {
            return std::unexpected{Failure{.line = script.error().line, .message = script.error().message}};
        }
        config.offloads = offloads;
        Harness harness{config};
        Runner runner{harness, *script, runner_config};
        return runner.run();
    }

    bool mentions(const std::expected<void, Failure>& result, const std::string_view word) {
        return !result.has_value() && result.error().message.find(word) != std::string_view::npos;
    }

    class RunnerTest : public ::testing::TestWithParam<aloe::fabric::EmulatedOffloads> {};

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             RunnerTest,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None, aloe::fabric::EmulatedOffloads::Checksums));

}  // namespace

TEST_P(RunnerTest, AnExpectedSegmentWithinItsWindowPasses) {
    const auto result = run_text(std::string{listen_preamble} + "+0 < S 0:0(0) win 65535 <mss 1460>\n"
                                                                 "+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>\n",
                                 GetParam());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, TheRetransmitComesAtPlusOne) {
    const auto result = run_text(std::string{listen_preamble} + "+0 < S 0:0(0) win 65535 <mss 1460>\n"
                                                                 "+0 > S. 0:0(0) ack 1 <mss 1460>\n"
                                                                 "+1 > S. 0:0(0) ack 1 <mss 1460>\n"
                                                                 "+2 > S. 0:0(0) ack 1 <mss 1460>\n",
                                 GetParam());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, ASegmentThatComesEarlyFails) {
    const auto result = run_text(std::string{listen_preamble} + "+0 < S 0:0(0) win 65535 <mss 1460>\n"
                                                                 "+0 > S. 0:0(0) ack 1 <mss 1460>\n"
                                                                 "+2 > S. 0:0(0) ack 1 <mss 1460>\n",
                                 GetParam());
    EXPECT_TRUE(mentions(result, "early")) << (result ? "passed" : result.error().message);
    EXPECT_EQ(result.error().line, 7);
}

TEST_P(RunnerTest, AMissingSegmentFails) {
    const auto result = run_text(std::string{listen_preamble} + "+0 > S. 0:0(0) ack 1 <mss 1460>\n", GetParam());
    EXPECT_TRUE(mentions(result, "missing")) << (result ? "passed" : result.error().message);
    EXPECT_EQ(result.error().line, 5);
}

TEST_P(RunnerTest, AMismatchNamesTheFieldAndBothSegments) {
    const auto result = run_text(std::string{listen_preamble} + "+0 < S 0:0(0) win 65535 <mss 1460>\n"
                                                                 "+0 > S 0:0(0) <mss 1460>\n",
                                 GetParam());
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(mentions(result, "flags differ")) << result.error().message;
    EXPECT_TRUE(mentions(result, "> S 0:0(0) <mss 1460>")) << result.error().message;
    EXPECT_TRUE(mentions(result, "> S. 0:0(0) ack 1 win 46720 <mss 1460>")) << result.error().message;
    EXPECT_EQ(result.error().line, 6);
}

TEST_P(RunnerTest, TheQueueSurvivesAnInboundLineBetweenTwoOutboundLines) {
    const auto result = run_text(std::string{listen_preamble} + "+0 < S 0:0(0) win 65535 <mss 1460>\n"
                                                                 "+0 < . 1:1(0) ack 1 win 65535\n"
                                                                 "+0 > S. 0:0(0) ack 1 <mss 1460>\n",
                                 GetParam());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, AnUnexpectedSegmentAtTheEndFails) {
    const auto result = run_text(std::string{listen_preamble} + "+0 < S 0:0(0) win 65535 <mss 1460>\n", GetParam());
    EXPECT_TRUE(mentions(result, "unexpected")) << (result ? "passed" : result.error().message);
    EXPECT_TRUE(mentions(result, "> S. 0:0(0) ack 1 win 46720 <mss 1460>"));
}

TEST_P(RunnerTest, AnAbsoluteTimeGoingBackwardsFails) {
    const auto result = run_text(std::string{listen_preamble} + "0.5 < S 0:0(0) win 65535 <mss 1460>\n"
                                                                 "0.5 > S. 0:0(0) ack 1 <mss 1460>\n"
                                                                 "0.2 < . 1:1(0) ack 1 win 65535\n",
                                 GetParam());
    EXPECT_TRUE(mentions(result, "backwards")) << (result ? "passed" : result.error().message);
    EXPECT_EQ(result.error().line, 7);
}

TEST_P(RunnerTest, AnAnyLineGivesUpAtTheBound) {
    const auto result = run_text(std::string{listen_preamble} + "* > S. 0:0(0) ack 1 <mss 1460>\n",
                                 GetParam(),
                                 RunnerConfig{.any_bound = 2s});
    EXPECT_TRUE(mentions(result, "missing")) << (result ? "passed" : result.error().message);
    EXPECT_TRUE(mentions(result, "2.000000"));
}

TEST_P(RunnerTest, TheScriptsToleranceOverridesTheRunners) {
    // The SYN-ACK retransmit lands at exactly one second; a window ending at 0.9 with a 200 ms tolerance admits it.
    const auto result = run_text("--tolerance_usecs=200000\n" + std::string{listen_preamble} +
                                     "+0 < S 0:0(0) win 65535 <mss 1460>\n"
                                     "+0 > S. 0:0(0) ack 1 <mss 1460>\n"
                                     "+0.9 > S. 0:0(0) ack 1 <mss 1460>\n",
                                 GetParam());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, AnUnboundCallFailsWithItsName) {
    const auto result = run_text("0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n+0 getsockopt(3, SOL_TCP, TCP_INFO, ..., ...) = 0\n", GetParam());
    EXPECT_TRUE(mentions(result, "getsockopt")) << (result ? "passed" : result.error().message);
    EXPECT_EQ(result.error().line, 2);
}

TEST_P(RunnerTest, ABadChecksumFailsEvenWhenEveryFieldMatches) {
    HarnessConfig config;
    config.offloads = GetParam();
    Harness harness{config};
    // A forged SYN-ACK from the stack's own port with a corrupted TCP checksum: every field a script would check is right.
    const auto forged = aloe::frames::tcp_frame({.destination_mac  = config.peer_mac,
                                                 .source_mac       = config.stack_mac,
                                                 .source           = config.stack_address,
                                                 .destination      = config.peer_address,
                                                 .source_port      = config.stack_port,
                                                 .destination_port = config.peer_port,
                                                 .sequence         = aloe::wire::TcpSequence{7000},
                                                 .acknowledgement  = config.peer_isn + 1,
                                                 .flags            = {aloe::wire::TcpFlag::Syn, aloe::wire::TcpFlag::Ack},
                                                 .window           = 46720,
                                                 .mss              = 1460,
                                                 .checksums        = aloe::frames::Checksums::Wrong},
                                                {});
    auto packet = harness.stack_port().allocate(0);
    ASSERT_TRUE(packet.has_value());
    ASSERT_TRUE(aloe::frames::fill(*packet, forged));
    std::array<aloe::fabric::Packet, 1> burst{std::move(*packet)};
    ASSERT_EQ(harness.stack_port().transmit(0, burst), 1);

    const auto script = parse_script("+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>\n");
    ASSERT_TRUE(script.has_value());
    Runner runner{harness, *script};
    const auto result = runner.run();
    EXPECT_TRUE(mentions(result, "checksum")) << (result ? "passed" : result.error().message);
}

TEST(RunnerFormat, PrefixesThePathAndLine) {
    EXPECT_EQ(aloe::scenario::format(Failure{.line = 12, .message = "missing: x"}, "tests/scenarios/tcp/a.pkt"),
              "tests/scenarios/tcp/a.pkt:12: missing: x");
}
```

Note on `ABadChecksumFailsEvenWhenEveryFieldMatches` under `EmulatedOffloads::Checksums`: the fabric fills checksums only when the packet's transmit metadata asks; the forged packet asks for nothing, so its wrong checksum reaches the peer in both modes.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Scenario`
Expected: FAIL to compile, `runner.hpp` missing.

- [ ] **Step 3: Implement the runner**

As the Interfaces block says. Private members: `Harness* harness_`, `const Script* script_`, `RunnerConfig config_` (with `tolerance` replaced by the script's when set), `Peer peer_`, `core::TimePoint now_`, `core::TimePoint previous_`, `std::deque<Seen> queue_`, `std::map<int, Binding> descriptors_` (Task 14 fills the `Binding` type; this task needs only `Socket`, `Bound` and `Listener` kinds with the port). Private functions: `step()`, `collect()`, `drain_events()` (this task: `while (harness_->tcp().poll_event()) {}`), `window_of(const Event&)`, `run_inbound`, `run_outbound`, `run_call`, `fail(const Event&, std::string)`. `run()` iterates the events, returns the first failure, then the end-of-script check.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Scenario && ./build/debug/tests/unit_tests/fixtures/scenario/Aloe.Tests.Unit.Scenario --gtest_filter='*Runner*'`
Expected: PASS, 25 tests.

- [ ] **Step 5: Commit**

```bash
git add fixtures/scenario tests/unit_tests/fixtures/scenario
git commit -m "feat(scenario): the runner's clock, inbound lines and outbound matching in queue order"
```

---

## Task 14: The runner: the connection calls

**Files:**
- Modify: `fixtures/scenario/run/runner.hpp`, `fixtures/scenario/run/runner.cpp`
- Test: `tests/unit_tests/fixtures/scenario/test_runner.cpp`

**Interfaces:**
- Consumes: Task 13; `tcp::Stack::connect`, `unlisten`; `Connection::prepare`, `commit`, `unread`, `consume`, `peer_closed`, `close`, `release`, `state`; `stream::Events`.
- Produces: the bindings of the spec's table. The descriptor table: `struct Binding { enum class Kind : std::uint8_t { Socket, Listener, Connection, Closed }; Kind kind = Kind::Socket; std::uint16_t port = 0; Harness::Connection* connection = nullptr; bool established = false; std::optional<stream::Event> terminal; };` plus `std::deque<Harness::Connection*> pending_accepts_`.

  `drain_events()`: for each event, find the binding whose `connection` is the event's; `accepted()` on an unknown connection pushes it to `pending_accepts_`; `connected()` or `accepted()` sets `established`; `closed()`, `reset()`, `timed_out()` or `unresolved()` records the first of them as `terminal`, calls `release()` on the connection and nulls the pointer (for a pending accept that resets before `accept`, drop it from the deque and release). Other events are ignored.

  The calls, each failing with the quoted text when the descriptor is not of the kind named:

| Call | Does | Result check |
|---|---|---|
| `socket(..., SOCK_STREAM, IPPROTO_TCP) = n` | Binds `n` as `Socket`. `n` already bound: `descriptor n is in use`. | none |
| `setsockopt(n, SOL_SOCKET, SO_REUSEADDR, ...)`, `setsockopt(n, SOL_TCP|IPPROTO_TCP, TCP_NODELAY, ...)` | Nothing. Any other option word: `unbound call setsockopt(<word>)`. | result must be `0`: else `setsockopt: result <r> is not 0` |
| `fcntl(n, F_SETFL, ...)` | Nothing. | result `0` |
| `bind(n, ..., ...)` | `port = config.stack_port`. | result `0` |
| `listen(n, backlog)` | `tcp.listen(port)`; a `ListenError`: `listen: <error>`. Kind becomes `Listener`. | result `0` |
| `accept(n, ..., ...) = m` | `n` is a `Listener`. With `pending_accepts_` empty the call is *unsatisfied*; else pop the front, bind `m` as `Connection` with `established = true`. `m` in use: `descriptor m is in use`. | none beyond `m` |
| `connect(n, ..., ...)` | `tcp.connect({peer_address, peer_port}, now_)`; a `ConnectError`: `connect: <error>`; binds `n` as `Connection`. | result `0`, or `-1` with error `EINPROGRESS`; anything else: `connect: unsupported result` |
| `write(n, ..., count)`, `send(n, ..., count, flags)` | On a `Connection` with a live pointer: loop `prepare(remaining)`; none: stop; fill the span with `frames::pattern(span.size(), static_cast<std::uint8_t>(written))`; `commit(span.size())`; false: stop; `written += span.size()`. | `written` must equal the result: else `write: accepted <written> of <count>, expected <result>` |
| `read(n, ..., count)`, `recv(n, ..., count, flags)` | See below. | see below |
| `close(n)` | `Listener`: `tcp.unlisten(port)`, unbind. `Connection` with a live pointer: `connection->close()`, kind `Closed` (the pointer stays until the terminal event releases it). `Connection` already terminal: unbind. `Socket`: unbind. | result `0` |
| `shutdown(n, SHUT_WR)` | `connection->close()`; the kind stays `Connection`. | result `0` |
| anything else | `unbound call <name>` | |

  `read`/`recv` with result `m >= 0`: `available = min(unread().size(), count)`. `m > 0`: satisfied when `available == m` (consume `m`); when `available == 0` the call is unsatisfied; when `0 < available != m`: `read: <available> unread, expected <m>`. `m == 0`: satisfied when `peer_closed()` and `unread().empty()`; unsatisfied otherwise. With result `-1` and an errno word: `EAGAIN` is satisfied when the connection is live, `unread().empty()` and not `peer_closed()`; `ETIMEDOUT` when `terminal == TimedOut`; `ECONNRESET` when `terminal == Reset` and `established`; `ECONNREFUSED` when `terminal == Reset` and not `established`; `EHOSTUNREACH` when `terminal == Unresolved`; any other word: `unbound errno <word>`. A terminal connection asked for data (`m >= 0`) fails: `read: the connection is <Reset|TimedOut|...>`. An **unsatisfied** `accept`, `read` or `recv` whose event has a window (`low != high`) is retried after one step until `now_ > hi`; then, or at once without a window, it fails `read: not satisfied: <what was found>` naming the unread count, `peer_closed`, the state or the terminal event. Every other call runs once.

- [ ] **Step 1: Write the failing tests**

Append to `test_runner.cpp`:

```cpp
namespace {

    constexpr std::string_view passive_open = "0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n"
                                              "+0 setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0\n"
                                              "+0 bind(3, ..., ...) = 0\n"
                                              "+0 listen(3, 1) = 0\n"
                                              "+0 < S 0:0(0) win 65535 <mss 1460>\n"
                                              "+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>\n"
                                              "+0 < . 1:1(0) ack 1 win 65535\n"
                                              "+0 accept(3, ..., ...) = 4\n";

}  // namespace

TEST_P(RunnerTest, PassiveOpenWithAcceptAndClose) {
    const auto result = run_text(std::string{passive_open} + "+0 close(4) = 0\n"
                                                              "+0 > F. 1:1(0) ack 1 win 46720\n"
                                                              "+0 < . 1:1(0) ack 2 win 65535\n"
                                                              "+0 < F. 1:1(0) ack 2 win 65535\n"
                                                              "+0 > . 2:2(0) ack 2 win 46720\n",
                                 GetParam());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, ActiveOpenWithConnect) {
    const auto result = run_text("0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n"
                                 "+0 fcntl(3, F_SETFL, O_RDWR|O_NONBLOCK) = 0\n"
                                 "+0 connect(3, ..., ...) = -1 EINPROGRESS (Operation now in progress)\n"
                                 "+0 > S 0:0(0) win 46720 <mss 1460>\n"
                                 "+0 < S. 0:0(0) ack 1 win 65535 <mss 1460>\n"
                                 "+0 > . 1:1(0) ack 1 win 46720\n",
                                 GetParam());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, WriteSegmentsByThePeersMssAcrossInterleavedAcks) {
    const auto result = run_text("0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n"
                                 "+0 bind(3, ..., ...) = 0\n"
                                 "+0 listen(3, 1) = 0\n"
                                 "+0 < S 0:0(0) win 65535 <mss 1000>\n"
                                 "+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>\n"
                                 "+0 < . 1:1(0) ack 1 win 65535\n"
                                 "+0 accept(3, ..., ...) = 4\n"
                                 "+0 write(4, ..., 2500) = 2500\n"
                                 "+0 > P. 1:1001(1000) ack 1 win 46720\n"
                                 "+0 < . 1:1(0) ack 1001 win 65535\n"
                                 "+0 > P. 1001:2001(1000) ack 1 win 46720\n"
                                 "+0 < . 1:1(0) ack 2001 win 65535\n"
                                 "+0 > P. 2001:2501(500) ack 1 win 46720\n"
                                 "+0 < . 1:1(0) ack 2501 win 65535\n",
                                 GetParam());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, ReadConsumesWhatArrivedAndEagainWhenNothingIs) {
    const auto result = run_text(std::string{passive_open} + "+0 < P. 1:1001(1000) ack 1 win 65535\n"
                                                              "+0 > . 1:1(0) ack 1001 win 45720\n"
                                                              "+0 read(4, ..., 1000) = 1000\n"
                                                              "+0 > . 1:1(0) ack 1001 win 46720\n"
                                                              "+0 read(4, ..., 1000) = -1 EAGAIN\n",
                                 GetParam());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, WriteBeyondTheWindowFailsWithTheCountAccepted) {
    const auto result = run_text("0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n"
                                 "+0 bind(3, ..., ...) = 0\n"
                                 "+0 listen(3, 1) = 0\n"
                                 "+0 < S 0:0(0) win 500 <mss 1460>\n"
                                 "+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>\n"
                                 "+0 < . 1:1(0) ack 1 win 500\n"
                                 "+0 accept(3, ..., ...) = 4\n"
                                 "+0 write(4, ..., 1000) = 1000\n",
                                 GetParam());
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(mentions(result, "500")) << result.error().message;
    EXPECT_TRUE(mentions(result, "1000")) << result.error().message;
    EXPECT_EQ(result.error().line, 8);
}

TEST_P(RunnerTest, ReadBeyondUnreadFailsWithTheCounts) {
    const auto result = run_text(std::string{passive_open} + "+0 < P. 1:101(100) ack 1 win 65535\n"
                                                              "+0 > . 1:1(0) ack 101 win 46620\n"
                                                              "+0 read(4, ..., 1000) = 500\n",
                                 GetParam());
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(mentions(result, "100")) << result.error().message;
    EXPECT_TRUE(mentions(result, "500")) << result.error().message;
}

TEST_P(RunnerTest, AReadWithAWindowWaitsForTheTimeout) {
    HarnessConfig config;
    config.tcp.retries = 1;  // the SYN again at one second, TimedOut two seconds after that
    const auto result  = run_text("0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n"
                                  "+0 connect(3, ..., ...) = -1 EINPROGRESS (Operation now in progress)\n"
                                  "+0 > S 0:0(0) win 46720 <mss 1460>\n"
                                  "+1 > S 0:0(0) win 46720 <mss 1460>\n"
                                  "+2~+2.01 read(3, ..., 1) = -1 ETIMEDOUT\n",
                                  GetParam(),
                                  {},
                                  config);
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, AReadWithoutAWindowRunsOnce) {
    HarnessConfig config;
    config.tcp.retries = 1;
    const auto result  = run_text("0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n"
                                  "+0 connect(3, ..., ...) = -1 EINPROGRESS (Operation now in progress)\n"
                                  "+0 > S 0:0(0) win 46720 <mss 1460>\n"
                                  "+0.5 read(3, ..., 1) = -1 ETIMEDOUT\n",
                                  GetParam(),
                                  {},
                                  config);
    EXPECT_TRUE(mentions(result, "not satisfied")) << (result ? "passed" : result.error().message);
    EXPECT_TRUE(mentions(result, "SynSent")) << result.error().message;
}

TEST_P(RunnerTest, ResetGivesEconnresetAndTheSlotIsReleased) {
    const auto result = run_text(std::string{passive_open} + "+0 < R. 1:1(0) ack 1 win 65535\n"
                                                              "+0 read(4, ..., 1000) = -1 ECONNRESET\n"
                                                              "+0 < . 1:1(0) ack 1 win 65535\n"
                                                              "+0 > R 1:1(0) win 0\n",
                                 GetParam());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, ARefusedConnectGivesEconnrefused) {
    const auto result = run_text("0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n"
                                 "+0 connect(3, ..., ...) = -1 EINPROGRESS (Operation now in progress)\n"
                                 "+0 > S 0:0(0) win 46720 <mss 1460>\n"
                                 "+0 < R. 0:0(0) ack 1 win 0\n"
                                 "+0 read(3, ..., 1) = -1 ECONNREFUSED\n",
                                 GetParam());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, CloseOnAListenerUnlistens) {
    const auto result = run_text(std::string{listen_preamble} + "+0 close(3) = 0\n"
                                                                 "+0 < S 0:0(0) win 65535 <mss 1460>\n"
                                                                 "+0 > R. 0:0(0) ack 1 win 0\n",
                                 GetParam());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, ShutdownSendsAFinAndKeepsReading) {
    const auto result = run_text(std::string{passive_open} + "+0 shutdown(4, SHUT_WR) = 0\n"
                                                              "+0 > F. 1:1(0) ack 1 win 46720\n"
                                                              "+0 < . 1:1(0) ack 2 win 65535\n"
                                                              "+0 < P. 1:101(100) ack 2 win 65535\n"
                                                              "+0 > . 2:2(0) ack 101 win 46620\n"
                                                              "+0 read(4, ..., 100) = 100\n"
                                                              "+0 > . 2:2(0) ack 101 win 46720\n",
                                 GetParam());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, AcceptWithNothingPendingFails) {
    const auto result = run_text(std::string{listen_preamble} + "+0 accept(3, ..., ...) = 4\n", GetParam());
    EXPECT_TRUE(mentions(result, "accept")) << (result ? "passed" : result.error().message);
    EXPECT_TRUE(mentions(result, "not satisfied"));
}

TEST_P(RunnerTest, AnUnknownSetsockoptOptionFails) {
    const auto result = run_text("0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n+0 setsockopt(3, SOL_TCP, TCP_MAXSEG, [1000], 4) = 0\n", GetParam());
    EXPECT_TRUE(mentions(result, "TCP_MAXSEG")) << (result ? "passed" : result.error().message);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Scenario && ./build/debug/tests/unit_tests/fixtures/scenario/Aloe.Tests.Unit.Scenario --gtest_filter='*Runner*'`
Expected: the new tests FAIL with `unbound call accept` and the like.

- [ ] **Step 3: Bind the calls**

As the Interfaces block says: `Binding`, `pending_accepts_`, `drain_events` with the release rule, and one private function per call name dispatched from `run_call` by `CallLine::name`. The retry loop for an unsatisfied `accept`, `read` or `recv`: `while (!satisfied && low != high && now_ < hi) { step(); try again; }`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `./build/debug/tests/unit_tests/fixtures/scenario/Aloe.Tests.Unit.Scenario`
Expected: PASS, every scenario test.

- [ ] **Step 5: Commit**

```bash
git add fixtures/scenario/run/runner.hpp fixtures/scenario/run/runner.cpp tests/unit_tests/fixtures/scenario/test_runner.cpp
git commit -m "feat(scenario): bind the script's calls to the brick's verbs"
```

---

## Task 15: The scripts and the test binary that runs them

**Files:**
- Create: `tests/scenarios/tcp/handshake/passive-open.pkt`, `active-open.pkt`, `syn-retransmitted-on-schedule.pkt`, `syn-ack-retransmitted-on-schedule.pkt`, `syn-to-closed-port-is-reset.pkt`, `stray-ack-in-listen-is-reset.pkt`; `tests/scenarios/tcp/data/inbound-data-is-acked-at-flush.pkt`, `write-segments-by-peer-mss.pkt`, `duplicate-is-acked-at-once.pkt`, `above-a-gap-is-acked-not-stored.pkt`; `tests/scenarios/tcp/close/local-close-then-remote-fin.pkt`, `remote-fin-then-local-close.pkt`, `fin-retransmitted-on-schedule.pkt`; `tests/scenarios/tcp/reset/rst-in-established.pkt`, `rst-to-syn-is-refused.pkt`
- Create: `tests/scenarios/README.md`
- Create: `tests/unit_tests/scenarios/CMakeLists.txt`, `tests/unit_tests/scenarios/scenario_main.cpp`
- Modify: `tests/unit_tests/CMakeLists.txt` (add `scenarios`)

**Interfaces:**
- Consumes: Task 14.
- Produces: the binary `Aloe.Tests.Unit.Scenarios`, one gtest `Scenario.<relative path without extension>/<Checksums|NoOffloads>` per script, discovered under the compile definition `ALOE_SCENARIOS_DIR`; the fifteen scripts.

- [ ] **Step 1: Write the scripts**

Every script starts with a comment of two to four lines: what it asserts, and where Linux differs today, how. The sequence numbers are relative; `win` is written on every line it is known for, so the window arithmetic is pinned too. The files:

`tests/scenarios/tcp/handshake/passive-open.pkt`:

```
// A passive open: the peer's SYN, our SYN-ACK with the one option Aloe sends, the peer's ACK, accept.
// The SYN-ACK offers the whole byte budget, 32 segments of 1460; Linux would add sackOK, wscale and TS.
0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0 setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0
+0 bind(3, ..., ...) = 0
+0 listen(3, 1) = 0

+0 < S 0:0(0) win 65535 <mss 1460>
+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>
+0 < . 1:1(0) ack 1 win 65535
+0 accept(3, ..., ...) = 4
+0 close(3) = 0
```

`tests/scenarios/tcp/handshake/active-open.pkt`:

```
// An active open: our SYN with the one option, the peer's SYN-ACK, our ACK at the flush.
// connect returns at once, as a non-blocking connect does; the ACK is deferred so a commit could ride it.
0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0 fcntl(3, F_SETFL, O_RDWR|O_NONBLOCK) = 0
+0 connect(3, ..., ...) = -1 EINPROGRESS (Operation now in progress)
+0 > S 0:0(0) win 46720 <mss 1460>
+0 < S. 0:0(0) ack 1 win 65535 <mss 1460>
+0 > . 1:1(0) ack 1 win 46720
```

`tests/scenarios/tcp/handshake/syn-retransmitted-on-schedule.pkt`:

```
// No answer to our SYN: it goes again 1, 3, 7, 15 and 31 seconds after the first, and the connection times out
// at 63 seconds. Linux retries six times with the same doubling and gives up at 127 seconds.
0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0 fcntl(3, F_SETFL, O_RDWR|O_NONBLOCK) = 0
+0 connect(3, ..., ...) = -1 EINPROGRESS (Operation now in progress)
+0 > S 0:0(0) win 46720 <mss 1460>
+1 > S 0:0(0) win 46720 <mss 1460>
+2 > S 0:0(0) win 46720 <mss 1460>
+4 > S 0:0(0) win 46720 <mss 1460>
+8 > S 0:0(0) win 46720 <mss 1460>
+16 > S 0:0(0) win 46720 <mss 1460>
+32~+32.01 read(3, ..., 1) = -1 ETIMEDOUT
```

`tests/scenarios/tcp/handshake/syn-ack-retransmitted-on-schedule.pkt`:

```
// The peer's ACK is late: our SYN-ACK goes again after 1, 2 and 4 seconds, then the ACK completes the handshake.
0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0 bind(3, ..., ...) = 0
+0 listen(3, 1) = 0

+0 < S 0:0(0) win 65535 <mss 1460>
+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>
+1 > S. 0:0(0) ack 1 win 46720 <mss 1460>
+2 > S. 0:0(0) ack 1 win 46720 <mss 1460>
+4 > S. 0:0(0) ack 1 win 46720 <mss 1460>
+0 < . 1:1(0) ack 1 win 65535
+0 accept(3, ..., ...) = 4
```

`tests/scenarios/tcp/handshake/syn-to-closed-port-is-reset.pkt`:

```
// A SYN to a port nobody listens on: RST+ACK with sequence zero, acknowledging the SYN (RFC 9293, no connection).
// Nothing is bound or listening in this script.
+0 < S 0:0(0) win 65535 <mss 1460>
+0 > R. 0:0(0) ack 1 win 0
```

`tests/scenarios/tcp/handshake/stray-ack-in-listen-is-reset.pkt`:

```
// An ACK to a listening port with no connection: RST at the acknowledgement number. No ISN is known on either
// side, so the numbers here are absolute.
0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0 bind(3, ..., ...) = 0
+0 listen(3, 1) = 0

+0 < . 1:1(0) ack 100 win 65535
+0 > R 100:100(0) win 0
```

`tests/scenarios/tcp/data/inbound-data-is-acked-at-flush.pkt`:

```
// In-order data is acknowledged at the flush, with the window less the unread bytes; the read reopens it and the
// window update goes out at the next flush. Linux would delay the first ACK up to 40 ms and skip the update.
0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0 bind(3, ..., ...) = 0
+0 listen(3, 1) = 0

+0 < S 0:0(0) win 65535 <mss 1460>
+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>
+0 < . 1:1(0) ack 1 win 65535
+0 accept(3, ..., ...) = 4

+0 < P. 1:1001(1000) ack 1 win 65535
+0 > . 1:1(0) ack 1001 win 45720
+0 read(4, ..., 1000) = 1000
+0 > . 1:1(0) ack 1001 win 46720
```

`tests/scenarios/tcp/data/write-segments-by-peer-mss.pkt`:

```
// The peer's SYN carries an MSS of 1000: a write of 2500 leaves as three segments of 1000, 1000 and 500,
// all at once; the peer acknowledges them one by one, which the runner's queue model allows between the lines.
0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0 bind(3, ..., ...) = 0
+0 listen(3, 1) = 0

+0 < S 0:0(0) win 65535 <mss 1000>
+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>
+0 < . 1:1(0) ack 1 win 65535
+0 accept(3, ..., ...) = 4

+0 write(4, ..., 2500) = 2500
+0 > P. 1:1001(1000) ack 1 win 46720
+0 < . 1:1(0) ack 1001 win 65535
+0 > P. 1001:2001(1000) ack 1 win 46720
+0 < . 1:1(0) ack 2001 win 65535
+0 > P. 2001:2501(500) ack 1 win 46720
+0 < . 1:1(0) ack 2501 win 65535
```

`tests/scenarios/tcp/data/duplicate-is-acked-at-once.pkt`:

```
// A segment that arrives twice: the first waits for the flush, the second is answered at once with the same
// acknowledgement, and the application reads the bytes once.
0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0 bind(3, ..., ...) = 0
+0 listen(3, 1) = 0

+0 < S 0:0(0) win 65535 <mss 1460>
+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>
+0 < . 1:1(0) ack 1 win 65535
+0 accept(3, ..., ...) = 4

+0 < P. 1:1001(1000) ack 1 win 65535
+0 > . 1:1(0) ack 1001 win 45720
+0 < P. 1:1001(1000) ack 1 win 65535
+0 > . 1:1(0) ack 1001 win 45720
+0 read(4, ..., 2000) = 1000
+0 > . 1:1(0) ack 1001 win 46720
```

`tests/scenarios/tcp/data/above-a-gap-is-acked-not-stored.pkt`:

```
// A segment above a gap is not kept in this phase: it is answered at once with the acknowledgement at the gap,
// and the read sees only what came in order. Linux would hold it and answer with a SACK block (phase 2, step 4).
0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0 bind(3, ..., ...) = 0
+0 listen(3, 1) = 0

+0 < S 0:0(0) win 65535 <mss 1460>
+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>
+0 < . 1:1(0) ack 1 win 65535
+0 accept(3, ..., ...) = 4

+0 < P. 1:1001(1000) ack 1 win 65535
+0 > . 1:1(0) ack 1001 win 45720
+0 < P. 2001:3001(1000) ack 1 win 65535
+0 > . 1:1(0) ack 1001 win 45720
+0 read(4, ..., 3000) = 1000
+0 > . 1:1(0) ack 1001 win 46720
```

`tests/scenarios/tcp/close/local-close-then-remote-fin.pkt`:

```
// We close first: FIN, the peer's ACK, the peer's FIN, our ACK at once. The slot is released at Closed, so
// nothing follows. There is no TIME_WAIT in this phase.
0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0 bind(3, ..., ...) = 0
+0 listen(3, 1) = 0

+0 < S 0:0(0) win 65535 <mss 1460>
+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>
+0 < . 1:1(0) ack 1 win 65535
+0 accept(3, ..., ...) = 4

+0 close(4) = 0
+0 > F. 1:1(0) ack 1 win 46720
+0 < . 1:1(0) ack 2 win 65535
+0 < F. 1:1(0) ack 2 win 65535
+0 > . 2:2(0) ack 2 win 46720
```

`tests/scenarios/tcp/close/remote-fin-then-local-close.pkt`:

```
// The peer closes first: its FIN is acknowledged at the flush (Linux: delayed up to 40 ms), the read reports the
// end, our close sends the FIN, the peer's ACK ends the connection, and a stray segment after that is reset.
0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0 bind(3, ..., ...) = 0
+0 listen(3, 1) = 0

+0 < S 0:0(0) win 65535 <mss 1460>
+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>
+0 < . 1:1(0) ack 1 win 65535
+0 accept(3, ..., ...) = 4

+0 < F. 1:1(0) ack 1 win 65535
+0 > . 1:1(0) ack 2 win 46720
+0 read(4, ..., 1000) = 0
+0 close(4) = 0
+0 > F. 1:1(0) ack 2 win 46720
+0 < . 2:2(0) ack 2 win 65535
+0 < . 2:2(0) ack 2 win 65535
+0 > R 2:2(0) win 0
```

`tests/scenarios/tcp/close/fin-retransmitted-on-schedule.pkt`:

```
// The peer does not acknowledge our FIN: it goes again after 1 and 2 seconds, then the ACK arrives and the
// connection waits in FIN-WAIT-2 for the peer's FIN, sending nothing.
0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0 bind(3, ..., ...) = 0
+0 listen(3, 1) = 0

+0 < S 0:0(0) win 65535 <mss 1460>
+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>
+0 < . 1:1(0) ack 1 win 65535
+0 accept(3, ..., ...) = 4

+0 close(4) = 0
+0 > F. 1:1(0) ack 1 win 46720
+1 > F. 1:1(0) ack 1 win 46720
+2 > F. 1:1(0) ack 1 win 46720
+0 < . 1:1(0) ack 2 win 65535
```

`tests/scenarios/tcp/reset/rst-in-established.pkt`:

```
// A RST on an established connection: the read reports it, the slot is released, and a later segment from the
// peer is answered with a RST of our own, since nothing remembers the connection.
0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0 bind(3, ..., ...) = 0
+0 listen(3, 1) = 0

+0 < S 0:0(0) win 65535 <mss 1460>
+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>
+0 < . 1:1(0) ack 1 win 65535
+0 accept(3, ..., ...) = 4

+0 < R. 1:1(0) ack 1 win 65535
+0 read(4, ..., 1000) = -1 ECONNRESET
+0 < . 1:1(0) ack 1 win 65535
+0 > R 1:1(0) win 0
```

`tests/scenarios/tcp/reset/rst-to-syn-is-refused.pkt`:

```
// A RST answering our SYN: the connection is refused, as connect reports ECONNREFUSED on Linux.
0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0 fcntl(3, F_SETFL, O_RDWR|O_NONBLOCK) = 0
+0 connect(3, ..., ...) = -1 EINPROGRESS (Operation now in progress)
+0 > S 0:0(0) win 46720 <mss 1460>
+0 < R. 0:0(0) ack 1 win 0
+0 read(3, ..., 1) = -1 ECONNREFUSED
```

`tests/scenarios/README.md`: one paragraph: these are Aloe's own scenario scripts in packetdrill's syntax, run by `Aloe.Tests.Unit.Scenarios` in both checksum modes of the fabric, with a pointer to `docs/architecture/scenario.md` for the grammar, the bindings and how to run one.

- [ ] **Step 2: Write the test binary and register it**

`tests/unit_tests/scenarios/scenario_main.cpp`:

```cpp
#include <aloe/fabric>
#include <aloe/scenario>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

// Every .pkt under ALOE_SCENARIOS_DIR becomes one test per checksum mode of the fabric, named after its path:
//
//     ./Aloe.Tests.Unit.Scenarios --gtest_filter='Scenario.tcp/close/*'
//
namespace {

    class ScriptTest : public ::testing::Test {
    public:
        ScriptTest(std::filesystem::path path, const aloe::fabric::EmulatedOffloads offloads)
            : path_{std::move(path)},
              offloads_{offloads} {
        }

        void TestBody() override {
            std::ifstream file{path_};
            ASSERT_TRUE(file.is_open()) << "cannot open " << path_;
            const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
            const auto script = aloe::scenario::parse_script(text);
            ASSERT_TRUE(script.has_value()) << path_.string() << ":" << script.error().line << ":" << script.error().column
                                            << ": " << script.error().message;
            aloe::scenario::HarnessConfig config;
            config.offloads = offloads_;
            aloe::scenario::Harness harness{config};
            aloe::scenario::Runner runner{harness, *script};
            const auto result = runner.run();
            EXPECT_TRUE(result.has_value()) << aloe::scenario::format(result.error(), path_.string());
        }

    private:
        std::filesystem::path path_;
        aloe::fabric::EmulatedOffloads offloads_;
    };

    void register_scripts(const std::filesystem::path& root) {
        std::vector<std::filesystem::path> scripts;
        for (const auto& entry : std::filesystem::recursive_directory_iterator{root}) {
            if (entry.is_regular_file() && entry.path().extension() == ".pkt") {
                scripts.push_back(entry.path());
            }
        }
        std::ranges::sort(scripts);
        for (const auto& path : scripts) {
            const std::string name = std::filesystem::relative(path, root).replace_extension().generic_string();
            for (const auto [offloads, mode] : {std::pair{aloe::fabric::EmulatedOffloads::None, "NoOffloads"},
                                                std::pair{aloe::fabric::EmulatedOffloads::Checksums, "Checksums"}}) {
                ::testing::RegisterTest("Scenario",
                                        (name + "/" + mode).c_str(),
                                        nullptr,
                                        nullptr,
                                        path.c_str(),
                                        1,
                                        [path, offloads = offloads]() -> ::testing::Test* { return new ScriptTest{path, offloads}; });
            }
        }
    }

}  // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    register_scripts(ALOE_SCENARIOS_DIR);
    return RUN_ALL_TESTS();
}
```

`tests/unit_tests/scenarios/CMakeLists.txt`:

```cmake
##############################################################################
# The scenario scripts: one gtest per script and checksum mode, discovered under tests/scenarios at run time
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Scenarios
        scenario_main.cpp
)
target_compile_definitions(${UNIT_TESTING_TARGET}.Scenarios
        PRIVATE
        ALOE_SCENARIOS_DIR="${CMAKE_SOURCE_DIR}/tests/scenarios"
)
target_link_libraries(${UNIT_TESTING_TARGET}.Scenarios
        PRIVATE
        Aloe::Fixtures::Scenario
        Aloe::Fixtures::Fabric
        GTest::gtest
        GTest::gmock
)
##############################################################################
```

(`GTest::gtest_main` is left out: the binary has its own `main`.) `tests/unit_tests/CMakeLists.txt` gains `add_subdirectory(scenarios)` after `fixtures`.

- [ ] **Step 3: Run the scripts**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Scenarios && ./build/debug/tests/unit_tests/scenarios/Aloe.Tests.Unit.Scenarios`
Expected: 30 tests, all PASS. A failing script prints `tests/scenarios/tcp/<area>/<name>.pkt:<line>: <message>`; when one fails, decide whether the script or the runner is wrong by reading the stack's code the "Verified Beforehand" row cites: the scripts assert today's TCP, and the plan's author derived each line from `tcp_stack.hpp`; a wrong line is corrected in the script with a note in the commit message, a wrong runner is fixed in Task 13 or 14's files.

Then `ctest --preset debug` for everything, and `./build/debug/tests/unit_tests/scenarios/Aloe.Tests.Unit.Scenarios --gtest_filter='Scenario.tcp/close/*'` to see the filter work.

- [ ] **Step 4: Commit**

```bash
git add tests/scenarios tests/unit_tests/scenarios tests/unit_tests/CMakeLists.txt
git commit -m "test(scenario): the first fifteen scripts, and the binary that runs every script in both checksum modes"
```

---

## Task 16: Documentation, the roadmap, the repository notes and the completion gates

**Files:**
- Modify: `docs/architecture/device.md`, `docs/architecture/fixtures.md`, `docs/architecture/loop.md`, `docs/roadmap.md`, `docs/guides/getting-started.md`, `README.md`, `AGENTS.md`
- Create: `docs/architecture/scenario.md`

**Interfaces:**
- Consumes: everything above.
- Produces: the public record of the loan, the impairment and the runner; the closing comment for issue #17.

- [ ] **Step 1: `docs/architecture/device.md`**

- The `IsPacket` bullet gains: "`lend()` returns a second handle over the same buffer for the device to transmit, and `lent()` is true while that handle is alive: the loan. A lent packet is read, never written; a device asked to fill checksums may complete them in the lent buffer; dropping the lender while lent frees the buffer once, when the handle goes. `lent()` is a poll."
- The `IsDevice` bullet gains: "`reclaim(queue)` asks the driver to free transmitted packets it still holds and returns how many; `capabilities().tx_reclaim` says whether the driver has the call."
- The `Capabilities` bullet gains `tx_reclaim`.
- The fabric backend bullet gains: "A transmitted loan is held in the port's in-flight list under `PortConfig::loans`: `OnStep` (the default) finishes at `Fabric::advance` and is released by the queue's next transmit, receive or reclaim, with `reclaim` releasing at once; `Held` releases only on `return_loans`, the driver that will not let go. Impairment and the clock are on the [fixtures page](fixtures.md)."
- The ethdev backend bullet gains: "The loan is the mbuf's reference count, raised by `lend` and read by `lent`; `reclaim` is `rte_eth_tx_done_cleanup`, probed once after the start, which the ring, null and tap drivers lack and the Intel drivers have. `receive` copies an mbuf that arrives shared, so the ring driver's loopback cannot move a lender's offsets; a card never delivers one."
- Design notes gain three paragraphs: **The loan.** (why on the packet, not a ticket: the ring and `transmit` need no change; what each backend does; the three CI drivers return at transmit or when the reader lets go, and only the fabric's `Held` policy models a card that holds a frame long after it left). **A shared mbuf is copied on receive.** (the ring hazard, the rule, the cost: one load and one untaken branch per packet on hardware; a failed copy counts in `dropped`). **Fast-free stays off.** (the `static_assert` on the mask and why).
- Replace "Scripted loss, reordering and delay arrive in a later phase; the delivery function is the one place they plug in." with "Scripted impairment, loss, duplication, reordering and delay, sits on a port's egress; see the [fixtures page](fixtures.md)."

- [ ] **Step 2: `docs/architecture/fixtures.md`, `loop.md`, `README.md`, `AGENTS.md`**

`fixtures.md`:

- The table gains `| scenario | #include <aloe/scenario> | Aloe::Fixtures::Scenario | Packetdrill-style scripts run against the TCP brick on the fabric. See [scenario](scenario.md). |`.
- A section **Impairment and the clock** after the usage example: the `Impairment` fields and their meanings, the order script then model, the determinism rule (raw engine bits, the same seed under clang and GCC), `ImpairmentCounters`, `Fabric::advance(now)` as the step that delivers delayed frames, releases reorder holds and finishes `OnStep` loans, with the delay measured from the last `advance`, and the loan policies. State that a test loop with delay or loans advances the fabric at every tick, as `TcpHarness::advance` and the two-stack test do, and that `advance` is safe from any thread.
- The design notes gain: the pool and the block marks stay the queue thread's; `advance` touches only the finished counts; a dropped lender orphans its block; the `Pool` destructor assertion is the check that every loan came back.

`loop.md`: the `ShardQueue` bullet gains "`reclaim()` asks the device to free finished transmits on this queue".

`README.md`: the fixtures table gains `| aloe::scenario | fixtures/scenario/ | Packetdrill-style scenario scripts run against the TCP brick on the fabric: the parser, the runner and the harness. |`.

`AGENTS.md`:

- The introduction's fixtures sentence: "the fixtures (`fabric`, an in-memory device backend, `frames`, for tests, and `scenario`, a runner for packetdrill-style scripts)".
- The layout row for `fixtures/<module>/` names `scenario` with the others.
- The namespace rule's list gains `aloe::scenario`.
- "Traps already found" gains:
  - "None of DPDK's ring, null and tap drivers returns a transmitted mbuf lazily: null and tap free it inside the transmit call, the ring hands the very mbuf to the receiver. A packet that stays lent after its frame left exists on CI only on a fabric port with `LoanReturn::Held`; `ethdev::Port::receive` copies a shared mbuf, so a lent packet never reaches a ring reader as itself."
  - "A fabric with delayed frames or loans on `OnStep` ports needs `Fabric::advance(now)` from the test's loop, once per tick; nothing is delivered or finished without it, and a loan never advanced stays out until the port is destroyed, which the pool's assertion reports."
  - "`std::uniform_real_distribution` gives different numbers on libstdc++ and libc++ for the same engine; the fabric's impairment turns the engine's raw bits into a double itself, and a test of its determinism must not use the distribution either."
  - "The scenario scripts binary registers its tests at run time, so `ctest` lists one entry for it; use `--gtest_filter='Scenario.tcp/<area>/*'` on the binary to run one area."

- [ ] **Step 3: `docs/architecture/scenario.md`**

A new page in the pattern of the others (what it is, key types, usage, design notes), covering:

- What: Aloe's own scenario scripts in packetdrill's syntax, run against `tcp::Stack` over `net::Ipv4<fabric::Port>` on the fabric, without root; Linux's corpus is the reference for what Linux does and nothing from it is copied in (it is GPL-2.0; Aloe is BSD-3-Clause).
- Key types: `Script`, `parse_script`, `ParseError`; `Harness`, `HarnessConfig` (the defaults: addresses, ports 8080 and 40000, the peer ISN, both checksum modes, the hour-long ARP lifetimes); `Runner`, `RunnerConfig`, `Failure`, `format`; `Peer` and `ObservedSegment`; `match`; the notation functions.
- The grammar subset, as the spec's "The script language" section lists it, and what fails at parse time.
- The call bindings, as Task 14's table, with the errno words.
- Time and matching, as the spec's sections say them: the step, actions at nominal time, outbound windows and the tolerance, the queue model with the three ways an unexpected segment is caught, the matching rule, the checksum verification in both modes.
- Writing and running a script: the file lives under `tests/scenarios/tcp/<area>/`, starts with a comment saying what it asserts and where Linux differs, writes `win` where it is known; `cmake --build --preset debug --target Aloe.Tests.Unit.Scenarios && ./build/debug/tests/unit_tests/scenarios/Aloe.Tests.Unit.Scenarios --gtest_filter='Scenario.tcp/close/*'`; a failure prints `path:line: message` with both segments in the script's notation.
- The first set: the fifteen scripts by area and what each pins, and the note that later steps change them as sackOK, wscale, timestamps and delayed ACKs land.
- Design notes: why a fixture module and not `tests/shared/` (reusable by anyone testing the brick, no gtest inside); why the runner's clock is virtual and steps at the wheel's resolution; why unbound calls fail loudly.

- [ ] **Step 4: `docs/roadmap.md` and `docs/guides/getting-started.md`**

`docs/roadmap.md`, step 2 of phase 2: replace "Scripted loss, reordering, delay and duplication in the fabric, and a runner for packetdrill-style scenario scripts. TCP's behaviour does not change in this step." with "Scripted loss, duplication, reordering and delay in the fabric, and a runner for scenario scripts in packetdrill's syntax: Aloe's own scripts, with Linux's corpus as the reference for what Linux does. TCP's behaviour does not change in this step. Landed; the loan is a device and loop capability until step 3 lends from `commit`."

`docs/guides/getting-started.md`, after the paragraph on `ctest` labels: a paragraph naming `Aloe.Tests.Unit.Scenarios`, that it registers one test per script under `tests/scenarios/` and per fabric checksum mode, and the `--gtest_filter` form to run one area or one script.

- [ ] **Step 5: The completion gates**

Run, each from a clean configure where the preset was not configured before:

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset gcc-debug && cmake --build --preset gcc-debug && ctest --preset gcc-debug
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
cmake --preset debug -DDO_BENCHMARKS=ON && cmake --build --preset debug && ctest --preset debug -R Benchmark
./scripts/check-format.sh
sudo ./build/debug/tests/manual_tests/ethdev/Aloe.Tests.Manual.Ethdev.Tap --gtest_filter='*Loan*'
```

Expected: every command passes; the tap run prints the loan test passing (it needs `CAP_NET_ADMIN`; run it on the machine where the branch is finished, once). Record the outputs' last lines in the pull request description.

- [ ] **Step 6: Commit, push and open the pull request**

```bash
git add docs README.md AGENTS.md
git commit -m "docs: the packet loan, the fabric's impairment and clock, and the scenario module"
git push -u origin device/packet-loan
```

Pull request title: `[DEVICE][ETHDEV][FABRIC][LOOP][FRAMES][SCENARIO] Packet loan, scripted impairment and a scenario-script runner`. Description, in a few lines: what changed and why (the loan on both backends, the receive copy of a shared mbuf, impairment and the fabric's clock, the scenario module and the first fifteen scripts); how it was verified (the gates above, the tap run); the premises of issue #17 that failed (the licence, so the scripts are Aloe's own; no CI driver returns lazily; reader discipline replaced by the receive copy; TCP untouched until #18).

Then, if `gh` is authenticated, the closing comment on issue #17 (else leave its text in the pull request description for the author to post):

> Landed in #<PR>. Against the issue's text: the scripts are Aloe's own in packetdrill's syntax, since packetdrill's corpus and the kernel's in-tree scripts are GPL-2.0 and Aloe is BSD-3-Clause; none of the ring, null and tap drivers returns a transmitted mbuf lazily (null and tap free at transmit, the ring hands the very mbuf to the receiver), so the "still lent" case is a fabric port with `LoanReturn::Held`; the ring hazard is closed by `ethdev::Port::receive` copying a shared mbuf, not by reader discipline; `commit` does not lend yet, that is #18. The device conformance suite covers the loan on both backends; impairment and the clock step are in the fabric; `Aloe.Tests.Unit.Scenarios` runs fifteen scripts in both checksum modes.

---

## Coverage map

| Spec section | Task |
|---|---|
| The concept: `lend`, `lent`, the contract, `reclaim`, `tx_reclaim` | 1, 2, 3, 5 |
| Ethdev: the count, the probe, the receive copy, fast-free | 1, 2 |
| The fabric: marks, orphan, in-flight list, `OnStep` and `Held`, `reclaim`, `return_loans`, `loans_outstanding` | 3, 4 |
| The loop: `ShardQueue::reclaim`; `Ipv4` unchanged | 5 |
| What #18 does with it | 16 (recorded on the device page's design notes) |
| Impairment: the model, the script, the counters, determinism | 6 |
| The fabric's clock: `advance`, delay from the last stamp, holds, thread safety | 4, 6 |
| The scenario module and layout; one test binary; scripts under `tests/scenarios/` | 9, 15 |
| The script language | 9, 10 |
| The binding to the brick; the errno words; the ARP lifetimes | 12, 13, 14 |
| Time: the step, nominal actions, windows, retries, the queue model | 13, 14 |
| Matching, checksums in both modes, the failure text | 11, 12, 13 |
| The first set of scripts | 15 |
| Testing: conformance, ethdev, fabric, two stacks, scenario, harnesses | 2, 4, 5, 6, 7, 9 to 15 |
| Documentation and the roadmap | 16 |
| Delivery and the gates | 16 |

## Plan handoff

Execute with superpowers:subagent-driven-development, one task per subagent in order, a fresh reviewer after each with the checklist under "How to Execute and Review", and a whole-branch review before Task 16's gates. Tasks 1 to 4 are independent of each other and could run in parallel worktrees; Task 5 needs all four; Tasks 9 to 15 are a chain. The spec travels with the plan: a reviewer who doubts a script line in Task 15 reads the "Verified Beforehand" row on today's TCP and `common/tcp/stack/tcp_stack.hpp`, never changes TCP to make a script pass.
