# TCP Module

The tcp module (`common/tcp/`) is the minimal TCP brick: one `tcp::Stack` per shard, over that shard's
[`net`](net.md) IPv4 brick and its [loop](loop.md)'s timer wheel. It opens connections in both directions,
carries in-order data both ways without copying it, closes with FIN and RST, and nothing more: phase 1 of the
[roadmap](../roadmap.md). The brick has four verbs: `process` the segments IP sorted out, `poll_event` to
drain notifications, the connections' own non-blocking operations, and `flush` for the ACKs that waited.
Every connection is a [stream](stream.md): the contract TLS and WebSocket reuse later. Everything is reached
through the umbrella `#include <aloe/tcp>` (`export/aloe/tcp`), and targets link `Aloe::Common::Tcp`. It
depends on [`net`](net.md), [`stream`](stream.md), [`loop`](loop.md), [`device`](device.md),
[`wire`](wire.md) and [`core`](core.md), and not on [`execution`](execution.md), [`log`](log.md) or DPDK:
nothing here names the asynchronous model, logs, reads a clock or blocks.

## Key types

- **`aloe::tcp::Stack<Ip>`** -- the brick, templated on the IP brick below it (`net::Ipv4<Device>`).
  Constructed from the IP brick, a `loop::TimerWheel&` and a `TcpConfig`, which it validates (throwing
  `std::invalid_argument`); every slot, node and list is allocated there and never again.
  `process(segments, now)` takes `ip.received(Ipv4Protocol::Tcp)` and leaves every packet moved out or
  released; `poll_event()` removes one pending notification; `flush(now)` sends the ordinary ACKs that
  waited. `listen(port)` and `unlisten(port)` manage listeners; `connect(peer, now)` returns a `SynSent`
  connection the caller owns, or a `ConnectError`; the slot is the caller's even while its SYN waits for
  ARP. `counters()`, `pending_events()`, `table_size()` and `nodes_available()` are for tests and
  monitoring. The stack keeps the stamp of the last `process`, `flush` or `connect` and uses it for
  everything a connection does in between. It names its types `ConnectionType` (`Connection<Ip>`) and
  `EventType` (`ConnectionEvent<Ip>`).
- **`aloe::tcp::Connection<Ip>`** -- one connection, a slot of its stack with a stable `index()` and address
  until `release`. It models `stream::IsStream`: `unread()` and `consume(n)` on the receive side;
  `writable()`, `prepare(n)`, `commit(n)` and the copying convenience `send(bytes)` on the transmit side;
  `close()`, `abort()` and `release()`. TCP adds `state()`, `local()`, `remote()`, `mss()`, `committed()`,
  `acknowledged()` and `unacknowledged()`.
- **`aloe::tcp::ConnectionEvent<Ip>`** -- what `poll_event` returns: the connection and the
  `stream::Events` taken from it. The flags are edges; `poll_event` and `release` are the only things that
  clear them.
- **`aloe::tcp::ReadView<Packet>`** -- `unread()`: a forward range of `std::span<const std::byte>`, one span
  per held packet's unconsumed payload, over the packets themselves. `size()` counts bytes. Invalidated by
  `consume` and by the next `process`.
- **`aloe::tcp::TcpConfig`** -- slots per shard (`connections`), `listeners`, the per-connection packet cap
  `receive_segments` (which also sizes the byte window), the shard's shared `receive_pool` of held-packet
  nodes, the ephemeral port range, and the retry schedule: `retry_initial`, `retries` and
  `unresolved_retry`.
- **`aloe::tcp::TcpCounters`** -- monotonic, one per drop reason and one per send failure: what was sent by
  kind, connections opened, accepted, closed, reset and timed out, every reason a segment was dropped, and
  `send_refused`, `send_unresolved`, `commits_refused` and `allocation_failures`.
- **`aloe::tcp::FlowTable`** -- the fixed open-addressing index from a `FlowKey` (remote address, remote
  port, local port) to a slot: linear probing, backward-shift deletion, at most half full, no allocation
  after construction.
- **`aloe::tcp::State`** -- the RFC 793 states this phase has: `Closed`, `SynSent`, `SynReceived`,
  `Established`, `FinWait1`, `FinWait2`, `CloseWait`, `Closing`, `LastAck`. There is no `TimeWait`.
- **`aloe::tcp::ConnectError`** -- `TableFull`, `NoPort`, `Unplaceable`, `NoRoute`.
  **`aloe::tcp::ListenError`** -- `InUse`, `TableFull`. **`aloe::tcp::Endpoint`** -- an address and a port.

## Usage

An echo server, a hand-written loop over one queue. One tick receives, lets the stack process, advances the
wheel, drains the events, then flushes the stack and the queue. This is the loop the two-brick test runs over
two fabric ports and `examples/tcp_echo` runs over a DPDK port:

```cpp
using Clock = aloe::core::Clock;
using Ip    = aloe::net::Ipv4<aloe::fabric::Port>;
using Tcp   = aloe::tcp::Stack<Ip>;

aloe::loop::ShardCounters counters;
aloe::loop::ShardQueue<aloe::fabric::Port> queue{port, 0, 64, counters};
Ip ip{queue, {.address = {10, 0, 0, 10}, .prefix = 24}};
aloe::loop::TimerWheel wheel{std::chrono::milliseconds{1}, Clock::now()};
Tcp tcp{ip, wheel, {}};
std::vector<aloe::fabric::Packet> burst(64);

std::ignore = tcp.listen(7);
while (running) {
    const auto now             = Clock::now();
    const std::size_t received = queue.receive(burst);
    ip.process(std::span{burst}.first(received), now);
    tcp.process(ip.received(aloe::wire::Ipv4Protocol::Tcp), now);
    std::ignore = wheel.advance(now);  // timer events join the receive events before the drain

    while (const auto event = tcp.poll_event()) {
        Tcp::ConnectionType& c = *event->connection;
        if (event->events.readable() || event->events.writable()) {
            echo(c);
        }
        if (c.peer_closed() && c.unread().empty() && c.state() == aloe::tcp::State::CloseWait) {
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

The drain is explicit: nothing calls back into the program, and the program decides what to do with each
connection and in which order. A client is the same loop with `tcp.connect(peer, Clock::now())` before it
and a send on `Connected`. The echo is the one place a byte is copied, because an echo is defined as sending
back what came in; the copy goes straight from the received packet into the packet that leaves:

```cpp
void echo(Tcp::ConnectionType& c) {
    while (!c.unread().empty()) {
        const std::span<const std::byte> chunk = c.unread().front();
        const auto out                         = c.prepare(chunk.size());
        if (!out) {
            return;  // no room now: resume on Writable
        }
        std::ranges::copy(chunk.first(out->size()), out->begin());
        if (!c.commit(out->size())) {
            return;  // refused: the bytes are still unread, retry on the Writable hint
        }
        c.consume(out->size());
    }
}
```

Consuming after the commit keeps the bytes if the commit is refused; the consume then moves the window's edge
after the segment has left, which costs a window update at the flush. An application that produces its own
messages has nothing to copy: it encodes straight into the span `prepare` returns and commits what it wrote.
`send(bytes)` is the copying convenience for bytes that already exist elsewhere, and returns the prefix it
accepted.

A program over a multi-queue device runs one such loop per queue, each with its own IP brick, TCP brick and
wheel. Every queue shares one address, so the ARP reply to a request one loop sent can arrive on another
queue: each loop sets `net::Ipv4Config::accept_unsolicited_replies` and forwards what `ip.resolved()`
reports to the others, as the [net page](net.md) describes.

### As tasks

The [runtime](runtime.md) writes this loop for you. `runtime::TcpStack<Device>` composes the IP brick, the TCP
brick and the stream owner for each shard, and a task per connection awaits the senders the
[stream page](stream.md) lists:

```cpp
using Stack = aloe::runtime::TcpStack<aloe::ethdev::Port>;

aloe::runtime::task<void> echo(Stack::StreamType stream) {
    for (;;) {
        const auto readable = co_await stream.readable(1);
        if (!readable) {
            break;  // the peer closed, reset or timed out
        }
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

aloe::runtime::task<void> serve(Stack::StreamsType* streams, aloe::runtime::Scheduler scheduler) {
    for (;;) {
        auto stream = co_await streams->accept(7);
        if (!stream) {
            break;
        }
        scheduler.spawn(echo(std::move(*stream)));
    }
}
```

The task is the loop's echo with the waiting written as `co_await`: the brick, the drain and the flush are the
same calls, made by the shard's step.

### The examples

`examples/tcp_echo` is the hand-written loop on one queue of a DPDK port; `examples/tcp_echo_tasks` is the
same echo as tasks over `Runtime` and `TcpStack` on every queue the port opens. Both take the same arguments,
listen or connect, and print their counters when they stop:

```bash
tcp_echo <port> <address>/<prefix> [<gateway>] (--listen <port> | --connect <host>:<port>) [-- <EAL arguments...>]

sudo ./build/debug/examples/tcp_echo/Aloe.Examples.TcpEcho net_tap0 10.78.0.2/24 --listen 7 -- --vdev=net_tap0,iface=aloe0
ip addr add 10.78.0.1/24 dev aloe0 && ip link set aloe0 up && nc 10.78.0.2 7     # in another shell, as root

sudo ./build/debug/examples/tcp_echo_tasks/Aloe.Examples.TcpEchoTasks net_tap0 10.78.0.2/24 --connect 10.78.0.1:7 \
    -- --vdev=net_tap0,iface=aloe0                                                 # against a kernel echo server on port 7
```

The client sends one line, prints its echo and closes; the server echoes until interrupted.

## Counters

`TcpCounters`, one per stack, monotonic, read on the owning thread or after it has stopped. Every drop and
every send failure increments exactly one counter.

| Group       | Counter                 | Counts                                                                                          |
|-------------|-------------------------|-------------------------------------------------------------------------------------------------|
| Segments    | `segments_received`     | Segments handed to `process`.                                                                   |
|             | `data_segments_sent`    | Committed data segments queued.                                                                 |
|             | `pure_acks_sent`        | ACKs without data, from `flush` and the immediate-ACK exceptions.                               |
|             | `control_segments_sent` | SYN, SYN-ACK and FIN, first sends and retransmits alike.                                        |
|             | `resets_sent`           | RSTs, from `abort`, `release` and the replies to segments with no connection.                   |
|             | `retransmits`           | Timer-driven control retransmits.                                                               |
|             | `window_updates`        | Segments that extended the advertised right edge.                                              |
| Connections | `connections_opened`    | Active opens that reached `Established`.                                                        |
|             | `connections_accepted`  | Passive opens handed to the application by `Accepted`.                                          |
|             | `connections_closed`    | Normal closes, both FINs acknowledged.                                                          |
|             | `connections_reset`     | RSTs received on a connection the application owns.                                             |
|             | `connections_timed_out` | Owned connections whose control retransmits ran out.                                            |
|             | `handshakes_failed`     | Passive opens nobody owned yet that were reset or timed out, freed silently.                   |
| Drops       | `dropped_bad_header`    | Unparseable headers and malformed option lists.                                                 |
|             | `dropped_bad_checksum`  | A `Bad` verdict, or an `Unknown` one that failed the software check.                            |
|             | `dropped_no_connection` | No connection and no listener; answered with RST unless it was one.                             |
|             | `dropped_closed`        | Segments for a `Closed` connection not yet released; answered with RST.                         |
|             | `dropped_unexpected`    | Valid segments the state does not admit: a stray answer in `SynSent`, an ACK of unsent data, data after the peer's FIN. |
|             | `dropped_duplicate`     | Data entirely before `rcv_nxt`.                                                                 |
|             | `dropped_out_of_order`  | Data starting after `rcv_nxt`, above a gap.                                                     |
|             | `dropped_out_of_window` | Data reaching past the advertised edge.                                                         |
|             | `dropped_no_slot`       | Data for a connection already holding `receive_segments` packets.                              |
|             | `dropped_no_node`       | Data when the shard's `receive_pool` is empty.                                                  |
|             | `dropped_table_full`    | SYNs to a listener with no free connection slot.                                                |
| Send        | `send_refused`          | Segments IP or the queue refused; the numbers stay as they were.                               |
|             | `send_unresolved`       | Segments that waited for ARP.                                                                   |
|             | `commits_refused`       | Commits after the connection left `Established` and `CloseWait`; the prepared packet is dropped. |
|             | `allocation_failures`   | `prepare` calls the packet pool could not serve; the retry hint says when to try again.         |

The runtime's `TcpStack` adds two counters of its own, `forwards_dropped()` and `forwards_rejected()`, for
ARP forwards between shards (see the [runtime page](runtime.md)).

## Configuration

`TcpConfig` is validated by the `Stack` constructor, which throws `std::invalid_argument` for a config it
cannot run. Every shard gets the same config.

| Field              | Default  | Meaning                                                                         | Rejected when                                  |
|--------------------|----------|---------------------------------------------------------------------------------|------------------------------------------------|
| `connections`      | 1024     | Connection slots per shard; the flow table has at least twice as many buckets. | zero, or more than 2^30                        |
| `listeners`        | 8        | Listening ports per shard.                                                      | zero                                           |
| `receive_segments` | 32       | Packets one connection may hold; also sizes the byte window, `min(receive_segments * mss, 65535)`. | zero                  |
| `receive_pool`     | 2048     | Held-packet nodes shared by every connection of the shard.                      | zero                                           |
| `ephemeral_first`, `ephemeral_last` | 32768, 60999 | `connect`'s local ports, inclusive.                         | empty, or containing port 0                    |
| `retry_initial`    | 1 s      | The first control retransmit's delay; it doubles per try.                       | not positive, or too large to double `retries` times within `core::Duration` |
| `retries`          | 5        | Control retransmits before `TimedOut`.                                          | zero, or 63 and more                           |
| `unresolved_retry` | 10 ms    | The poll interval while a control segment waits for ARP; not a try.             | not positive                                   |

The constructor also refuses an MTU that leaves no room for a TCP header. The retry bound keeps the longest
delay and the whole schedule within a quarter of `core::Duration`'s range, so no arithmetic on the timer path
can overflow. Size the device's packet pool for the receive descriptors, `receive_pool` held packets, the
transmit ring and what the card holds, and the open preparations: `receive_pool` is not carved out of it.

## Design notes

**Zero-copy both ways.** A received segment's packet is held as it arrived and `unread()` points into it;
`consume` advances through it and returns each packet to the pool once it is empty. A sent segment is written
by the application into a fresh packet from IP, and `commit` prepends the TCP header in its headroom and
hands the packet to IP, which prepends the rest. The data path holds no stream buffer and no copy the
application did not write itself.

**Two limits on what is held, independent of each other.** The receive window is a byte window: the stack
offers `rcv_nxt + (B - unread)`, where `B` is `min(receive_segments * mss, 65535)`, and only extends the
edge it offered, never retracts it. Separately, a connection holds at most `receive_segments` packets, and
the shard's held packets come from one pool of `receive_pool` nodes. Small segments can reach the packet cap
before the byte window closes. A segment that arrives when either cap is reached is dropped, counted
(`dropped_no_slot` or `dropped_no_node`) and not acknowledged: the immediate ACK reports the last byte
accepted, the peer's retransmission recovers it, and the offered window is never taken back. The edge is
extended only while a packet slot is free, so a connection at its cap stops offering more.

**ACKs wait, with exceptions.** An ordinary acknowledgement of in-order data is queued and sent by `flush`,
unless a data segment leaving first carries it, so a request answered in the same tick costs one segment.
It goes at once for every second full-sized segment, for anything that cannot be accepted (a duplicate, a
segment out of window or above a gap, data after the peer's FIN), while recovering from a gap, and for a FIN
the connection is closing on. A refused immediate ACK stays queued for the next `flush`.

**Ownership is logical state.** A passive open belongs to the stack until the handshake completes and
`Accepted` hands it to the application; one that fails before then is freed silently. A connection from
`connect` is the caller's at once. From `prepare` to `commit` (with any count, zero discards) or `release`,
the prepared packet is the application's: a `close`, `abort`, received RST or time-out in between leaves the
span writable, and the commit that follows drops the packet, returns false, counts `commits_refused` and
raises no retry hint, since the events already say why. After `Closed`, `Reset` or `TimedOut` the held segments stay readable until
`release`, the owner's last call, which aborts a connection that is still open and frees the slot.

**The table hashes on its own; placement chooses ports.** The flow table mixes a base hash into its own
bucket hash. When the card hashes IPv4 TCP tuples the base hash is the card's RSS hash, so a received segment
needs no software hash; otherwise it is a software hash of the key. A connection must live on the shard whose
queue receives its segments. Inbound, the card has already chosen. Outbound, `connect` walks the ephemeral
range and takes the first free port whose replies the card's hash steers back to this queue. When the card
hashes addresses only, or steering is off, the port cannot change the queue: `connect` succeeds on the queue
the addresses select (queue 0 with steering off) and returns `Unplaceable` on every other, rather than open a
connection whose replies go elsewhere. With steering off that is every queue but 0, even with a free port:
every reply lands on queue 0.

**Control segments retry on the wheel.** SYN, SYN-ACK and FIN are retransmitted from the connection's timer,
the delay doubling from `retry_initial` each try: with the defaults at 1, 3, 7, 15 and 31 seconds after the first
send, keeping their sequence numbers, and the connection times out at 63 seconds. A connection the application
owns raises `TimedOut`. While IP waits for ARP the timer polls every `unresolved_retry` (10 ms by default)
instead, and those polls are not tries. A known limitation follows: a connect whose next hop never answers
ARP keeps polling and never times out on its own; the application gives up with `abort` or `release`, or, in
the runtime, a deadline.

**The terminal ACK goes before `Closed`.** When the last segment of a close needs an ACK from us, the ACK is
queued before `Closed` is raised. If IP refuses it, the connection keeps its state, `flush` retries the ACK,
and `Closed` follows only once it has left. A duplicate FIN is acknowledged again without advancing the
receive sequence twice.

**Hot paths are bounded.** Every operation after construction is `noexcept` and allocates nothing but packets
from IP's pool. Events go through one function; the event, ACK and retry lists are intrusive in the
connection, so raising, queueing and draining are constant time.

**Compared with others.** Linux copies between socket buffers and user memory at both ends, and runs the
stack in the kernel behind system calls. Seastar's native stack is also shard per core over DPDK, but owns the
loop and exposes connections through futures. TLDK, fd.io's DPDK TCP library, is the closest in shape: a
library the application drives from its own loop. Aloe keeps the received packets as the read buffer, lets the
application write into the packet that leaves, and leaves the loop to the program or to the runtime written
over the same bricks.

**What phase 1 leaves out, and why.** No retransmission of data: a lost data segment is recovered only by the
peer, and two minimal Aloe peers cannot recover lost data from each other until phase 2. No storage of
out-of-order segments: they are dropped and the peer resends. No TIME_WAIT: the slot is the owner's until
`release`. No SACK, window scaling or timestamps, and no congestion control. These come with the full TCP of
phase 2, which needs the fabric's scripted loss to test them. The brick makes no deployment or latency claim
yet; the benchmarks come with the phases that measure them.

**Hardening is deferred too.** The brick is not yet meant for hostile peers. It has no SYN cookies, so a SYN
flood fills the fixed table until the half-open connections time out; its initial sequence numbers come from a
per-shard random engine, not RFC 6528's keyed hash; it accepts a RST anywhere in the window, without RFC 5961's
challenge ACKs; the flow table's software hash is unkeyed, so chosen 4-tuples can lengthen a probe, bounded by
the fixed table; and on a multi-queue device the IP brick accepts unsolicited ARP replies to our address,
with no rate limit. Each is a hardening step for full TCP, beside data retransmission, TIME_WAIT and
out-of-order storage.
