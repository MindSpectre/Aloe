# Roadmap

The direction of work, in order. Each phase ends with a condition that says when it is done. The design
these phases build towards is in [docs/architecture/overview.md](architecture/overview.md).

## 0. Runtime core

Done. The repository skeleton: toolchain, dependencies through vcpkg, the `Aloe::Dpdk` target, the
[`core`](architecture/core.md) module, smoke tests and CI. The [device layer](architecture/device.md): the
packet and device concepts, the in-memory fabric and the DPDK backend, with one conformance suite that runs
against both without root or hugepages. The [loop bricks](architecture/loop.md): the device queue with its
transmit ring, the timer wheel, the work node with the run queue and the cross-thread inbox, and the
counters, with no dependency on the asynchronous model. The [shard runtime](architecture/runtime.md) over
them: core launch, run loop, scheduler, timer sender, counting scope, task type, stop plumbing and logging.
Tasks and timers run on shards, work is submitted across shards, and an Ethernet echo runs on several shards
on both backends with every frame landing on the shard the hash selects, verified on the fabric under
ThreadSanitizer.

## 1. Minimal TCP

Ethernet, ARP with cache and gateway resolution, IPv4 without fragmentation, ICMP echo, and UDP with
multicast group membership, which is what a feed handler consumes. The first four exist as the
[`net`](architecture/net.md) brick.

TCP has landed as the [`tcp`](architecture/tcp.md) brick: the handshake in both directions, in-order data
held as the received packets, FIN and RST, a fixed byte window and the MSS option, and control retransmission
of SYN, SYN-ACK and FIN on the shard's wheel. Receive, transmit and maintenance run on one owning thread per
connection, with their state ownership and the interactions between them recorded so a later design can
place them on two cores. A fixed connection table keyed by 4-tuple gives each connection a stable index;
inbound placement follows the card's RSS hash and outbound placement the choice of local port. ARP
resolutions are forwarded between shards.

The [stream](architecture/stream.md) contract every later layer reuses exists: `process` a burst,
`poll_event` to drain, `unread` and `consume` over the received packets, `prepare` and `commit` into the
packet that leaves, a convenience `send` that reports the bytes it accepted and never coalesces two sends into
one segment, and `flush`. The connection senders, accept, connect, readable, writable, acked, send, close and
closed, are a runtime layer written over the event list, after the bricks, with `runtime::TcpStack` as the
ready-made shard stack; the shard gained the per-tick hooks they need. `examples/tcp_echo` and
`examples/tcp_echo_tasks` are the echo in both products.

Phase 1 leaves out, on purpose: retransmission of data, storage of out-of-order segments, TIME_WAIT, and the
hardening a stack facing hostile peers needs (SYN cookies, RFC 6528 initial sequence numbers, RFC 5961
challenge ACKs, a keyed table hash, ARP-reply rate limiting). It makes no deployment or latency claim.

Remaining: UDP with multicast, and the first benchmark target, which measures tick-to-send through the
bricks against a raw poll of the device, on the fabric. Placement by flow rules per connection waits for the
device API it needs.

Done when an echo client and server written as a hand-written loop over the bricks, with no coroutine and
no sender on the data path, interoperate with the Linux kernel stack over a tap device; the same echo
written as runtime tasks does too; and the simulation tests pass on a loss-free fabric. The simulation tests
pass; the tap suite and the examples build and wait for a run on a machine with root.

## 2. Full TCP

Retransmission with RTO estimation, out-of-order reassembly, SACK, window scaling, timestamps, delayed ACK,
zero-window probes, keepalive and TIME_WAIT. Pre-built segment headers per connection, so a send patches
sequence, acknowledgement, timestamp and checksum and posts. Scripted loss, reordering and delay in the
fabric, which is where they are first needed. Congestion control as a pluggable policy, NewReno first and
CUBIC second, with optional pacing. Hardening against hostile peers: SYN cookies, RFC 6528 initial sequence
numbers, RFC 5961 challenge ACKs, a keyed table hash and ARP-reply rate limiting. The public API freezes as v1
at the end of this phase.

Done when the simulation suite passes under scripted impairment, packetdrill-style scenario tests pass,
throughput and latency hold up against Linux under netem, and a soak run shows no leaks.

## 3. TLS

TLS 1.3 client and server over OpenSSL, bound to the Aloe stream so records move without copies. TLS is a
stream layer with the same shape as the layers below it: a brick with `process`, `events`, `send` and
`flush`, and senders over it in the runtime. The record layer runs on the connection's core; NIC TLS
offload through DPDK's security API is the later option.

Done when TLS echo interoperates with the `openssl` command-line tools and a public HTTPS endpoint, with
handshake and per-record latency measured.

## 4. HTTP/1.1 and WebSocket

A minimal HTTP/1.1 client and server, enough for the upgrade handshake and plain requests. WebSocket per
RFC 6455 on both sides: masking, fragmentation, ping and pong, close handshake, and optional
permessage-deflate.

Done when the Autobahn test suite passes for client and server, and a sample holds a real public WebSocket
feed for hours.

## 5. Adoption

An Asio-shaped adapter over the runtime so Boost.Beast can run over Aloe for cross-checking, benchmarks
against the kernel stack, documentation and examples.

## Testing

Every phase adds tests at three levels.

1. **Simulated fabric.** Deterministic, no root, no hugepages; runs in CI.
2. **DPDK over a tap device** against the Linux kernel stack, with real packet buffers. Needs hugepages and
   root, so it runs locally.
3. **Real network cards,** for performance and offload validation.

## Open questions

- Congestion control algorithms beyond NewReno and CUBIC, and TLS libraries beyond OpenSSL.
- When IPv6 arrives.
- Whether libc++ becomes supported, and whether C++ modules are revisited at the v1 freeze.
- Whether DPDK is pinned to a long-term-support release instead of the version vcpkg ships.
- The two-core shard, one core per direction: designed in phase 1, measured against the single-core shard
  after phase 2.
- Which clock the loop reads: `steady_clock` today, a TSC clock behind a policy when the benchmark says so.
- Whether `loop::ShardCounters` splits along the product boundary, or stays one struct both fill.

Resolved in phase 1: errors are reported as `std::expected` in the value channel, and the stopped channel
carries cancellation only; abseil does not enter the project, since the connection table and the ARP cache
are fixed open-addressing tables of Aloe's own.
