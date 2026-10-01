# Roadmap

The direction of work, in order. Each phase ends with a condition that says when it is done. The design
these phases build towards is in [docs/architecture/overview.md](architecture/overview.md).

## 0. Runtime core

The repository skeleton is done: toolchain, dependencies through vcpkg, the `Aloe::Dpdk` target, the
[`core`](architecture/core.md) module, smoke tests and CI. The [device layer](architecture/device.md) is
done: the packet and device concepts, the in-memory fabric and the DPDK backend, with one conformance
suite that runs against both without root or hugepages.

Still to come: the shard runtime, meaning core launch, run loop, scheduler, run queue, cross-shard inbox,
timer wheel and timer sender, counting scope, task type, stop and deadline plumbing, counters and logging.
The runtime verifies steering and thread concurrency as a whole, on the fabric under ThreadSanitizer.

Done when tasks and timers run on shards, work can be submitted across shards, and an Ethernet echo runs
on several shards on both backends with every frame landing on the shard the hash selects.

## 1. Minimal TCP

Ethernet, ARP with cache and gateway resolution, IPv4 without fragmentation, ICMP echo. TCP with the
handshake in both directions, in-order data, FIN and RST, a fixed window and the MSS option, and nothing
else. A connection table keyed by 4-tuple, shard placement for inbound and outbound connections, and the
stream concept expressed as senders.

Done when an echo client and server written as coroutine tasks interoperate with the Linux kernel stack over
a tap device, and the simulation tests pass on a loss-free fabric.

## 2. Full TCP

Retransmission with RTO estimation, out-of-order reassembly, SACK, window scaling, timestamps, delayed ACK,
zero-window probes, keepalive and TIME_WAIT. Scripted loss, reordering and delay in the fabric, which is
where they are first needed. Congestion control as a pluggable policy, NewReno first and
CUBIC second, with optional pacing. The public API freezes as v1 at the end of this phase.

Done when the simulation suite passes under scripted impairment, packetdrill-style scenario tests pass,
throughput and latency hold up against Linux under netem, and a soak run shows no leaks.

## 3. TLS

TLS 1.3 client and server over OpenSSL, bound to the Aloe stream so records move without copies. TLS is a
stream layer with the same shape as the layers below it.

Done when TLS echo interoperates with the `openssl` command-line tools and a public HTTPS endpoint, with
handshake and per-record latency measured.

## 4. HTTP/1.1 and WebSocket

A minimal HTTP/1.1 client and server, enough for the upgrade handshake and plain requests. WebSocket per
RFC 6455 on both sides: masking, fragmentation, ping and pong, close handshake, and optional
permessage-deflate.

Done when the Autobahn test suite passes for client and server, and a sample holds a real public WebSocket
feed for hours.

## 5. Adoption

An Asio-shaped adapter so Boost.Beast can run over Aloe for cross-checking, benchmarks against the kernel
stack, documentation and examples.

## Testing

Every phase adds tests at three levels.

1. **Simulated fabric.** Deterministic, no root, no hugepages; runs in CI.
2. **DPDK over a tap device** against the Linux kernel stack, with real packet buffers. Needs hugepages and
   root, so it runs locally.
3. **Real network cards,** for performance and offload validation.

## Open questions

- Whether a callback API is offered next to the senders, or stays internal.
- How errors are reported: the error channel with a small error code, or `std::expected` in the value
  channel.
- Congestion control algorithms beyond NewReno and CUBIC, and TLS libraries beyond OpenSSL.
- When IPv6 arrives.
- Whether libc++ becomes supported, and whether C++ modules are revisited at the v1 freeze.
- Whether DPDK is pinned to a long-term-support release instead of the version vcpkg ships.
- The two-core shard, one core per direction, measured against the single-core shard after phase 2.
