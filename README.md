# Aloe

Aloe is a C++26 userspace TCP/IP stack on DPDK for Linux, built for programs
whose latency is measured from a tick to a send. It is a library of bricks you
write your own loop over, each a plain call that returns before anything waits,
with the whole path from Ethernet frame to WebSocket message running in
userspace. On top of the bricks sits a runtime that writes the loop for you and
offers senders and receivers, and later a stream API in the spirit of
Boost.Beast, for the code that waits.

The repository contains the device layer with an in-memory backend for tests and
a DPDK backend, the queue, wheel and work node a loop is built from, the runtime
that drives them on one thread per queue, IPv4 with ARP and ICMP echo, and a TCP
whose connections are zero-copy streams: reliable on the send side, recovering
from loss by RACK-TLP over the peer's SACK blocks, holding what arrives above a
gap as the packets it came in, with window scaling and NewReno, and with senders
over it in the runtime. The stack is built in phases:
shard runtime, minimal TCP, full TCP, TLS, and finally HTTP/1.1 with WebSocket.
See the [architecture overview](docs/architecture/overview.md) for the design
and the [roadmap](docs/roadmap.md) for the phases.

## Status

Aloe is pre-v1. The public API may change at any step until it freezes as v1 at
the end of phase 2, and the stack makes no deployment or latency claim yet.

- **Done:** phase 0, the shard runtime, and phase 1, minimal TCP.
- **Phase 2, full TCP, steps 1 to 5 landed:** the tick-to-trade benchmark
  (its first recorded run waits for a quiet machine with hugepages), packet
  loan with scripted impairment and a packetdrill-style scenario runner,
  reliable send (RTO, timestamps and PAWS, zero-window probes, fast retransmit
  and NewReno, bounded recovery), the receive side (segments held above a
  gap without a copy, SACK and duplicate SACK, window scaling, an optional
  delayed ACK) and loss recovery with SACK (a scoreboard, RACK-TLP, PRR and
  the undo), which misses its latency bar: it costs the benchmark's 64-byte
  tick 40 ns at p50 and 60 ns at p99, as its page records.
- **Next:** the lifecycle and client hardening (TIME_WAIT, keepalive, path MTU,
  RFC 6528 and RFC 5961), where the API freezes. TLS and HTTP/1.1 with
  WebSocket follow.

Until step 6 lands the stack is not hardened against hostile peers.

## Modules

| Namespace         | Directory           | Description                                                                                                                                                                                               | Docs                                                             |
|-------------------|---------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------|
| `aloe::core`      | `common/core/`      | The small things every module links, with no dependencies: the version, the clock (a calibrated TSC clock by default), discarding values, a fallback, and the const and static guards.                    | [docs/architecture/core.md](docs/architecture/core.md)           |
| `aloe::execution` | `common/execution/` | The one header that names the execution facilities (stdexec), linked on demand.                                                                                                                           | [docs/architecture/execution.md](docs/architecture/execution.md) |
| `aloe::log`       | `common/log/`       | Named loggers over quill, linked on demand.                                                                                                                                                               | [docs/architecture/log.md](docs/architecture/log.md)             |
| `aloe::wire`      | `common/wire/`      | Addresses, headers and checksums grouped by protocol (Ethernet, IPv4, ARP, ICMP, TCP): values and formats, no state.                                                                                      | [docs/architecture/wire.md](docs/architecture/wire.md)           |
| `aloe::device`    | `common/device/`    | The Packet and Device concepts, the packet metadata and receive-side scaling.                                                                                                                             | [docs/architecture/device.md](docs/architecture/device.md)       |
| `aloe::ethdev`    | `devices/ethdev/`   | The DPDK device backend.                                                                                                                                                                                  | [docs/architecture/device.md](docs/architecture/device.md)       |
| `aloe::loop`      | `common/loop/`      | The bricks a loop is built from: a device queue with its transmit ring, the timer wheel, the work node with the run queue and the inbox, and the counters.                                                | [docs/architecture/loop.md](docs/architecture/loop.md)           |
| `aloe::runtime`   | `common/runtime/`   | The loop written for you: shards on pinned threads, the scheduler, timer senders, scope and task, the connection senders and the ready-made TCP stack, and the runtime that launches one shard per queue. | [docs/architecture/runtime.md](docs/architecture/runtime.md)     |
| `aloe::net`       | `common/net/`       | The first protocol brick: IPv4 over a device queue with ARP and ICMP echo, datagrams sorted per transport, checksums by offload or software.                                                              | [docs/architecture/net.md](docs/architecture/net.md)             |
| `aloe::stream`    | `common/stream/`    | The zero-copy stream contract every protocol layer offers: the concepts, the event and error vocabulary, and the copying `send`.                                                                          | [docs/architecture/stream.md](docs/architecture/stream.md)       |
| `aloe::tcp`       | `common/tcp/`       | TCP as a brick over IP: the handshake both ways, data held as received packets in order and above a gap, SACK, window scaling, reliable send, RACK-TLP, NewReno behind a policy, FIN, RST and placement.  | [docs/architecture/tcp.md](docs/architecture/tcp.md)             |

Fixtures for testing code written over the bricks live apart from the stack, in `fixtures/`
([docs/architecture/fixtures.md](docs/architecture/fixtures.md)):

| Namespace        | Directory            | Description                                                                                                                     |
|------------------|----------------------|---------------------------------------------------------------------------------------------------------------------------------|
| `aloe::fabric`   | `fixtures/fabric/`   | The in-memory device backend: deterministic, no root, no hugepages, no network card, with scripted impairment and a clock step. |
| `aloe::frames`   | `fixtures/frames/`   | Frame builders and a parser over the wire formats, and packet byte helpers.                                                     |
| `aloe::scenario` | `fixtures/scenario/` | Packetdrill-style scenario scripts run against the TCP brick on the fabric: the parser, the runner and the harness.             |

Each module ships an umbrella header. Consumers link the module's target and
include it by name:

```cpp
#include <aloe/core>
```

## Quick example

A TCP echo server, first as a loop you write over the bricks, then as a task
on the runtime. Both are trimmed: the full programs, with the client side and
how to run them against the kernel over a tap device, are
`examples/tcp_echo/tcp_echo.cpp` and `examples/tcp_echo_tasks/tcp_echo_tasks.cpp`.

The bricks: no coroutine and no sender on the data path. Each call returns
before anything waits, and the program decides what happens between them:

```cpp
using Port = aloe::ethdev::Port;
using Ipv4 = aloe::net::Ipv4<Port>;
using Tcp  = aloe::tcp::Stack<Ipv4>;

aloe::core::calibrate_clock();
aloe::ethdev::Eal eal{eal_arguments};
Port port{{.name = "net_tap0", .queues = 1}};
aloe::loop::ShardQueueCounters counters;
aloe::loop::ShardQueue<Port> queue{port, 0, 512, counters};
Ipv4 ip{queue, ip_config};
aloe::loop::TimerWheel wheel{std::chrono::milliseconds{1}, aloe::core::Clock::now()};
Tcp tcp{ip, wheel, {}};
std::vector<Port::Packet> burst(64);
std::ignore = tcp.listen(7);

for (;;) {
    const auto now             = aloe::core::Clock::now();
    const std::size_t received = queue.receive(burst);
    ip.process(std::span<Port::Packet>{burst}.first(received), now);
    tcp.process(ip.received(aloe::wire::Ipv4Protocol::Tcp), now);
    std::ignore = wheel.advance(now);

    while (const auto event = tcp.poll_event()) {
        Tcp::ConnectionType& c = *event->connection;
        if (event->events.readable() || event->events.writable()) {
            echo(c);  // prepare, copy from c.unread(), commit, consume
        }
        if (c.peer_closed() && c.unread().empty()) {
            c.close();
        }
        if (event->events.closed() || event->events.reset() || event->events.timed_out()) {
            c.release();
        }
    }
    tcp.flush(now);
    std::ignore = queue.flush();
}
```

The runtime: the same loop, written for you on one shard per queue, with
senders for the code that waits:

```cpp
aloe::runtime::task<void> echo(Stack::StreamType stream) {
    while (co_await stream.readable(1)) {
        while (!stream.unread().empty()) {
            const auto chunk = stream.unread().front();
            if (!co_await stream.send(chunk)) {
                co_return;
            }
            stream.consume(chunk.size());
        }
    }
    std::ignore = co_await stream.close();
}

aloe::runtime::task<void> serve(Stack::StreamsType* streams, aloe::runtime::Scheduler scheduler, std::uint16_t port) {
    while (auto stream = co_await streams->accept(port)) {
        scheduler.spawn(echo(std::move(*stream)));
    }
}
```

## Requirements

- Linux on x86-64. Fedora 44 is the distribution the project is verified on;
  `scripts/install-linux.sh` also covers Debian/Ubuntu and Arch.
- A C++26 compiler: clang 22 or GCC 16, both with libstdc++ 16. The build uses
  CMake presets, Ninja, mold and ccache; the install script provides them.
- Everything else, DPDK included, comes through vcpkg on the first configure.
- `core::Clock` reads the TSC by default and refuses to start without an
  invariant TSC and the kernel's `tsc` clocksource, which most virtual machines
  lack. On a VM, configure with `-DUSE_TSC_CLOCK=OFF`.
- The tests need neither root, nor hugepages, nor a network card. The examples
  and the manual tap tests need root; the benchmark needs hugepages.

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
tests need neither root nor hugepages nor a network card. The benchmark under
`benchmarks/` does need hugepages; see
[docs/benchmarks/tick-to-trade.md](docs/benchmarks/tick-to-trade.md).

See [docs/guides/getting-started.md](docs/guides/getting-started.md) for
prerequisites, the vcpkg setup, and the full list of presets and build options.

## Documentation

- `docs/architecture/` - the stack's design in overview.md, then one page per module.
- `docs/guides/` - task-oriented walkthroughs, starting with getting-started.md.
- `docs/benchmarks/` - one page per benchmark with its recorded results, the result files under `results/`.
- `docs/roadmap.md` - the phases still to come, each with its completion condition.
- `AGENTS.md` - how the repository is built, tested and laid out, for coding agents and people alike.
- `codestyle.md` - the code style beyond what `.clang-format` enforces.

The Doxygen API reference indexes this README alongside `docs/architecture/`,
`docs/guides/`, `docs/benchmarks/` and the roadmap:

```bash
cmake --preset debug -DBUILD_DOCS=ON
cmake --build build/debug --target docs
```

The generated site lands at `build/docs/html/index.html`.

## License

BSD-3-Clause - see [LICENSE](LICENSE).
