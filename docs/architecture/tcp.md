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
  connection the caller owns, or a `ConnectError`. `counters()`, `pending_events()`, `table_size()` and
  `nodes_available()` are for tests and monitoring. The stack keeps the stamp of the last `process`, `flush`
  or `connect` and uses it for everything a connection does in between.
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

An echo server and its client, each a hand-written loop over one queue. One tick receives, lets the stack
process, advances the wheel, drains the events, then flushes the stack and the queue. This is the loop the
two-brick test runs over two fabric ports:

```cpp
aloe::loop::ShardCounters counters;
aloe::loop::ShardQueue<aloe::fabric::Port> queue{port, 0, 64, counters};
aloe::net::Ipv4<aloe::fabric::Port> ip{queue, {.address = {10, 0, 0, 10}, .prefix = 24}};
aloe::loop::TimerWheel wheel{1ms, now};
aloe::tcp::Stack<aloe::net::Ipv4<aloe::fabric::Port>> tcp{ip, wheel, {}};
std::vector<aloe::fabric::Packet> burst(64);

std::ignore = tcp.listen(7);
while (running) {
    const auto now             = std::chrono::steady_clock::now();
    const std::size_t received = queue.receive(burst);
    ip.process(std::span{burst}.first(received), now);
    tcp.process(ip.received(aloe::wire::Ipv4Protocol::Tcp), now);
    std::ignore = wheel.advance(now);

    while (const auto event = tcp.poll_event()) {
        auto& c = *event->connection;
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
connection and in which order. The echo is the one place a byte is copied, because an echo is defined as
sending back what came in; the copy goes straight from the received packet into the packet that leaves:

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
            return;
        }
        c.consume(out->size());
    }
}
```

An application that produces its own messages has nothing to copy: it encodes straight into the span
`prepare` returns and commits what it wrote. `send(bytes)` is the copying convenience for bytes that already
exist elsewhere, and returns the prefix it accepted.

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
the prepared packet is the application's: a close or reset in between leaves the span writable, and the
commit that follows is refused. After `Closed`, `Reset` or `TimedOut` the held segments stay readable until
`release`, the owner's last call, which aborts a connection that is still open and frees the slot.

**The table hashes on its own; placement chooses ports.** The flow table mixes a base hash into its own
bucket hash. When the card hashes IPv4 TCP tuples the base hash is the card's RSS hash, so a received segment
needs no software hash; otherwise it is a software hash of the key. A connection must live on the shard whose
queue receives its segments. Inbound, the card has already chosen. Outbound, `connect` walks the ephemeral
range and takes the first free port whose replies the card's hash steers back to this queue. When the card
hashes addresses only, or steering is off, the port cannot change the queue: `connect` succeeds on the queue
the addresses select (queue 0 with steering off) and returns `Unplaceable` on every other, rather than open a
connection whose replies go elsewhere.

**Control segments retry on the wheel.** SYN, SYN-ACK and FIN are retransmitted from the connection's timer,
the delay doubling from `retry_initial` each try: with the defaults at 1, 3, 7, 15 and 31 seconds after the first
send, keeping their sequence numbers, and the connection times out at 63 seconds. A connection the application
owns raises `TimedOut`. While IP waits for ARP the timer polls every `unresolved_retry` (10 ms by default)
instead, and those polls are not tries.

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
