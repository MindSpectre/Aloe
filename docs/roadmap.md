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

Ethernet, ARP with cache and gateway resolution, IPv4 without fragmentation and ICMP echo exist as the
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

Placement by flow rules per connection waits for the device API it needs. The first benchmark opens phase 2
as its baseline. UDP with multicast group membership, which a feed handler consumes, is off the TCP, TLS and
WebSocket path this roadmap follows, and waits until after it.

Done when an echo client and server written as a hand-written loop over the bricks, with no coroutine and
no sender on the data path, interoperate with the Linux kernel stack over a tap device; the same echo
written as runtime tasks does too; and the simulation tests pass on a loss-free fabric within the resource
envelope the TCP page states: at most `receive_segments` segments in flight to a connection, and a pool sized
for the connections that hold data at once. Outside it a drop between two Aloe peers is final until phase 2.
The simulation tests and the tap suite pass; the examples build and wait for a run on a machine with root.

## 2. Full TCP

The first deployment is a client: long-lived connections out to trading venues, many of them across the
internet behind TLS and WebSocket. That sets the order: loss recovery and receive latency before throughput,
and the hardening a client needs before the hardening a server needs. The RFCs are the specification; where
they leave a choice, Aloe does what Linux does, and Linux's packetdrill corpus is the reference for what Linux
does; the scenario scripts are Aloe's own. The steps, in order:

1. **Baseline benchmark.** Tick-to-trade and the gateway round through the bricks and through the runtime,
   against a floor of raw frames, on DPDK's ring driver with a scripted venue on a second core; per-message
   distributions, recorded in [docs/benchmarks/tick-to-trade.md](benchmarks/tick-to-trade.md) before any step
   adds work to the hot path. Every later step reruns it. The loads and the isolated round that inform the
   two-core shard follow in a second part. The benchmark exists; its first recorded run waits for a quiet
   machine with hugepages.
2. **Packet loan and test tools.** A packet the stack must keep after sending is lent to the device and comes
   back when the device is done with it: TCP sees ownership move and return, never a reference count. The
   DPDK backend implements the loan with the mbuf's reference count, the fabric by holding the packet. A
   packet still on loan is never rewritten: as in Linux and lwIP, a retransmit waits until the first copy has
   left the host. Scripted loss, duplication, reordering and delay in the fabric, and a runner for scenario
   scripts in packetdrill's syntax: Aloe's own scripts, with Linux's corpus as the reference for what Linux
   does. TCP's behaviour does not change in this step. Landed; the loan is a device and loop capability until
   step 3 lends from `commit`. See the [device layer](architecture/device.md), the
   [fixtures](architecture/fixtures.md) and the [scenario runner](architecture/scenario.md).
3. **Reliable send.** The retransmission queue over lent packets, patched in place when they come back; RTO
   estimation (RFC 6298); timestamps for RTT measurement and PAWS (RFC 7323); zero-window probes; fast
   retransmit and NewReno (RFC 5681, RFC 6582) behind a pluggable congestion-control policy. Pre-built segment
   headers per connection, so a send patches sequence, acknowledgement, timestamp and checksum and posts.
4. **Receive side.** Out-of-order segments held as the received packets, without copying; SACK blocks
   (RFC 2018); window scaling (RFC 7323) with a receive pool sized for it; delayed ACK; receiver-side silly
   window avoidance (RFC 1122).
5. **Loss recovery with SACK.** RACK-TLP (RFC 8985), what Linux runs by default. Its tail loss probe resends
   a lost last segment, the end of an order, after about two round trips instead of a full RTO.
6. **Lifecycle and client hardening.** TIME_WAIT, keepalive, the FIN-WAIT-2 timer, ICMP destination
   unreachable and path MTU, and the edge cases the phase 1 reviews left open: a RST for a bad ACK in
   SYN-RECEIVED, refusing a peer MSS of zero, reading the acknowledgement of a segment dropped for the window.
   RFC 6528 initial sequence numbers, RFC 5961 challenge ACKs and ARP-reply rate limiting. The public API
   freezes as v1 at the end of this step.

Left for after this phase, because a client does not need them yet: CUBIC and pacing, since orders are small;
SYN cookies and a keyed flow-table hash, which defend a server that accepts connections from strangers.

Done when the simulation suite passes under scripted impairment, packetdrill-style scenario tests pass,
throughput and latency hold up against Linux under netem, and a soak run shows no leaks.

## 3. TLS

TLS 1.3 client and server over OpenSSL, bound to the Aloe stream so records move without copies. TLS is a
stream layer with the same shape as the layers below it: a brick with `process`, `events`, `send` and
`flush`, and senders over it in the runtime. Its first task makes the runtime's stream wrappers generic over
`stream::IsStream`, with `tls::Stream` as the second implementation that validates the boundary; until then
they are written over `tcp::Stack`. The record layer runs on the connection's core; NIC TLS offload through
DPDK's security API is the later option.

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
- When UDP with multicast returns, after the WebSocket phase or beside it.
- When IPv6 arrives.
- Whether libc++ becomes supported, and whether C++ modules are revisited at the v1 freeze.
- Whether DPDK is pinned to a long-term-support release instead of the version vcpkg ships.
- The two-core shard, one core per direction: designed in phase 1, measured against the single-core shard
  after phase 2.
- Whether `loop::ShardCounters` splits along the product boundary, or stays one struct both fill.

Resolved in phase 1: errors are reported as `std::expected` in the value channel, and the stopped channel
carries cancellation only; abseil does not enter the project, since the connection table and the ARP cache
are fixed open-addressing tables of Aloe's own. Resolved before phase 2: the loop reads the TSC. `core::Clock`
is a calibrated TSC clock by default, and a build option selects `steady_clock` for machines that cannot trust
the TSC, CI's runners among them; see the [core page](architecture/core.md).
