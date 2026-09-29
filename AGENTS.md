# AGENTS.md

Guidance for coding agents, and for people, working in this repository.

Aloe is a C++26 userspace TCP/IP stack on DPDK for Linux, with senders and
receivers (stdexec) as its asynchronous model. The repository currently holds
the build skeleton: toolchain, dependencies, and smoke tests. The stack is
built in phases: shard runtime, minimal TCP, full TCP, TLS, HTTP/1.1 with
WebSocket.

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
- A change is done when `debug`, `gcc-debug` and `asan` all build and pass,
  and the format check passes. clang-tidy runs inside the clang builds and its
  warnings are errors, so a clean clang build is the lint check.
- A pre-commit hook (`.githooks/pre-commit`) runs `./scripts/check-format.sh --staged`
  on the staged `.cpp`/`.hpp` files. The first configure switches it on by setting
  `core.hooksPath`, unless one is already set. CI runs `./scripts/check-format.sh` on
  every pull request, so `git commit --no-verify` does not get past it.
- Tests need no root, no hugepages and no network card. Keep it that way.
- `build/` and `vcpkg/` are generated and git-ignored.

## Layout

| Path | Contents |
|---|---|
| `common/<module>/` | Foundation modules. `core` holds the version and the execution alias. |
| `component/<module>/` | Stack modules, added as the phases land. |
| `<module>/export/aloe/<module>` | Umbrella header, no extension. Consumers write `#include <aloe/<module>>`. |
| `tests/unit_tests/`, `tests/integration_tests/`, `tests/functional_tests/` | One CTest label each. |
| `tests/shared/` | Test helpers, such as the unprivileged EAL arguments. |
| `cmake/` | `vcpkg-bootstrap.cmake` (the toolchain file), `dpdk.cmake`, test and library helpers. |
| `triplets/` | The vcpkg overlay triplet every port is built with. |
| `examples/`, `benchmarks/` | Living examples; benchmark targets as the stack grows. |

Targets are named `Aloe.<Group>.<Module>` with an `Aloe::<Group>::<Module>`
alias. Register tests with `add_unit_test`, `add_integration_test` or
`add_functional_test` from `cmake/tests.cmake`.

## Rules

- **Execution facilities.** Write `aloe::ex::` for senders, receivers and
  schedulers, and `aloe::task<T>` for coroutine tasks. Only
  `common/core/execution/execution.hpp` may name `stdexec::` or `exec::`.
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
  attributes, error handling, include order.
- **Error handling.** `std::expected` on hot paths, exceptions on setup and
  cold paths.

## Traps already found

- clang-tidy reports `readability-static-accessed-through-instance` on every
  `co_await` in a stdexec task. The check is disabled in `.clang-tidy`; do not
  re-enable it.
- clang-tidy rejects `std::move` on a sender whose type is trivially copyable.
  Pass the sender expression directly.
- One process can start DPDK's EAL once. Keep all EAL checks of a test binary
  in one test, or fork, as `test_eal_concurrent.cpp` does.
- DPDK is built for the generic x86-64 baseline (`-Dplatform=generic` in the
  triplet), so a cached build starts on any CPU. `cmake/dpdk.cmake` refuses a
  DPDK built with `-march=native`.
- CMake wraps long `message()` text at a width that depends on the paths in
  it. Tests that match configure output collapse whitespace first.
- Debug executables that link DPDK are about 200 MB, because every driver is
  linked whole.

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
