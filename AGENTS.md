# AGENTS.md

Guidance for coding agents, and for people, working in this repository.

Aloe is a C++26 userspace TCP/IP stack on DPDK for Linux. It is two products:
bricks, plain calls a program writes its own loop over, and a runtime built only
from the bricks that writes the loop for you and offers senders and receivers
(stdexec) for the code that waits. The repository holds the build skeleton, the
foundation (`core`, with `execution` and `log` beside it), the protocol formats
(`wire`), the device layer (the packet and device concepts and the DPDK backend `ethdev`), the
fixtures (`fabric`, an in-memory device backend, `frames`, for tests, and `scenario`, a runner for
packetdrill-style scripts), the loop bricks (`loop`), the shard runtime (`runtime`), the IP base
(`net`: Ethernet, ARP, IPv4 and ICMP echo, the first protocol brick), the stream contract (`stream`) and
minimal TCP (`tcp`), with its senders in the runtime. The stack is built in phases: shard runtime,
minimal TCP, full TCP, TLS, HTTP/1.1 with WebSocket.

## Build and test

```bash
cmake --preset debug             # first run clones vcpkg and builds DPDK: about five minutes
cmake --build --preset debug
ctest --preset debug             # all tests
ctest --preset debug -L unit     # or: integration, functional
./scripts/check-format.sh        # every .cpp/.hpp against .clang-format
```

- Every configure, build and test preset shares one name. The full list is in
  `CMakePresets.json` and `docs/guides/getting-started.md`.
- A change is done when `debug`, `gcc-debug`, `asan` and `tsan` all build and pass,
  and the format check passes. PR CI builds the `ci` preset only, so the sanitizer
  and GCC runs are local. clang-tidy runs inside the clang builds and its
  warnings are errors, so a clean clang build is the lint check.
- A pre-commit hook (`.githooks/pre-commit`) runs `./scripts/check-format.sh --staged`
  on the staged `.cpp`/`.hpp` files. The first configure switches it on by setting
  `core.hooksPath`, unless one is already set. CI runs `./scripts/check-format.sh` on
  every pull request, so `git commit --no-verify` does not get past it.
- Tests need no root, no hugepages and no network card. Keep it that way.
- `build/` and `vcpkg/` are generated and git-ignored.

## Layout

| Path                                                                       | Contents                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
|----------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `common/<module>/`                                                         | Stack and foundation modules. `core` holds the small things every module links, with no dependencies (version, the clock, header-only helpers); `execution` is the one header that names stdexec and `log` the one that names quill, both linked on demand; `wire` (addresses, headers and checksums grouped by protocol: `ethernet/`, `ipv4/`, `arp/`, `icmp/`, `tcp/`; values only, no state); `device` (concepts, no DPDK), `ethdev` (DPDK backend, the only module that links DPDK), `loop` (the bricks: device queue, timer wheel, work node, run queue, inbox, counters; no stdexec, no DPDK), `runtime` (shards, scheduler, scope, task, runtime, the stream senders and the ready-made `TcpStack`; built on `loop`, no DPDK), `net` (the IP base: the ARP cache and the `Ipv4<Device>` brick over `wire`'s formats; built on `loop` and `device`, no `execution`, no `log`, no DPDK), `stream` (the zero-copy stream concepts, events and errors; header-only, links `core` only), `tcp` (the minimal TCP brick `tcp::Stack<Ip>` over `net`, its flow table and held-packet pool; no `execution`, no `log`, no DPDK). |
| `fixtures/<module>/`                                                       | What Aloe ships for testing code written over the bricks, kept apart from the stack: `fabric` (the in-memory device backend, with scripted impairment, loan policies and a clock step), `frames` (frame builders and a parser) and `scenario` (packetdrill-style scripts run against the TCP brick on the fabric: the parser, the runner and the harness; no gtest). Built on `common`; nothing in `common` depends on them.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| `component/<module>/`                                                      | Protocol modules (HTTP/1.1, HTTP/2, HTTP/3, WebSocket, ...), each an independent unit built on `common`. None exist yet; the directory appears with the first.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| `<module>/export/aloe/<module>`                                            | Umbrella header, no extension. Consumers write `#include <aloe/<module>>`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| `tests/unit_tests/`, `tests/integration_tests/`, `tests/functional_tests/` | One CTest label each.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| `tests/manual_tests/`                                                      | Label `manual`: tests that need privileges or hardware. Every test preset excludes the label; run the binary by hand.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| `tests/scenarios/`                                                         | The scenario scripts, `tcp/<area>/<name>.pkt`, which `Aloe.Tests.Unit.Scenarios` finds at run time and runs in both checksum modes of the fabric.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| `tests/shared/`                                                            | Helpers for Aloe's own test binaries: the unprivileged EAL arguments, the EAL as a gtest environment, the net and runtime fixtures, and the device conformance suite every backend runs.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| `cmake/`                                                                   | `vcpkg-bootstrap.cmake` (the toolchain file), `dpdk.cmake`, test and library helpers.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| `triplets/`                                                                | The vcpkg overlay triplet every port is built with.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| `examples/`                                                                | Living examples.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| `benchmarks/`                                                              | `benchmarks/tick_to_trade/`: the baseline benchmark, a harness library and a binary in `aloe::benchmarks`, run by hand in `release` on hugepages; its tests live under `tests/*/benchmarks/` and need neither.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |

Targets are named `Aloe.<Group>.<Module>` with an `Aloe::<Group>::<Module>`
alias; the groups are `Common`, `Fixtures` and `Benchmarks` (`Aloe.Benchmarks.TickToTrade`, over the harness
library `Aloe::Benchmarks::Harness`). Register tests with `add_unit_test`, `add_integration_test`,
`add_functional_test` or `add_manual_test` from `cmake/tests.cmake`.

Every module's public headers sit on one flat include path, so header
basenames are unique across modules: `packet.hpp` is the concept,
`fabric_packet.hpp` and `ethdev_packet.hpp` are the backends' types.

## Rules

- **Execution facilities.** Write `aloe::execution::ex::` for senders, receivers
  and schedulers, `aloe::execution::task<T>` for coroutine tasks, and
  `aloe::execution::TaskEnvironment` for a task bound to a concrete scheduler.
  Only `common/execution/execution/execution.hpp` may name `stdexec::` or `exec::`.
- **Logging.** Only `common/log/log/log.hpp` may name `quill::`. Everything else
  takes a `log::Logger` from `log::logger(name)` and writes
  `log.info<"text {}">(value)`. Nothing logs on a hot path.
- **Time.** A stamp is `core::TimePoint`, an interval `core::Duration`, the clock
  `core::Clock`. A class does not declare its own clock aliases. `core::Clock` is the TSC
  clock unless the build sets `USE_TSC_CLOCK=OFF`, so a `std::chrono::steady_clock` stamp does
  not compile where the stack expects one. A program that reads `core::Clock` in its own loop
  calls `core::calibrate_clock()` during setup.
- **DPDK.** Link the target `Aloe::Dpdk` and nothing else. Linking DPDK's
  pkg-config data directly builds a program that starts with no drivers.
- **Dependencies.** Libraries come from `vcpkg.json` only: no system
  libraries, no fetched sources, no submodules. System packages are for build
  tools, installed by `scripts/install-linux.sh`.
- **No C++ modules.** `CMAKE_CXX_SCAN_FOR_MODULES` stays `OFF`; with it on,
  GCC 16 crashes inside stdexec.
- **Public headers define no macros.** Implementation details go in a nested
  `detail` namespace.
- **One class, one place.** A class's members are defined in its body, or in its
  one `.cpp`. One class's definitions are never spread over several headers, and
  no header is included after another's namespace closes to finish it; a class
  that outgrows its file is split into classes with responsibilities of their
  own, each in its own file.
- **Style.** `codestyle.md` covers what `.clang-format` does not: naming,
  namespaces, parameters, attributes, error handling, include order.
- **Namespaces.** Every module gets a namespace named after it: `aloe::core`,
  `aloe::execution`, `aloe::log`, `aloe::wire`, `aloe::device`, `aloe::fabric`, `aloe::frames`, `aloe::scenario`,
  `aloe::ethdev`, `aloe::loop`, `aloe::runtime`, `aloe::net`, `aloe::stream`, `aloe::tcp`; the benchmarks live in
  `aloe::benchmarks`. Nothing is declared directly in `aloe`. Another module's names are qualified with its
  namespace (`wire::MacAddress` inside `aloe::fabric`), never pulled in with
  `using namespace`. Test helpers live in `aloe::testing`.
- **stdexec and quill stay out of hot-path headers.** Only the runtime, the
  examples and the tests include `<aloe/execution>` or `<aloe/log>`; a device or
  protocol header takes helpers from `<aloe/core>` and the loop's bricks from
  `<aloe/loop>`. No module below the runtime links `Aloe::Common::Execution` or
  `Aloe::Common::Log`, so a brick cannot include either by accident.
- **Bricks before runtime.** A capability lands in `loop` or a protocol module
  first, as a plain call or an event the caller drains, and the runtime wraps it
  in a sender afterwards. The runtime never has a capability the bricks lack,
  and the docs and examples lead with the bricks.
- **Error handling.** `std::expected` on hot paths, exceptions on setup and
  cold paths.

## Traps already found

- clang-tidy reports `readability-static-accessed-through-instance` on every
  `co_await` in a stdexec task. The check is disabled in `.clang-tidy`; do not
  re-enable it.
- clang-tidy rejects `std::move` on a sender whose type is trivially copyable.
  Pass the sender expression directly.
- `stdexec::get_completion_behavior` is deprecated and fails under `-Werror`;
  alias the `exec::` spelling in core.
- A type with a `std::atomic` member is immovable; `Work` is one, so containers
  of operation states use `std::unique_ptr` or `std::deque`.
- clang-tidy's `cppcoreguidelines-pro-type-static-cast-downcast` fires on every
  `static_cast` from `Work&` or `Timer&` to the operation state. `.clang-tidy`
  sets the check's `StrictMode` to `false`, so it flags only polymorphic
  downcasts; do not re-enable strict mode.
- clang-tidy's `readability-make-member-function-const` fires on a member that
  only writes through a pointer member. Call `core::force_non_const(this)`
  first when the type owns what the pointer reaches (`ethdev::Packet`,
  `ShardQueue`); make the member `const` when the type is a non-owning handle
  (`log::Logger`).
- clang-tidy's `readability-convert-member-functions-to-static` fires on stdexec
  `query` members that never touch `this`. Make them `static`; stdexec calls
  `env.query(tag)` either way.
- clang-tidy's `cert-msc51-cpp` fires on a fixed random seed. A reproducible
  test keeps the seed with a one-line `NOLINT` and its reason.
- A local class cannot declare member templates, so a test sender with a
  templated `connect` goes in the anonymous namespace, not inside the test body.
- GCC's `-Wshadow` warns when a constructor parameter shadows a member function
  (`device` against `device()`). Rename the parameter, keep the member.
- Inside a class template, a call to a member template of a dependent object
  needs `.template`: `logger().template info<"...">(...)`.
- `log::Logger::flush` spins forever when no `Logging` is alive; quill waits for
  the backend to acknowledge the flush.
- A shard task awaits every sender directly, with no `affine` wrap; it awaits
  only its own shard's senders and child tasks (threading contract rule 4), and
  the scope asserts a completion arrives on the thread that first spawned.
- One process can start DPDK's EAL once. Keep all EAL checks of a test binary
  in one test, or fork, as `test_eal_concurrent.cpp` does. Tests of the DPDK
  backend start it in a gtest environment, `aloe::testing::EalEnvironment`.
- A closed DPDK port cannot be reopened. A test that constructs and destroys an
  `ethdev::Port` probes a fresh virtual device with `aloe::testing::probe_vdev`.
- The ring driver (`net_ring`) sets no MTU and has no RSS; the null driver
  (`net_null`) has both. Ethdev tolerates `-ENOTSUP` from either.
- `EXPECT_THROW(std::ignore = T{.a = 1, .b = 2}, E)` needs the expression in
  an extra pair of parentheses, or the macro splits it at the commas.
- DPDK is built for the generic x86-64 baseline (`-Dplatform=generic` in the
  triplet), so a cached build starts on any CPU. `cmake/dpdk.cmake` refuses a
  DPDK built with `-march=native`.
- CMake wraps long `message()` text at a width that depends on the paths in
  it. Tests that match configure output collapse whitespace first.
- Debug executables that link DPDK are about 200 MB, because every driver is
  linked whole.
- `loop::ShardQueue::transmit(Packet&&)` moves the packet only on success. Code that uses the packet
  after a refused transmit is right, and clang-tidy's `bugprone-use-after-move` flags it anyway; mark
  the lines `NOLINT(bugprone-use-after-move)` with that reason, as `net::Ipv4::send` does.
- The fabric never refuses a transmit and refuses frames shorter than an Ethernet header. A test that
  needs a refused send wraps the port in a device whose `transmit` returns 0; a test of a short frame
  builds the packet on the port's own pool and calls the brick directly.
- `ShardQueue::transmit` flushes once when its ring is full before it refuses, so a refusing device alone
  does not refuse the next send. A test that needs one gives the queue a one-slot ring, fills it with a
  packet the device will not take, then sends (`aloe::testing::TcpRefusingFixture::refuse`).
- clang-tidy's `modernize-make-unique` flags `new T[n]`, and `std::make_unique<T[]>` and
  `std::deque::emplace_back` construct through code that is not a friend, so a private constructor does not
  work. A type only one class may create takes a passkey instead: a public constructor whose parameter is a
  private tag type only that class names, as `tcp::Connection` (built by `tcp::Stack` in a `std::deque`,
  which never moves an element) and `fabric::Port` do.
- A member function of a class template that is declared but never defined fails only once something
  instantiates it, at link time. Define every function a template member reaches (`tcp::Stack::process`
  reaches the whole receive path) in the change that first instantiates it.
- `frames::Checksums::Wrong` corrupts both checksums in `ipv4_frame` but only the TCP checksum in
  `tcp_frame`: the IPv4 header stays right, so the segment reaches TCP and counts as a bad TCP checksum
  instead of being dropped by IP.
- A view whose member `size()` counts bytes, not elements, must specialise `std::ranges::disable_sized_range`,
  or `std::views::take`, `std::ranges::distance` and `std::ranges::to` trust the byte count as an element
  count and walk past the end. `stream::IsReadView` rejects any sized range, so a read view must have
  forward, not random-access, iterators.
- A `loop::TimerWheel` advanced once past several deadlines fires a re-arming timer once, not once per
  deadline it passed: the re-armed deadline is computed from the new stamp. A test of a retry schedule steps
  the clock, at most one retry interval per `advance`, and checks each deadline, or it sees one retry where a
  real loop sees several.
- DPDK's tap driver points an rt-signal at its queue descriptors, so every frame the kernel writes to the tap
  interrupts some thread of the process. The handler has `SA_RESTART`, which restarts `recv` and `send` but never
  `poll`: a test thread polling a kernel socket gets `EINTR` and must retry until its deadline, as
  `readable_by` in `test_tcp_tap.cpp` does.
- Fedora's firewalld puts the tap's kernel interface in its default zone, which opens only TCP ports 1025-65535.
  A kernel listener on a lower port never sees the stack's SYN, while connections the kernel opens pass as
  established traffic, so only the client direction fails. A tap test or example whose kernel end listens
  uses a port above 1024.
- A build with `USE_TSC_CLOCK=ON`, the default, refuses to start on a machine whose CPU has no invariant TSC or
  whose kernel clocksource is not `tsc`; most virtual machines fail the second check. CI's runners are such
  machines, so the `ci` preset turns the option off and the tests that read the real TSC skip there: a change
  to `TscClock` is verified by the local presets only. A developer on a VM configures with
  `-DUSE_TSC_CLOCK=OFF`.
- The first `TscClock::now()` in a process calibrates for 100 ms, and a refusal there terminates the program,
  since `now` cannot throw. Setup code calls `core::calibrate_clock()` first; a test that races threads on the
  clock calibrates before it starts them.
- A `net_ring` virtual device loops each queue's transmit back onto its own receive, so two parties cannot talk
  over one. Two ports built with `rte_eth_from_rings` over cross-connected rings can, as `benchmarks::RingPair`
  builds them; such a port has the MAC `00:00:00:00:00:00`, which `Ipv4::process` accepts like any other.
- `rte_eth_from_rings` registers its port as `net_ring_<name>` and fails with `ENAMETOOLONG` when that passes the
  28 characters DPDK allows a ring's name. `benchmarks::RingPair` appends `-s` and `-p` for its ports and
  `-to-stack-<queue>` for its rings, so a `RingPairConfig::name` stays within 15 characters.
- DPDK's per-thread mempool cache does not amortise a one-way flow: a thread that only allocates refills its
  cache in bulk each time it runs dry, and a thread that only frees flushes it each time it passes 1.5 times its
  size, a ring operation every few hundred mbufs. Mbufs that cross cores on a measured path go back the way they
  came (`benchmarks::VenuePeer` sends every frame in the mbuf the stack's last frame arrived in), so each core
  takes from and returns to its own pool in balance.
- `tcp::Stack` resets its count of full-sized segments whenever it sends a segment that carries an ACK, and
  `flush` sends the ACK a data segment queued, so the immediate ACK of every second full-sized segment (RFC 5681)
  fires only when two full segments reach one `process` call. A closed loop whose replies carry the ACK never
  sees it; a test that wants it delivers two full segments in one burst.
- The EAL pins the main thread to the main lcore of `-l`, and every thread created after it inherits that mask.
  A thread meant to run elsewhere pins itself, or widens its mask to every CPU as `benchmarks::widen_to_all_cpus`
  does; otherwise it shares the main lcore's core with whatever spins there.
- A stack whose peer runs inline on its thread cannot resolve ARP on the wire: the peer answers the request while
  it waits for the next handshake segment, but the stack sends that segment only after it processes the reply, and
  it cannot step while the peer waits. `tick_to_trade` refuses `--arp-on-wire` with `--topology same` or `both`.
- `tick_to_trade`'s two pools of 8192 mbufs do not fit the 64 MB the EAL takes under `--no-huge` by default: a
  run without hugepages passes `-m 128` after `--`, and the shared test EAL arguments carry `-m 128` for the same
  reason. The pools are that large because every level 1 configuration registers a DPDK lcore that is never
  unregistered, and its mempool cache keeps up to 768 mbufs out of reach.
- GCC 16 rejects `std::ranges::find` over a `std::vector<std::string>` with a string literal as the value, which
  clang accepts: the `std::ranges::equal_to` constraint between `std::string` and the literal is not satisfied.
  Compare with `std::ranges::any_of` and a lambda, as `benchmark_options.cpp` does.
- A runtime task's `accept` opens its listener when the task first runs, and a shard's step receives before it
  runs tasks, so a SYN already on the ring when the runtime starts finds no listener and is reset. Open the
  listener before `start()` with `Streams::ensure_listener(port)`, as the benchmark's level 1 does, until a
  public `Streams::listen` exists.
- Under the `tsan` preset with `-DDO_BENCHMARKS=ON`, the benchmark's integration tests report hundreds of data
  races, all on mbuf memory handed across `rte_ring` inside DPDK's archives, which are built without TSan
  instrumentation, so TSan never sees the ordering. They are uninformative, not defects; the harness's own shared
  state raises none.
- None of DPDK's ring, null and tap drivers returns a transmitted mbuf lazily: null and tap free it inside the
  transmit call, the ring hands the very mbuf to the receiver. A packet that stays lent after its frame left exists
  on CI only on a fabric port with `LoanReturn::Held`; `ethdev::Port::receive` copies a shared mbuf, so a lent
  packet never reaches a ring reader as itself.
- DPDK 26.03's tap driver answers a requested transmit checksum fill by copying the headers into a new segment
  and trimming them off the original mbuf (`tap_write_mbufs`), which would move a lent packet's offsets.
  `transmit_offloads_of` in `port.cpp` clears the checksum bits for `net_tap`, so a tap port reports no transmit
  checksum offload and the stack checksums in software there, as on the ring. Do not hand them back.
- `rte_eth_dev_conf_get` is an experimental DPDK symbol. A test that calls it defines `ALLOW_EXPERIMENTAL_API`
  before its first include, as `test_ethdev_null.cpp` does; a public header never does.
- A fabric with delayed frames or loans on `OnStep` ports needs `Fabric::advance(now)` from the test's loop, once
  per tick; nothing is delivered or finished without it. A loan never advanced or reclaimed stays out until the
  port is destroyed, which releases it silently, so the pool's assertion does not catch a missing `advance`: it
  shows as `lent()` staying true and `loans_outstanding(queue)` above zero.
- `std::uniform_real_distribution` gives different numbers on libstdc++ and libc++ for the same engine; the
  fabric's impairment turns the engine's raw bits into a double itself, and a test of its determinism must not use
  the distribution either.
- A test constant named `gamma` is ambiguous once `<cmath>` reaches the test through `<aloe/core>`: glibc declares
  a `gamma` function. The fabric tests name theirs `gamma_mac`.
- GCC rejects `for (const auto [a, b] : {std::pair{...}, ...})` under `-Werror=range-loop-construct`, since each
  element is copied; write `const auto& [a, b]`, as `scenario_main.cpp` does.
- Once `frames::TcpSpec` owns a `std::vector` (its raw `options`), clang-tidy's `modernize-pass-by-value` wants a
  constructor that keeps one to take it by value and move it, as `TcpPeer(TcpSpec spec, ...)` does, not as
  `const TcpSpec`.
- The scenario scripts binary registers its tests at run time, so `ctest` lists one entry for it; use
  `--gtest_filter='Scenario.tcp/<area>/*'` on the binary to run one area. A `-` in a gtest filter starts the
  excluded patterns, so a script with a dash in its name is matched with `?`: `fin?retransmitted*`, not `fin-*`.

## Documentation

Public documentation follows one pattern; keep it current in the same change as the code.

- `docs/architecture/overview.md` describes the stack's design. Update it when a design decision changes.
- `docs/architecture/<module>.md` describes one module: what it is, key types, usage, design notes. A new
  module gets a page and a row in the README's module table.
- `docs/roadmap.md` lists the phases. Update it when a phase lands or its scope changes.
- `docs/guides/` holds task walkthroughs. Build and test changes go in `getting-started.md`.
- `docs/benchmarks/<name>.md` describes one benchmark and carries its recorded results; a result file is
  committed beside it under `docs/benchmarks/results/`.

## Commits

Conventional commit subjects with the module as scope: `feat(core): ...`,
`build(dpdk): ...`, `fix(build): ...`, `docs: ...`, `ci: ...`.

## Pull requests

- Titles start with bracketed tags, then a short summary:
  `[DEVICE][FABRIC] Steer by RSS on multi-queue ports`.
  - Work on specific modules gets one tag per module, upper case, as many as
    the change touches: `[CORE]`, `[DEVICE]`, `[ETHDEV]`.
  - Work about the code base in general gets a general tag: `[REFACTORING]`
    for cross-cutting code changes, `[INFRASTRUCTURE]` for build, CI, hooks and
    tooling, `[DOCS]` for documentation only.
- Descriptions are short: what changed and why in a few lines, then how it was
  verified. Leave out what the diff already shows.
