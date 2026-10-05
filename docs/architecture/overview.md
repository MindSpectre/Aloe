# Aloe Architecture

Aloe is a userspace TCP/IP stack on DPDK for programs whose latency budget is measured from a tick to a
send: order gateways, feed handlers, and the servers that talk to them. It is a library first: a set of
bricks a program writes its own loop over, each a plain call that returns before anything waits. On top of
the bricks sits a runtime that writes the loop for you and offers senders and receivers, and later a stream
API in the spirit of Boost.Beast, for the code that waits. Three goals rank every trade-off, in this order:
**predictable low latency on the data path**, **high throughput**, and **long-lived connections**.

This page describes the design the stack is built towards. Today the repository holds the build skeleton, the
[`core`](core.md) and [`utils`](utils.md) modules, the [device layer](device.md), the [loop bricks](loop.md),
the [shard runtime](runtime.md) and the IP base ([`net`](net.md)); the rest arrives phase by phase, as the [roadmap](../roadmap.md) lays
out. Each section says what exists and what is still design.

## The shard, as a rule

A shard is one core that owns one receive queue and one transmit queue of the network card, its packet
buffer pool, its ARP cache, its connection table, its timer wheel and whatever work it runs. It is the only
writer of all of it. Work on a shard runs to completion, so the data path takes no locks and touches no
atomics. A connection belongs to one shard for its whole life: every packet of it is received, processed
and answered on that core.

The shard is a rule about who writes what, not an object that owns a thread. Nothing in Aloe starts a
thread, claims a core or runs a loop unless the program asks the runtime to. A program that already has
readers, writers and balancers on pinned cores keeps them and gives each one a queue, a stack and a wheel.

- **Inbound packets must reach the owning shard.** A gateway with a few sessions places each connection
  by a flow rule, exact 4-tuple to queue. A server with many connections is placed by the card's
  receive-side scaling hash, and for outbound connections the local port is chosen so the hash lands on the
  wanted shard. Software steering between shards is the last resort.
- **A shard may be two cores, one per direction.** TCP is designed as a receive half and a transmit half.
  The receive half is the only writer of acknowledgement, window and round-trip state; the transmit half is
  the only writer of the send buffer and owns the retransmit timer; they exchange a small record. On one
  core that is two calls in sequence. On two cores it is a seqlock or a ring, and each half transmits on
  its own queue. The first implementation runs both halves in one loop. The split is a composition, not a
  redesign, and that is a phase 1 design requirement.

Status: the single-core shard exists, as the rule the [loop bricks](loop.md) follow and as the
[runtime](runtime.md)'s `Shard`. Flow rules, the two halves and a connection's owning shard arrive with TCP
in phase 1.

## Two products

**Bricks.** The [`loop`](loop.md) module and, from phase 1, the protocol modules. Every brick is a library
that depends on the layer below it, calls nothing above it, and reports what happened as a list the caller
drains. None of them names the asynchronous model. The loop a program writes over them is the product's
primary form: its thread, its order, and nothing calls it back. The design for a TCP gateway's writer core,
with the TCP names still to be fixed in phase 1:

```cpp
aloe::loop::ShardQueue<aloe::ethdev::Port> queue{port, 0, 512, counters};
aloe::loop::TimerWheel wheel{std::chrono::milliseconds{1}, Clock::now()};
aloe::net::Ipv4<aloe::ethdev::Port> ip{queue, ip_config};            // IPv4 with its ARP and ICMP echo
aloe::tcp::Stack<aloe::net::Ipv4<aloe::ethdev::Port>> tcp{ip, wheel, config};   // depends on the layer below, on nothing above
aloe::tcp::Connection& session = tcp.connect(exchange);              // non-blocking; the SYN leaves at the next flush

for (;;) {
    const auto now      = Clock::now();
    const std::size_t n = queue.receive(burst);
    ip.process({burst.data(), n}, now);                              // ARP and ping answered; datagrams sorted per transport
    tcp.process(ip.received(aloe::device::Ipv4Protocol::Tcp), now);  // segments in; acks and retransmits queued

    for (aloe::tcp::Connection& c : tcp.events()) {                  // intrusive list: readable, connected, closed, writable
        if (c.readable()) {
            c.consume(decode(c.unread(), reports));
        }
    }
    while (const Order* order = orders.peek()) {                     // the ring a strategy core fills
        if (!session.send(encode(*order))) break;                    // window or ring full: keep the order
        orders.pop();
    }
    std::ignore = queue.flush();                                     // to the transmit burst, now
    std::ignore = wheel.advance(now);                                // TCP's timers, a plain call
}
```

A UDP feed on the same core is a second stack in the same loop. The reconnect and logon logic is a state
machine the program writes, or a task in the runtime for whoever wants one.

**Runtime.** The [`runtime`](runtime.md) module is the same loop written for you: one thread per queue,
pinned, with the drain protocol, counters, a scheduler, timers as senders, a counting scope and a coroutine
task. It is built only from the bricks, and its step is the loop above with one extra pass that wakes
whatever parked on a connection. It pays for that per awaited event, one run-queue push, one indirect call
and one coroutine resume, and never per packet. A program that outgrows it drops to the bricks and keeps
every line of protocol code.

**The rule between them.** A capability lands in the bricks first, as a plain call or an event a caller
drains, and the runtime wraps it in a sender afterwards. The runtime never has a capability the bricks
lack. The documentation and the examples lead with the bricks.

Status: `loop`, `runtime` and the first protocol brick, `net`, exist. The
transport bricks and the connection senders arrive in phase 1.

## Layers

Protocols compose at compile time, each layer templated over the one below it: TCP over IPv4 over a device
queue, then TLS over TCP, then WebSocket over TLS over TCP. The templates point downward only. A layer is
parameterised by the device and its packet type, so the hot path inlines, and never by whoever reads it,
which is what keeps a layer changeable and lets two halves run on two cores by composition.

Every protocol brick has the same four verbs: feed it a burst, drain its events, call its non-blocking
operations, flush. The stream concept that TLS and WebSocket reuse is defined in phase 1 in these terms,
`process`, `events`, `unread` and `consume`, `send`, `flush`, with the connection senders in the runtime
expressed over them. Phase 1 gets the most design care although its code is the simplest, because the v1
freeze at the end of phase 2 fixes that shape.

The seam between IPv4 and the transports is a list: `net::Ipv4::process` sorts the datagrams it
accepts into one list per transport, and the loop hands `received(Tcp)` to TCP and `received(Udp)` to
UDP. IPv4 calls nobody above it. ARP is private to the brick, as it is to the kernel.

Protocol headers include `<aloe/loop>` and `<aloe/device>` and never `<aloe/core>`. The `loop` target does
not link `core`, so stdexec stays out of the data path by construction, not by review.

Two goals for TCP follow from the gateway case and are stated here so phase 1 leaves room for them. A send
of many small messages in one tick posts them as one transmit burst of as many segments, never coalesced
into one. A connection may keep pre-built segment headers, so a send patches sequence, acknowledgement,
timestamp and checksum and posts; the header layout chosen in phase 1 must allow it, and the feature itself
is phase 2.

The device layer has two backends: DPDK's ethdev for real network cards and for tap devices, and an
in-memory fabric for deterministic tests. Scripted loss, reordering and delay join the fabric in phase 2.
Network cards vary, so offloads such as checksums and segmentation are queried from the card at run time,
and every offload has a software fallback. Steering is a pure function of what the card reports, so a
program can predict which queue any flow lands on.

Status: the device layer, the loop and the IP base exist, see
[device](device.md), [loop](loop.md) and [net](net.md). UDP and TCP arrive in phase 1, TLS in phase 3,
HTTP/1.1 and WebSocket in phase 4.

## Senders and receivers

The runtime's asynchronous model is senders and receivers: stdexec today, `std::execution` once the standard
library ships it. Code names these facilities only through the [`core`](core.md) module, so the switch is a
one-header change. The protocol bricks know nothing about it. Between them and the senders there is one
seam: the event list a brick reports, which the runtime's step drains to wake parked operation states. On
that seam the runtime provides:

- **A concrete scheduler per shard.** A value type, never type-erased. Scheduling on the same shard pushes
  onto an intrusive run queue with no atomics. Scheduling onto another shard goes through that shard's
  multi-producer inbox.
- **Leaf senders whose operation state is the wait node.** Connect, accept, wait until readable, send,
  close and timers. The operation state lives in the caller's frame and parks in a wait slot the runtime
  keeps per connection, in a parallel array keyed by the connection's stable index, so the brick never
  stores a pointer to a waiter and completing one allocates nothing.
- **Readiness on receive.** The readable sender completes when enough bytes have arrived. The application
  then reads a zero-copy view of the input and consumes what it used, through the same `unread` and
  `consume` a hand-written loop calls.
- **Two completions on send.** One when the bytes are accepted into the send queue, and one when the peer
  has acknowledged them, which is when a zero-copy buffer may be reused. Both are events the brick reports.
- **Level-triggered cancellation.** Each connection owns a stop source. A stop request wakes a parked
  operation and marks the connection closed, so any operation started afterwards completes stopped at once.
- **Deadlines as stamps.** A connection carries a deadline, the shard's timer tick sweeps it, and expiry
  becomes a stop request. There is one wheel per shard, never a timer per operation.
- **A per-shard counting scope.** One task per connection runs in it. Requesting stop and waiting for it to
  empty is graceful shutdown.
- **A task type bound to the shard scheduler,** with the connection's memory arena reachable from inside it.

Status: the scheduler, the timer senders, the counting scope, the stop plumbing and the shard-bound task
exist in [runtime](runtime.md), on the C++26 task type from P3552. The connection leaf senders, readiness
on receive, the two send completions and deadline stamps arrive with TCP in phase 1, over the event list.

## Build decisions

These exist today and shape every later module.

- **C++26 on libstdc++**, with clang 22 and GCC 16. No C++ modules: with the current compilers, module units
  that include stdexec do not build, and DPDK's inline C functions cannot be used from exported templates.
- **DPDK linked statically through one target, `Aloe::Dpdk`.** Its archives are linked whole, because DPDK
  drivers register themselves from static constructors and an ordinary static link drops them. The price is
  size: a debug executable that links DPDK is about 200 MB.
- **DPDK built for the generic x86-64 baseline,** so a cached build starts on any CPU model. See
  [getting started](../guides/getting-started.md#how-dpdk-is-built-and-linked).
- **Tests need no root, no hugepages and no network card.** DPDK starts with a null virtual device.
