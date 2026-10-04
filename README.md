# Aloe

Aloe is a C++26 userspace TCP/IP stack on DPDK for Linux, built for programs
whose latency is measured from a tick to a send. It is a library of bricks you
write your own loop over, each a plain call that returns before anything waits,
with the whole path from Ethernet frame to WebSocket message running in
userspace. On top of the bricks sits a runtime that writes the loop for you and
offers senders and receivers, and later a stream API in the spirit of
Boost.Beast, for the code that waits.

The repository contains the build skeleton, the device layer, the loop bricks
and the shard runtime: the toolchain, the dependency setup, the packet and
device abstraction with an in-memory backend for tests and a DPDK backend, the
queue, wheel and work node a loop is built from, and the runtime that drives
them on one thread per queue. The stack itself is built in phases: a minimal
TCP, full TCP with retransmission and congestion control, TLS, and finally
HTTP/1.1 with WebSocket. See the [architecture overview](docs/architecture/overview.md)
for the design and the [roadmap](docs/roadmap.md) for the phases.

## Modules

| Namespace       | Directory            | Description                                                                                                     | Docs                                                         |
|-----------------|----------------------|-----------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------|
| `aloe::core`    | `common/core/`       | Library version, and the one header that names the execution facilities.                                        | [docs/architecture/core.md](docs/architecture/core.md)       |
| `aloe::utils`   | `common/utils/`      | Small header-only helpers with no dependencies: discarding values, a fallback, and the const and static guards. | [docs/architecture/utils.md](docs/architecture/utils.md)     |
| `aloe::device`  | `common/device/`     | The Packet and Device concepts, addresses, checksums and receive-side scaling.                                  | [docs/architecture/device.md](docs/architecture/device.md)   |
| `aloe::fabric`  | `common/fabric/`     | The in-memory device backend: a fixture for tests, simulation and demos.                                        | [docs/architecture/device.md](docs/architecture/device.md)   |
| `aloe::ethdev`  | `common/ethdev/`     | The DPDK device backend.                                                                                        | [docs/architecture/device.md](docs/architecture/device.md)   |
| `aloe::loop`    | `common/loop/`       | The bricks a loop is built from: a device queue with its transmit ring, the timer wheel, the work node with the run queue and the inbox, and the counters. | [docs/architecture/loop.md](docs/architecture/loop.md)       |
| `aloe::runtime` | `common/runtime/`    | The loop written for you: shards on pinned threads, the scheduler, timer senders, scope and task, and the runtime that launches one shard per queue. | [docs/architecture/runtime.md](docs/architecture/runtime.md) |

Each module ships an umbrella header. Consumers link the module's target and
include it by name:

```cpp
#include <aloe/core>
```

## Quick example

Trimmed from `examples/hello/hello.cpp`:

```cpp
#include <aloe/core>
#include <print>
#include <tuple>

#include <rte_version.h>

int main() {
    const auto result =
        aloe::core::ex::sync_wait(aloe::core::ex::just(41) | aloe::core::ex::then([](int value) { return value + 1; }));

    std::println("Aloe {}", aloe::core::version_string);
    std::println("{}", rte_version());
    std::println("sender result {}", std::get<0>(*result));
}
```

## Building

On a machine with nothing installed yet, this installs the system dependencies,
clones the repository, and produces a first build:

```bash
bash <(curl -fsSL https://raw.githubusercontent.com/MindSpectre/Aloe/main/scripts/install-linux.sh)
```

In an existing checkout, configure and build directly. vcpkg is provisioned on
the first configure, so there is nothing to set up beforehand:

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The first configure builds DPDK from source and takes several minutes. The
tests need neither root nor hugepages nor a network card.

See [docs/guides/getting-started.md](docs/guides/getting-started.md) for
prerequisites, the vcpkg setup, and the full list of presets and build options.

## Documentation

- `docs/architecture/` - the stack's design in overview.md, then one page per module.
- `docs/guides/` - task-oriented walkthroughs, starting with getting-started.md.
- `docs/roadmap.md` - the phases still to come, each with its completion condition.
- `AGENTS.md` - how the repository is built, tested and laid out, for coding agents and people alike.
- `codestyle.md` - the code style beyond what `.clang-format` enforces.

The Doxygen API reference indexes this README alongside `docs/architecture/`,
`docs/guides/` and the roadmap:

```bash
cmake --preset debug -DBUILD_DOCS=ON
cmake --build build/debug --target docs
```

The generated site lands at `build/docs/html/index.html`.

## License

BSD-3-Clause - see [LICENSE](LICENSE).
