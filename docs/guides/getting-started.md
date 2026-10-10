# Getting Started

Aloe is developed and tested on Linux. Every preset but one builds with clang,
linked with mold, with ccache fronting the compiler. Both supported compilers,
clang and GCC, use libstdc++ as the standard library.

## Prerequisites

The toolchain observed during this guide's verification build (Fedora 44):

- clang / clang++ 22.1.8
- GCC 16.1.1, which also provides libstdc++ 16 for clang
- CMake 4.3.0 (`cmakeMinimumRequired` in `CMakePresets.json` is 3.29)
- Ninja (the generator every preset uses)
- ccache 4.12.3 (wired in as `CMAKE_CXX_COMPILER_LAUNCHER`/`CMAKE_C_COMPILER_LAUNCHER`)
- mold (`CMAKE_EXE_LINKER_FLAGS`/`CMAKE_SHARED_LINKER_FLAGS` pass `-fuse-ld=mold`)
- Python 3 (the DPDK port builds with meson, which vcpkg provisions itself)
- git, curl, pkg-config, autoconf/automake/libtool, bison, flex, perl with `IPC::Cmd` (vcpkg
  port builds need these)

`scripts/install-linux.sh` installs this set for Fedora/RHEL (dnf), Debian/Ubuntu (apt),
and Arch (pacman). Fedora 44 is the distribution the project is verified on. Aloe uses
C++26 library features, so other distributions need a libstdc++ at least as new as the
one in GCC 16.

## One-command setup

On a machine that has none of the above, the Linux script also works standalone. Piped
from curl it installs the dependencies, clones the repository into `./Aloe`, then
configures and builds the `debug` preset:

```bash
bash <(curl -fsSL https://raw.githubusercontent.com/MindSpectre/Aloe/main/scripts/install-linux.sh)
```

`--dir <path>` changes where it clones, `--preset <name>` builds something other than
`debug`, `--no-build` stops after configuring, and `--deps-only` installs the system
packages and nothing else. Run from inside an existing checkout the script installs the
dependencies and stops, leaving the configure to you.

## vcpkg setup

Dependencies (DPDK, stdexec, quill, GoogleTest, and Google Benchmark for the benchmark targets)
are declared in `vcpkg.json` and resolved through a vcpkg checkout that lives inside the
source tree. `vcpkg/` is gitignored - every clone provisions its own copy, there is no
submodule.

Provisioning is automatic. `CMakePresets.json` points `CMAKE_TOOLCHAIN_FILE` at
`cmake/vcpkg-bootstrap.cmake`, a shim that resolves a vcpkg checkout, clones and bootstraps
one if there is none, and then chains to vcpkg's real toolchain file. `cmake --preset debug`
on a fresh clone therefore just works; nothing has to be set up by hand. The shim resolves
in this order:

1. An existing checkout selected by `-DVCPKG_ROOT=<path>` on the configure line
2. `vcpkg/` in the repository root
3. the `VCPKG_ROOT` environment variable (the toolchain image sets this to `/opt/vcpkg`)
4. otherwise: `git clone --depth 1` upstream vcpkg into `vcpkg/` and bootstrap it

The explicit override is preserved through CMake compiler checks. When changing the
checkout in an existing build directory, reconfigure with `--fresh` to clear vcpkg's
cached root.

Which checkout a build resolved to, and how, is printed in the `VCPKG` banner at configure
time. Two cache variables tune the clone: `VCPKG_BOOTSTRAP_URL` (clone source) and
`VCPKG_BOOTSTRAP_REF` (a commit, tag, or branch to check out - setting it switches to a
full clone, since an arbitrary sha is not reachable from a shallow one).

No specific vcpkg commit is pinned by default and the manifest carries no
`vcpkg-configuration.json` baseline, so the DPDK and stdexec versions are the ones the
vcpkg checkout ships. The CI toolchain image (`infrastructure/toolchain/Dockerfile`)
clones vcpkg the same way.

The first configure builds every port from source and is slow (DPDK alone takes a few
minutes); subsequent configures reuse the installed tree under
`build/<preset>/vcpkg_installed/` and vcpkg's binary cache.

## How DPDK is built and linked

- **Statically, with every driver.** Drivers register themselves from static
  constructors, so `cmake/dpdk.cmake` links the DPDK archives whole. Link against
  `Aloe::Dpdk` and never against DPDK's pkg-config data directly, or the drivers are
  dropped silently. The price is size: a debug executable that links `Aloe::Dpdk` is
  about 200 MB.
- **For the generic x86-64 baseline.** `triplets/x64-linux-clang.cmake` passes
  `-Dplatform=generic` to DPDK. A DPDK tuned to the machine that built it refuses to
  start on a different CPU model, which would break a shared binary cache. Translation
  units that link `Aloe::Dpdk` are compiled with the instruction-set flags DPDK reports.
- **With whatever optional system libraries are installed.** DPDK enables some features
  when it finds a development package on the machine, for example libelf. A binary cache
  built on such a machine then needs that package wherever it is restored.

## Configure and build

| Preset              | Use it for                                                                  |
|---------------------|-----------------------------------------------------------------------------|
| `debug`             | Default day-to-day build: tests, benchmarks, examples                       |
| `release`           | Optimized build with the full feature set                                   |
| `ci`                | What PR CI builds: `release` minus benchmark/example targets and their ports |
| `gcc-debug`         | Debug build of the project with GCC; clang-tidy does not run                |
| `asan`              | Debug build instrumented with AddressSanitizer + UndefinedBehaviorSanitizer |
| `tsan`              | Debug build instrumented with ThreadSanitizer; no benchmarks                |
| `llvm-coverage`     | Debug build with clang source-based coverage (`llvm-profdata`/`llvm-cov`)   |
| `gcc-coverage`      | Debug build with gcov-style coverage (matches CLion's CTest coverage view)  |
| `release-lto`       | Release with link-time optimization                                         |
| `release-perf`      | Release codegen with frame pointers kept, for `perf record -g` profiling    |
| `release-instrprof` | Release build instrumented for LLVM profiling-guided hotspot analysis       |

Each preset has a matching build and test preset of the same name and configures into
`build/<preset>/`. The canonical sequence for the default `debug` preset:

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

`ctest --preset <name> -L unit`, `-L integration` or `-L functional` restricts the run to
one label. The integration tests start DPDK with no hugepages, no PCI scan and one virtual
device, so they need neither root nor a network card. `Aloe.Tests.Integration.Tcp.Ring` is one:
the TCP brick over DPDK's ring driver on real mbufs, with a scripted peer at another address,
software checksums and no root.

Tests under `tests/manual_tests/` carry the label `manual` and need privileges or hardware.
Every test preset excludes that label, so they never run unasked; run the binary by hand. The
four there today drive DPDK's tap driver against the kernel and need `CAP_NET_ADMIN`:

```bash
cmake --build --preset debug
sudo ./build/debug/tests/manual_tests/ethdev/Aloe.Tests.Manual.Ethdev.Tap
sudo ./build/debug/tests/manual_tests/runtime/Aloe.Tests.Manual.Runtime.Tap
sudo ./build/debug/tests/manual_tests/net/Aloe.Tests.Manual.Net.Tap          # the kernel pings the IP base
sudo ./build/debug/tests/manual_tests/tcp/Aloe.Tests.Manual.Tcp.Tap          # the kernel talks TCP to the brick and the runtime
```

The net test gives the tap's kernel end an address itself and pings through the kernel's own stack, so it
needs `CAP_NET_RAW` as well. `examples/ping_responder` is the same loop as a program you can `ping` from a
shell.

The TCP suite needs DPDK's tap driver, which every Aloe build links, and root. It creates the interface
`aloe-tcp`, puts the kernel at 10.78.0.1/24 and Aloe at 10.78.0.2/24, and gives the kernel end its address
itself, as `ip addr add 10.78.0.1/24 dev aloe-tcp` and `ip link set aloe-tcp up` would. Port 7 carries an
echo in four scenarios: a kernel client against the brick and against the runtime, and the brick and the
runtime as clients of a kernel listener. Without root every case is skipped. To watch the traffic while it
runs, `sudo tcpdump -ni aloe-tcp tcp` in another shell; the interface goes away when the suite exits.

## Formatting and the pre-commit hook

`.clang-format` decides how C++ is laid out. `./scripts/check-format.sh` checks every
`.cpp` and `.hpp` under `common/`, `tests/`, `examples/` and `benchmarks/`
against it and fails if any of them differs, and `clang-format -i <files>` fixes them.
`clang-format` comes with the toolchain that `scripts/install-linux.sh` installs.

The repository ships a pre-commit hook, `.githooks/pre-commit`, that runs
`./scripts/check-format.sh --staged`. It checks the staged version of each staged C++
file, which is what the commit would record, and rejects the commit with the
`clang-format -i` command to run. A commit that stages no C++ is not checked, and the hook
never rewrites a file.

There is nothing to set up: configuring, which every build does first, sets
`core.hooksPath` to `.githooks` in the checkout, and the configure output says what it
did. It leaves alone a `core.hooksPath` that is already set, in the checkout or in your
global git configuration. In that case, enable the hook by hand:

```bash
git config core.hooksPath .githooks
```

`git commit --no-verify` skips the hook. CI does not: every pull request runs
`./scripts/check-format.sh` on the whole tree, in the toolchain image, before it builds.

## Options

Feature toggles live in `cmake/features.cmake` and are set with `-D<NAME>=ON|OFF` on the
`cmake --preset` invocation, or overridden per preset in `CMakePresets.json`:

- `USE_TESTS` - build the GoogleTest suites under `tests/`
- `DO_BENCHMARKS` - build the benchmark binaries under `benchmarks/`
- `BUILD_EXAMPLES` - build the example apps under `examples/`
- `KEEP_FRAME_POINTERS` - keep frame pointers in Release builds so `perf` call graphs
  resolve; off by default because Release otherwise omits them
- `BUILD_DOCS` - generate the Doxygen API site (target `docs`); OFF by default so a normal
  build does not require Doxygen

## Building the docs

The Doxygen site is behind `BUILD_DOCS`, off by default so ordinary builds do not need
Doxygen installed:

```bash
cmake --preset debug -DBUILD_DOCS=ON
cmake --build build/debug --target docs
```

The generated site lands at `build/docs/html/index.html`; open it in a browser. The
`Doxyfile` (`docs/doxygen/Doxyfile`) indexes `README.md`, `docs/roadmap.md`, `docs/architecture`,
`docs/guides`, and `common`, and uses the vendored doxygen-awesome-css theme, so the
architecture pages and guides appear alongside the generated API reference.
