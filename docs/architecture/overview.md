# Aloe Architecture

Aloe is a userspace TCP/IP stack on DPDK for programs whose latency budget is measured from a tick to a
send: order gateways, feed handlers, and the servers that talk to them. It is a library first: a set of
bricks a program writes its own loop over, each a plain call that returns before anything waits. On top of
the bricks sits a runtime that writes the loop for you and offers senders and receivers, and later a stream
API in the spirit of Boost.Beast, for the code that waits. Three goals rank every trade-off, in this order:
**predictable low latency on the data path**, **high throughput**, and **long-lived connections**.

This page describes the design the stack is built towards. Today the repository holds the build skeleton, the
[`core`](core.md), [`execution`](execution.md) and [`log`](log.md) modules, the protocol formats in
[`wire`](wire.md), the [device layer](device.md), the [loop bricks](loop.md),
the [shard runtime](runtime.md), the IP base ([`net`](net.md)), the [stream](stream.md) contract, minimal
[TCP](tcp.md) with its senders in the runtime and, apart from the stack, the [fixtures](fixtures.md) for
testing code written over the bricks; the rest arrives phase by phase, as the [roadmap](../roadmap.md) lays
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
- **A future shard may favor one direction on two cores.** A gateway may give TX the shortest path
  and place maintenance beside RX; a feed handler may favor RX and place maintenance beside TX.
  Minimal TCP runs receive, transmit and maintenance on one owning thread per connection. Phase 1
  establishes their state ownership and explicit interactions so a later design can evaluate these
  compositions. Concurrent execution needs a publication, feedback, resource and teardown contract;
  substituting a seqlock or ring for a record is not sufficient. Protocol feedback still needs timely
  service to keep the favored direction progressing. The choice of synchronization and maintenance
  placement follows measurement and is not part of the minimal TCP implementation.

Status: the single-core shard exists, as the rule the [loop bricks](loop.md) follow and as the
[runtime](runtime.md)'s `Shard`. TCP's ownership boundaries and its placement by the card's hash and the
choice of local port exist in [tcp](tcp.md). Flow rules and concurrent execution of one connection's halves
remain deferred.

## Two products

**Bricks.** The [`loop`](loop.md) module and the protocol modules, [`net`](net.md) and [`tcp`](tcp.md) so
far. Every brick is a library that depends on the layer below it, calls nothing above it, and reports what
happened as a list the caller drains. None of them names the asynchronous model. The loop a program writes
over them is the product's primary form: its thread, its order, and nothing calls it back. A TCP gateway's
writer core:

```cpp
using Clock = aloe::core::Clock;
using Ip    = aloe::net::Ipv4<aloe::ethdev::Port>;
using Tcp   = aloe::tcp::Stack<Ip>;

aloe::loop::ShardQueue<aloe::ethdev::Port> queue{port, 0, 512, counters};
aloe::loop::TimerWheel wheel{std::chrono::milliseconds{1}, Clock::now()};
Ip ip{queue, ip_config};                                              // IPv4 with its ARP and ICMP echo
Tcp tcp{ip, wheel, config};                                           // depends on the layer below, on nothing above
Tcp::ConnectionType* session = tcp.connect(exchange, Clock::now()).value();  // the SYN is queued, or waits for ARP

for (;;) {
    const auto now      = Clock::now();
    const std::size_t n = queue.receive(burst);
    ip.process(std::span{burst}.first(n), now);                      // ARP and ping answered; datagrams sorted per transport
    tcp.process(ip.received(aloe::wire::Ipv4Protocol::Tcp), now);   // segments in; immediate ACKs queued
    std::ignore = wheel.advance(now);                                // control retransmits, before the drain

    while (const auto event = tcp.poll_event()) {                    // the explicit drain: a connection and its flags
        Tcp::ConnectionType& c = *event->connection;
        if (event->events.readable()) {
            c.consume(decode(c.unread(), reports));                  // a zero-copy view over the received packets
        }
    }
    while (const Order* order = orders.peek()) {                     // the ring a strategy core fills
        const auto room = session->prepare(max_order_size);          // a span inside the packet that will leave
        if (!room || !session->commit(encode(*order, *room))) break; // no window or no packet: keep the order
        orders.pop();                                                // one order, one segment
    }
    tcp.flush(now);                                                  // the ACKs no data carried
    std::ignore = queue.flush();                                     // to the transmit burst, now
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

Status: `loop`, `runtime`, the IP brick `net`, the TCP brick `tcp` and the connection senders over it
exist; `examples/tcp_echo` and `examples/tcp_echo_tasks` are the same echo in both products. UDP arrives with
the rest of phase 1.

## Layers

Protocols compose at compile time, each layer templated over the one below it: TCP over IPv4 over a device
queue, then TLS over TCP, then WebSocket over TLS over TCP. The templates point downward only. A layer is
parameterised by the device and its packet type, so the hot path inlines, and never by whoever reads it,
which is what keeps a layer changeable and lets two halves run on two cores by composition.

Every protocol brick has the same four verbs: feed it a burst, drain its events, call its non-blocking
operations, flush. The [stream](stream.md) concept that TLS and WebSocket reuse is defined in these terms:
`process`, `poll_event`, `unread` and `consume`, `prepare` and `commit` with `send` as the copying
convenience, `flush`; the connection senders in the runtime are expressed over them. Phase 1 gets the most design care although its code is the simplest, because the v1
freeze at the end of phase 2 fixes that shape.

The seam between IPv4 and the transports is a list: `net::Ipv4::process` sorts the datagrams it
accepts into one list per transport, and the loop hands `received(Tcp)` to TCP and `received(Udp)` to
UDP. IPv4 calls nobody above it. ARP is private to the brick, as it is to the kernel.

Protocol headers include `<aloe/core>`, `<aloe/wire>`, `<aloe/loop>` and `<aloe/device>` and never `<aloe/execution>` or
`<aloe/log>`. No target below the runtime links `execution` or `log`, so stdexec and quill stay out of the
data path by construction, not by review.

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

Status: the device layer, the loop, the IP base and minimal TCP exist, see [device](device.md),
[loop](loop.md), [net](net.md) and [tcp](tcp.md). UDP arrives with the rest of phase 1, TLS in phase 3,
HTTP/1.1 and WebSocket in phase 4.

## Senders and receivers

The runtime's asynchronous model is senders and receivers: stdexec today, `std::execution` once the standard
library ships it. Code names these facilities only through the [`execution`](execution.md) module, so the switch is a
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
- **Two completions on send.** One when the bytes are committed to the transmit ring (`send`), and one when
  the peer has acknowledged them (`acked`). There is no send queue between the two: a committed segment is
  already a packet. Both are events the brick reports.
- **Level-triggered cancellation.** Each connection owns a stop source. A stop request wakes a parked
  operation and marks the connection closed, so any operation started afterwards completes stopped at once.
- **Deadlines as one timer per connection.** A connection's wait slot carries one timer node in the shard's
  wheel; its expiry is a cancel, which stops every wait of the connection. There is one wheel per shard,
  never a timer per operation.
- **A per-shard counting scope.** One task per connection runs in it. Requesting stop and waiting for it to
  empty is graceful shutdown.
- **A task type bound to the shard scheduler.** The connection's memory arena reachable from inside it comes
  later.

Status: the scheduler, the timer senders, the counting scope, the stop plumbing and the shard-bound task
exist in [runtime](runtime.md), on the C++26 task type from P3552, and so do the connection leaf senders
over TCP's events: accept, connect, readable, writable, send with `acked` as its second completion, close,
and the per-connection deadline. Their outcomes are `std::expected` values; the stopped channel carries
cancellation only.

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
