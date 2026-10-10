# Aloe

Aloe is a C++26 userspace TCP/IP stack on DPDK for Linux, built for programs
whose latency is measured from a tick to a send. It is a library of bricks you
write your own loop over, each a plain call that returns before anything waits,
with the whole path from Ethernet frame to WebSocket message running in
userspace. On top of the bricks sits a runtime that writes the loop for you and
offers senders and receivers, and later a stream API in the spirit of
Boost.Beast, for the code that waits.

The repository contains the build skeleton, the device layer, the loop bricks,
the shard runtime, the IP base and minimal TCP: the toolchain, the dependency
setup, the packet and device abstraction with an in-memory backend for tests and
a DPDK backend, the queue, wheel and work node a loop is built from, the runtime
that drives them on one thread per queue, IPv4 with ARP and ICMP echo, and a TCP
whose connections are zero-copy streams, with senders over them in the runtime.
The stack is built in phases: minimal TCP, then full TCP with retransmission
and congestion control, TLS, and finally HTTP/1.1 with WebSocket. See the [architecture overview](docs/architecture/overview.md)
for the design and the [roadmap](docs/roadmap.md) for the phases.

## Modules

| Namespace       | Directory            | Description                                                                                                     | Docs                                                         |
|-----------------|----------------------|-----------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------|
| `aloe::core`    | `common/core/`       | The small things every module links, with no dependencies: the version, the clock aliases, discarding values, a fallback, and the const and static guards. | [docs/architecture/core.md](docs/architecture/core.md)       |
| `aloe::execution` | `common/execution/` | The one header that names the execution facilities (stdexec), linked on demand.                              | [docs/architecture/execution.md](docs/architecture/execution.md) |
| `aloe::log`     | `common/log/`        | Named loggers over quill, linked on demand.                                                                     | [docs/architecture/log.md](docs/architecture/log.md)         |
| `aloe::wire`    | `common/wire/`       | Addresses, headers and checksums grouped by protocol (Ethernet, IPv4, ARP, ICMP, TCP): values and formats, no state. | [docs/architecture/wire.md](docs/architecture/wire.md)       |
| `aloe::device`  | `common/device/`     | The Packet and Device concepts, the packet metadata and receive-side scaling.                                   | [docs/architecture/device.md](docs/architecture/device.md)   |
| `aloe::ethdev`  | `common/ethdev/`     | The DPDK device backend.                                                                                        | [docs/architecture/device.md](docs/architecture/device.md)   |
| `aloe::loop`    | `common/loop/`       | The bricks a loop is built from: a device queue with its transmit ring, the timer wheel, the work node with the run queue and the inbox, and the counters. | [docs/architecture/loop.md](docs/architecture/loop.md)       |
| `aloe::runtime` | `common/runtime/`    | The loop written for you: shards on pinned threads, the scheduler, timer senders, scope and task, the connection senders and the ready-made TCP stack, and the runtime that launches one shard per queue. | [docs/architecture/runtime.md](docs/architecture/runtime.md) |
| `aloe::net`     | `common/net/`        | The first protocol brick: IPv4 over a device queue with ARP and ICMP echo, datagrams sorted per transport, checksums by offload or software. | [docs/architecture/net.md](docs/architecture/net.md)         |
| `aloe::stream`  | `common/stream/`     | The zero-copy stream contract every protocol layer offers: the concepts, the event and error vocabulary, and the copying `send`. | [docs/architecture/stream.md](docs/architecture/stream.md)   |
| `aloe::tcp`     | `common/tcp/`        | Minimal TCP as a brick over the IP brick: the handshake both ways, in-order data held as the received packets, FIN and RST, the connection table and placement. | [docs/architecture/tcp.md](docs/architecture/tcp.md)         |

Fixtures for testing code written over the bricks live apart from the stack, in `fixtures/`
([docs/architecture/fixtures.md](docs/architecture/fixtures.md)):

| Namespace       | Directory            | Description                                                                                                     |
|-----------------|----------------------|-----------------------------------------------------------------------------------------------------------------|
| `aloe::fabric`  | `fixtures/fabric/`   | The in-memory device backend: deterministic, no root, no hugepages, no network card.                            |
| `aloe::frames`  | `fixtures/frames/`   | Frame builders and a parser over the wire formats, and packet byte helpers.                                     |

Each module ships an umbrella header. Consumers link the module's target and
include it by name:

```cpp
#include <aloe/core>
```

## Quick example

Trimmed from `examples/hello/hello.cpp`:

```cpp
#include <aloe/core>
#include <aloe/execution>
#include <print>
#include <tuple>

#include <rte_version.h>

int main() {
    const auto result = aloe::execution::ex::sync_wait(aloe::execution::ex::just(41) |
                                                       aloe::execution::ex::then([](int value) { return value + 1; }));

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
