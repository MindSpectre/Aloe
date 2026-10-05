# AGENTS.md

Guidance for coding agents, and for people, working in this repository.

Aloe is a C++26 userspace TCP/IP stack on DPDK for Linux. It is two products:
bricks, plain calls a program writes its own loop over, and a runtime built only
from the bricks that writes the loop for you and offers senders and receivers
(stdexec) for the code that waits. The repository holds the build skeleton, the
device layer (the packet and device concepts, an in-memory backend `fabric` for
tests and the DPDK backend `ethdev`), the loop bricks (`loop`), the shard
runtime (`runtime`) and the IP base (`net`: Ethernet, ARP, IPv4 and ICMP echo, the first protocol
brick). The stack is built in phases: shard runtime, minimal TCP, full TCP, TLS, HTTP/1.1 with WebSocket.

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

| Path                                                                       | Contents                                                                                                                                                                                                   |
|----------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `common/<module>/`                                                         | Stack and foundation modules. `core` holds the version and the execution alias; `utils` holds small header-only helpers with no dependencies; `device` (concepts, no DPDK), `fabric` (in-memory backend), `ethdev` (DPDK backend, the only module that links DPDK), `loop` (the bricks: device queue, timer wheel, work node, run queue, inbox, counters; no stdexec, no DPDK), `runtime` (shards, scheduler, scope, task, runtime; built on `loop`, no DPDK), `net` (the IP base: wire formats, the ARP cache, the `Ipv4<Device>` brick; built on `loop` and `device`, no `core`, no DPDK). |
| `component/<module>/`                                                      | Protocol modules (HTTP/1.1, HTTP/2, HTTP/3, WebSocket, ...), each an independent unit built on `common`. None exist yet; the directory appears with the first. |
| `<module>/export/aloe/<module>`                                            | Umbrella header, no extension. Consumers write `#include <aloe/<module>>`.                                                                                                                                 |
| `tests/unit_tests/`, `tests/integration_tests/`, `tests/functional_tests/` | One CTest label each.                                                                                                                                                                                      |
| `tests/manual_tests/`                                                      | Label `manual`: tests that need privileges or hardware. Every test preset excludes the label; run the binary by hand.                                                                                      |
| `tests/shared/`                                                            | Test helpers: the unprivileged EAL arguments, the EAL as a gtest environment, frame builders, and the device conformance suite every backend runs.                                                         |
| `cmake/`                                                                   | `vcpkg-bootstrap.cmake` (the toolchain file), `dpdk.cmake`, test and library helpers.                                                                                                                      |
| `triplets/`                                                                | The vcpkg overlay triplet every port is built with.                                                                                                                                                        |
| `examples/`, `benchmarks/`                                                 | Living examples; benchmark targets as the stack grows.                                                                                                                                                     |

Targets are named `Aloe.<Group>.<Module>` with an `Aloe::<Group>::<Module>`
alias. Register tests with `add_unit_test`, `add_integration_test`,
`add_functional_test` or `add_manual_test` from `cmake/tests.cmake`.

Every module's public headers sit on one flat include path, so header
basenames are unique across modules: `packet.hpp` is the concept,
`fabric_packet.hpp` and `ethdev_packet.hpp` are the backends' types.

## Rules

- **Execution facilities.** Write `aloe::core::ex::` for senders, receivers and
  schedulers, `aloe::core::task<T>` for coroutine tasks, and
  `aloe::core::TaskEnvironment` for a task bound to a concrete scheduler. Only
  `common/core/execution/execution.hpp` may name `stdexec::` or `exec::`.
- **Logging.** Only `common/core/log/log.hpp` may name `quill::`. Everything else
  takes a `core::Logger` from `core::logger(name)` and writes
  `log.info<"text {}">(value)`. Nothing logs on a hot path.
- **DPDK.** Link the target `Aloe::Dpdk` and nothing else. Linking DPDK's
  pkg-config data directly builds a program that starts with no drivers.
- **Dependencies.** Libraries come from `vcpkg.json` only: no system
  libraries, no fetched sources, no submodules. System packages are for build
  tools, installed by `scripts/install-linux.sh`.
- **No C++ modules.** `CMAKE_CXX_SCAN_FOR_MODULES` stays `OFF`; with it on,
  GCC 16 crashes inside stdexec.
- **Public headers define no macros.** Implementation details go in a nested
  `detail` namespace.
- **Style.** `codestyle.md` covers what `.clang-format` does not: naming,
  namespaces, parameters, attributes, error handling, include order.
- **Namespaces.** Every module gets a namespace named after it: `aloe::core`,
  `aloe::utils`, `aloe::device`, `aloe::fabric`, `aloe::ethdev`, `aloe::loop`,
  `aloe::runtime`, `aloe::net`. Nothing is
  declared directly in `aloe`. Another module's names are qualified with its
  namespace (`device::MacAddress` inside `aloe::fabric`), never pulled in with
  `using namespace`. Test helpers live in `aloe::testing`.
- **stdexec stays out of hot-path headers.** Only the runtime and the public
  surface include `<aloe/core>`; a device or protocol header takes helpers from
  `<aloe/utils>` and the loop's bricks from `<aloe/loop>`, which does not link
  `core`, so a protocol module that links `Aloe::Common::Loop` and not
  `Aloe::Common::Core` cannot include stdexec by accident.
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
  only writes through a pointer member. Call `utils::force_non_const(this)`
  first when the type owns what the pointer reaches (`ethdev::Packet`,
  `ShardQueue`); make the member `const` when the type is a non-owning handle
  (`core::Logger`).
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
- `core::Logger::flush` spins forever when no `Logging` is alive; quill waits for
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

## Documentation

Public documentation follows one pattern; keep it current in the same change as the code.

- `docs/architecture/overview.md` describes the stack's design. Update it when a design decision changes.
- `docs/architecture/<module>.md` describes one module: what it is, key types, usage, design notes. A new
  module gets a page and a row in the README's module table.
- `docs/roadmap.md` lists the phases. Update it when a phase lands or its scope changes.
- `docs/guides/` holds task walkthroughs. Build and test changes go in `getting-started.md`.

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
