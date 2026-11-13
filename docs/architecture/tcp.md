# TCP Module

The tcp module (`common/tcp/`) is the TCP brick: one `tcp::Stack` per shard, over that shard's
[`net`](net.md) IPv4 brick and its [loop](loop.md)'s timer wheel. It opens connections in both directions,
carries in-order data both ways without copying it, closes with FIN and RST, and keeps every segment it sends,
as the packet it left in, until the peer acknowledges it: phase 1 of the [roadmap](../roadmap.md) and step 3 of
phase 2, reliable send. The receive side is still phase 1's. The brick has four verbs: `process` the segments IP
sorted out, `poll_event` to drain notifications, the connections' own non-blocking operations, and `flush` for
the resends and ACKs that waited. Every connection is a [stream](stream.md): the contract TLS and WebSocket
reuse later. Everything is reached through the umbrella `#include <aloe/tcp>` (`export/aloe/tcp`), and targets
link `Aloe::Common::Tcp`. It depends on [`net`](net.md), [`stream`](stream.md), [`loop`](loop.md),
[`device`](device.md), [`wire`](wire.md) and [`core`](core.md), and not on [`execution`](execution.md),
[`log`](log.md) or DPDK: nothing here names the asynchronous model, logs, reads a clock or blocks.

## Key types

- **`aloe::tcp::Stack<Ip, CongestionControl = NewReno>`** -- the brick, templated on the IP brick below it
  (`net::Ipv4<Device>`) and on a congestion control (see [below](#congestion-control)). Constructed from the
  IP brick, a `loop::TimerWheel&` and a `TcpConfig`, which it validates (throwing `std::invalid_argument`);
  every slot, node and list is allocated there and never again. `process(segments, now)` takes
  `ip.received(Ipv4Protocol::Tcp)` and leaves every packet moved out or released; `poll_event()` removes one
  pending notification; `flush(now)` retries the resends that waited for their packet and sends the ordinary
  ACKs that waited. `listen(port)` and `unlisten(port)` manage listeners; `connect(peer, now)` returns a
  `SynSent` connection the caller owns, or a `ConnectError`; the slot is the caller's even while its SYN waits
  for ARP. `counters()`, `pending_events()`, `table_size()`, `nodes_available()`, `send_nodes_available()` and
  `sent_segment(connection, position)`, one entry of a connection's retransmission queue, are for tests and
  monitoring; `force_next_isn(isn)` and `force_recover(connection, recover)` are for tests. The stack keeps the
  stamp of the last `process`, `flush` or `connect` and uses it for everything a connection does in between. It
  names its types `ConnectionType` (`Connection<Ip, CongestionControl>`), `EventType`
  (`ConnectionEvent<Ip, CongestionControl>`) and `Policy` (the congestion control).
- **`aloe::tcp::Connection<Ip, CongestionControl>`** -- one connection, a slot of its stack with a stable
  `index()` and address until `release`. It models `stream::IsStream`: `unread()` and `consume(n)` on the
  receive side; `writable()`, `prepare(n)`, `commit(n)` and the copying convenience `send(bytes)` on the
  transmit side; `close()`, `abort()` and `release()`. TCP adds `state()`, `local()`, `remote()`, `mss()`,
  `timestamps()`, `committed()`, `acknowledged()` and `unacknowledged()`. For tests and monitoring it reports
  the send side: `queued_segments()` (the retransmission queue's length), `delivered()` (payload bytes the peer
  acknowledged), `srtt()` and `rto()` (the estimator, the backoff included), `retransmit_count()` (expiries of
  the retransmission timer since the last ACK of new data, a deferred or refused resend included),
  `congestion_window()` and `congestion_state()` (the policy's),
  `in_recovery()` and `duplicate_acks()` (fast recovery), and `probing()` (zero-window probes running).
- **`aloe::tcp::ConnectionEvent<Ip, CongestionControl>`** -- what `poll_event` returns: the connection and the
  `stream::Events` taken from it. The flags are edges; `poll_event` and `release` are the only things that
  clear them.
- **`aloe::tcp::ReadView<Packet>`** -- `unread()`: a forward range of `std::span<const std::byte>`, one span
  per held packet's unconsumed payload, over the packets themselves. `size()` counts bytes. Invalidated by
  `consume` and by the next `process`.
- **`aloe::tcp::TcpConfig`** -- slots per shard (`connections`), `listeners`, the per-connection packet cap
  `receive_segments` (which also sizes the byte window), the shard's shared `receive_pool` of held-packet
  nodes, the send cap `send_segments` and the shard's `send_pool` of retransmission-queue nodes, the ephemeral
  port range, the retransmission timer's `rto_initial`, `rto_min` and `rto_max`, the caps `retries` and
  `data_retries`, the `user_timeout`, `timestamps`, `retransmit_defers`, and the ARP wait's `unresolved_retry`
  and `unresolved_polls`.
- **`aloe::tcp::TcpCounters`** -- monotonic, one per drop reason and one per send failure: what was sent by
  kind and resent by cause, fast recoveries, duplicate ACKs and round-trip samples, connections opened,
  accepted, closed, reset and timed out, every reason a segment was dropped, and `send_refused`,
  `send_unresolved`, `commits_refused` and `allocation_failures`.
- **`aloe::tcp::IsCongestionControl`**, **`AckInfo`**, **`NewReno`**, **`NoCongestionControl`** -- the policy
  concept, what an ACK of new data tells it, and its two models; see [below](#congestion-control).
- **`aloe::tcp::IsIp`** -- what the brick asks of the IP layer: `allocate`, `send`, `address`, `max_l4_size`,
  `next_hop` and `queue`, and for the frame template `prebuild`, `send_frame` and `generation` (see the
  [net page](net.md)). `net::Ipv4<Device>` models it.
- **`aloe::tcp::FlowTable`** -- the fixed open-addressing index from a `FlowKey` (remote address, remote
  port, local port) to a slot: linear probing, backward-shift deletion, at most half full, no allocation
  after construction.
- **`aloe::tcp::State`** -- the RFC 793 states the brick has: `Closed`, `SynSent`, `SynReceived`,
  `Established`, `FinWait1`, `FinWait2`, `CloseWait`, `Closing`, `LastAck`. There is no `TimeWait`.
- **`aloe::tcp::ConnectError`** -- `TableFull` (no slot, or no retransmission-queue node for the SYN),
  `NoPort`, `Unplaceable`, `NoRoute`. **`aloe::tcp::ListenError`** -- `InUse`, `TableFull`.
  **`aloe::tcp::Endpoint`** -- an address and a port.

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
        if (event->events.closed() || event->events.reset() || event->events.timed_out() ||
            event->events.unresolved()) {
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

Consuming after the commit keeps the bytes if the commit is refused; the consume then frees the window after the segment
has left, which costs a window update at the flush when it at least doubles the window on offer. An application that
produces its own messages has nothing to copy: it encodes straight into the span `prepare` returns and commits what it
wrote. `send(bytes)` is the copying convenience for bytes that already exist elsewhere, and returns the prefix it
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

sudo ./build/debug/examples/tcp_echo_tasks/Aloe.Examples.TcpEchoTasks net_tap0 10.78.0.2/24 --connect 10.78.0.1:7007 \
    -- --vdev=net_tap0,iface=aloe0                                                 # against a kernel echo server on port 7007
```

The client sends one line, prints its echo and closes; the server echoes until interrupted. The kernel's
listener needs a port the host firewall opens: Fedora's firewalld opens only 1025-65535 on a new interface
and rejects the SYN with an ICMP message the stack does not read, so the client retries until it times out.

## Congestion control

The congestion control is a template parameter, the second of `tcp::Stack` and of `runtime::TcpStack`,
constrained by `IsCongestionControl`. A model is a stateless struct: a per-connection `State` the connection
embeds, default-constructible and trivially copyable, and static hooks the stack calls, so there is no virtual
call and no allocation, and a model whose hooks are empty leaves no code behind. The policy answers one
question, how much may be in flight, and is told what happened; the stack keeps every mechanic that is the
same under any policy, from the retransmission queue to the duplicate-ACK count, limited transmit, the
`recover` guard, the retries and the user timeout.

| Hook                                  | Called                                                                     |
|---------------------------------------|----------------------------------------------------------------------------|
| `reset(state, mss)`                   | When the connection reaches `Established`: the initial window.             |
| `window(state)`                       | By `writable()`: the congestion window in bytes.                           |
| `on_ack(state, info)`                 | On an ACK of new data outside fast recovery.                               |
| `on_loss(state, flight, mss)`         | On the third duplicate ACK, as fast recovery starts.                       |
| `on_dupack(state, mss)`               | On each further duplicate ACK in fast recovery.                            |
| `on_partial_ack(state, acked, mss)`   | On an ACK in fast recovery short of `recover`.                             |
| `on_recovery_end(state, flight, mss)` | On the ACK past `recover` that ends fast recovery.                         |
| `on_timeout(state, flight, mss)`      | On the first expiry of the retransmission timer since an ACK of new data.  |
| `on_idle(state, mss)`                 | On a commit into an empty queue more than an RTO after the last data left. |

`AckInfo` carries the bytes the ACK covers, the flight before it, the MSS, the round-trip sample when one was
taken and the stamp, and the fields a delivery-rate estimator reads: the last transmission of the newest entry
the ACK freed, the connection's delivered bytes after the ACK and when that entry was sent, and whether that
send left window unused. No model reads the rate fields yet; they are there so a later model, BBR's, reads
them without a change to the concept or the queue.

- **`NewReno`**, the default: RFC 5681's slow start and congestion avoidance, RFC 6582's recovery and RFC 6928's
  initial window, `min(10 mss, max(2 mss, 14600))`, Linux's `TCP_INIT_CWND`, with an unbounded initial threshold.
  Below the threshold an ACK grows the window by the bytes it covers, at most a segment; at or above it the
  acknowledged bytes are counted and each window of them adds a segment. On loss
  `ssthresh = max(flight / 2, 2 mss)` and `cwnd = ssthresh + 3 mss`; a further duplicate adds a segment; a
  partial ACK deflates the window by the bytes it covers, adds a segment back when it covers at least one, and
  never leaves less than a segment; the full ACK sets `cwnd = min(ssthresh, max(flight, mss) + mss)`, RFC 6582's
  first option; a timeout sets `ssthresh = max(flight / 2, 2 mss)` and `cwnd = mss`; a send after an idle RTO
  restarts at `min(IW, cwnd)`, RFC 5681's restart window, and keeps `ssthresh`, where Linux's `tcp_cwnd_restart`
  first raises it to at least three quarters of the window. Linux's reno runs PRR in recovery instead of the
  inflation and deflation, so its windows differ there; the recovery scripts pin RFC 6582 as written.
- **`NoCongestionControl`**: the window is the largest 32-bit value and every hook is empty, so the peer's
  window is the only limit. Recovery still happens, fast retransmit, the RTO, the retries and the user timeout;
  what goes is the reaction to loss as a congestion signal. It is for private cross-connects, and it is as
  tested as `NewReno`: every scenario script that names no policy runs under both.

A program picks one by naming it; the default is `NewReno`:

```cpp
using Tcp      = aloe::tcp::Stack<Ip>;                                   // NewReno
using CrossTcp = aloe::tcp::Stack<Ip, aloe::tcp::NoCongestionControl>;  // the peer's window only
using Stack    = aloe::runtime::TcpStack<aloe::ethdev::Port, aloe::tcp::NoCongestionControl>;
```

## Counters

`TcpCounters`, one per stack, monotonic, read on the owning thread or after it has stopped. Every drop and
every send failure increments exactly one counter. Until step 6 adds an event for it, the counters are the
signal that a connection left the fast path: one that never does moves none of `retransmits`,
`fast_retransmits`, `retransmits_deferred`, `retransmits_copied`, `zero_window_probes` and `dropped_paws`.

| Group       | Counter                      | Counts                                                                                                                                                                             |
|-------------|------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Segments    | `segments_received`          | Segments handed to `process`.                                                                                                                                                      |
|             | `data_segments_sent`         | Committed data segments queued.                                                                                                                                                    |
|             | `pure_acks_sent`             | ACKs without data, from `flush` and the immediate-ACK exceptions; probes are counted apart.                                                                                        |
|             | `control_segments_sent`      | SYN, SYN-ACK and FIN transmissions: first sends, resends and copies alike.                                                                                                         |
|             | `resets_sent`                | RSTs, from `abort`, `release` and the replies to segments with no connection.                                                                                                      |
|             | `window_updates`             | Segments that extended the advertised right edge.                                                                                                                                  |
|             | `out_of_order_held`          | Segments held above a gap.                                                                                                                                                         |
|             | `out_of_order_released`      | Held out-of-order segments released to make room for a lower sequence.                                                                                                             |
|             | `sack_acks_sent`             | Pure ACKs that carried SACK blocks.                                                                                                                                                |
|             | `duplicate_sacks_sent`       | Of them, those whose first block reported data that arrived twice.                                                                                                                 |
|             | `delayed_acks_sent`          | ACKs the delay timer sent.                                                                                                                                                         |
| Resends     | `retransmits`                | Resends the retransmission timer drives, of any kept segment: one per expiry, whether the resend left, waited or was refused, and one per ACK of the loss episode after a timeout. |
|             | `fast_retransmits`           | Resends duplicate ACKs drive: the third duplicate's, and each partial ACK's in fast recovery.                                                                                      |
|             | `recoveries`                 | Fast-recovery episodes entered.                                                                                                                                                    |
|             | `retransmits_deferred`       | Resends that found their packet still lent after a reclaim and waited for the next flush: one per deferral, and one per tick while a segment's packet and its copy are both lent.  |
|             | `retransmits_copied`         | Resends sent as a copy, after `retransmit_defers` deferrals.                                                                                                                       |
|             | `zero_window_probes`         | Probes sent.                                                                                                                                                                       |
|             | `duplicate_acks`             | Duplicate ACKs received, by RFC 5681's definition.                                                                                                                                 |
|             | `rtt_samples`                | Round-trip samples taken, the handshake's included.                                                                                                                                |
| Connections | `connections_opened`         | Active opens that reached `Established`.                                                                                                                                           |
|             | `connections_accepted`       | Passive opens handed to the application by `Accepted`.                                                                                                                             |
|             | `connections_closed`         | Normal closes, both FINs acknowledged.                                                                                                                                             |
|             | `connections_reset`          | RSTs received on a connection the application owns.                                                                                                                                |
|             | `connections_timed_out`      | Owned connections whose retries or probes ran out, or whose user timeout passed.                                                                                                   |
|             | `connections_user_timed_out` | Of `connections_timed_out`, those the user timeout ended.                                                                                                                          |
|             | `connections_unresolved`     | Owned connections whose next hop never answered ARP within `unresolved_polls` polls.                                                                                               |
|             | `handshakes_failed`          | Passive opens nobody owned yet that were reset, timed out or never resolved, freed silently.                                                                                       |
| Drops       | `dropped_bad_header`         | Unparseable headers and malformed option lists.                                                                                                                                    |
|             | `dropped_bad_checksum`       | A `Bad` verdict, or an `Unknown` one that failed the software check.                                                                                                               |
|             | `dropped_no_connection`      | No connection and no listener; answered with RST unless it was one.                                                                                                                |
|             | `dropped_closed`             | Segments for a `Closed` connection not yet released; answered with RST.                                                                                                            |
|             | `dropped_unexpected`         | Valid segments the state does not admit: a stray answer in `SynSent`, an ACK of unsent data, data after the peer's FIN or a FIN held above a gap.                                  |
|             | `dropped_duplicate`          | Data entirely before `rcv_nxt`, or above a gap where held data covers every byte.                                                                                                  |
|             | `dropped_out_of_order`       | Data above a gap with no room for it: at the cap or with the pool empty, and above every segment held out of order.                                                                |
|             | `dropped_out_of_window`      | Data reaching past the advertised edge.                                                                                                                                            |
|             | `dropped_no_slot`            | Data for a connection already holding `receive_segments` packets.                                                                                                                  |
|             | `dropped_no_node`            | Data when the shard's `receive_pool` is empty, and SYNs to a listener when `send_pool` has no node for the SYN-ACK.                                                                |
|             | `dropped_table_full`         | SYNs to a listener with no free connection slot.                                                                                                                                   |
|             | `dropped_paws`               | Segments whose timestamp is older than `TS.Recent`; acknowledged at once, never kept.                                                                                              |
| Send        | `send_refused`               | Segments IP, the queue or a pool refused, a FIN that found no queue node included; the numbers stay as they were.                                                                  |
|             | `send_unresolved`            | Segments that waited for ARP, a rebuild of the frame template that found no next hop included.                                                                                     |
|             | `commits_refused`            | Commits after the connection left `Established` and `CloseWait`, or whose count the windows no longer cover; the prepared packet is dropped.                                       |
|             | `allocation_failures`        | `prepare` calls the packet pool could not serve and commits that found no queue node, both with a retry hint, and copies the packet pool could not serve.                          |

The runtime's `TcpStack` adds two counters of its own, `forwards_dropped()` and `forwards_rejected()`, for
ARP forwards between shards (see the [runtime page](runtime.md)).

## Configuration

`TcpConfig` is validated by the `Stack` constructor, which throws `std::invalid_argument` for a config it
cannot run. Every shard gets the same config.

| Field                               | Default      | Meaning                                                                                                                                                              | Rejected when                                                                                                         |
|-------------------------------------|--------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------|
| `connections`                       | 1024         | Connection slots per shard; the flow table has at least twice as many buckets.                                                                                       | zero, or more than 2^30                                                                                               |
| `listeners`                         | 8            | Listening ports per shard.                                                                                                                                           | zero                                                                                                                  |
| `receive_segments`                  | 512          | Packets one connection may hold, in order and out of order together; half of what they hold at `mss()` is the default window.                                        | zero, or above `receive_pool`                                                                                         |
| `receive_window`                    | 0            | The byte window when set, capped at what the slots hold; zero is half of what they hold.                                                                             | above 65535 shifted by 14                                                                                             |
| `receive_pool`                      | 2048         | Held-packet nodes shared by every connection of the shard.                                                                                                           | zero                                                                                                                  |
| `send_segments`                     | 32           | Data segments one connection may keep in its retransmission queue: the send cap. The FIN's entry, set aside at Established, is beyond it.                            | zero                                                                                                                  |
| `send_pool`                         | 2048         | Retransmission-queue nodes shared by every connection of the shard; each synchronized connection has one set aside for its FIN.                                      | zero                                                                                                                  |
| `ephemeral_first`, `ephemeral_last` | 32768, 60999 | `connect`'s local ports, inclusive.                                                                                                                                  | empty, or containing port 0                                                                                           |
| `rto_initial`                       | 1 s          | The RTO before the first round-trip sample, Linux's `TCP_TIMEOUT_INIT`; the SYN and SYN-ACK back off from it.                                                        | not positive, outside `rto_min..rto_max`, or too large to double `retries` times within a quarter of `core::Duration` |
| `rto_min`                           | 200 ms       | The floor of the RTO's deviation term, so the RTO is at least `srtt + rto_min`; Linux's `TCP_RTO_MIN`.                                                               | not positive, or below the wheel's resolution                                                                         |
| `rto_max`                           | 120 s        | The RTO's ceiling, where the backoff stops; Linux's `TCP_RTO_MAX`.                                                                                                   | above a quarter of `core::Duration`'s range                                                                           |
| `retries`                           | 5            | Timer-driven resends of the SYN or SYN-ACK before `TimedOut`.                                                                                                        | zero, or 63 and more                                                                                                  |
| `data_retries`                      | 15           | Timer-driven resends of data and the FIN, and unanswered zero-window probes, before `TimedOut`; Linux's `tcp_retries2`.                                              | zero, or 63 and more                                                                                                  |
| `user_timeout`                      | 0 (off)      | RFC 5482's bound: a synchronized connection whose oldest unacknowledged segment was first sent, or whose first unanswered probe went, this long ago ends `TimedOut`. | negative, or above a quarter of `core::Duration`'s range                                                              |
| `timestamps`                        | true         | Offer and use RFC 7323 timestamps.                                                                                                                                   | never                                                                                                                 |
| `sack`                              | true         | Offer and use SACK-permitted: report what is held, and what arrived twice.                                                                                           | never                                                                                                                 |
| `window_scaling`                    | true         | Offer and use RFC 7323 window scaling.                                                                                                                               | never                                                                                                                 |
| `delayed_ack`                       | 0 (off)      | How long an ACK for in-order data may wait, at most 500 ms; zero sends it at the flush of the same tick.                                                             | negative, above 500 ms, or set below the wheel's resolution                                                           |
| `retransmit_defers`                 | 1            | Deferred resends of one segment before the copy; zero copies at once.                                                                                                | never                                                                                                                 |
| `unresolved_retry`                  | 10 ms        | The poll interval while a SYN, SYN-ACK or FIN waits for ARP; not a try.                                                                                              | not positive                                                                                                          |
| `unresolved_polls`                  | 300          | Polls before `Unresolved`: 3 s by default, the bound Linux's three ARP probes give.                                                                                  | zero                                                                                                                  |

The constructor also refuses an MTU that leaves no room for a TCP header. The bounds on `rto_initial`,
`rto_max` and `user_timeout` keep every deadline within a quarter of `core::Duration`'s range, so no arithmetic
on the timer path can overflow, and `rto_min` below the wheel's resolution would ask for a timer finer than the
wheel fires. Size the device's packet pool for the receive descriptors, `receive_pool` held packets, `send_pool`
kept packets and the copies out beside them, the transmit ring and what the card holds, and the open
preparations: neither node pool is carved out of it.

## Design notes

**One class per concern.** `tcp::Stack` is the brick's façade and owner: the four verbs, the accessors, and
the mapping from a connection's state to its public handle. What it does is four procedure classes in
`tcp::detail`, each in its own header in a `detail/` folder, over one `detail::ControlBlock` per connection, RFC
9293's transmission control block as a struct with one part per concern, and one `detail::StackContext` per
shard, what every part reads: the IP brick, the config, the held-segment pool, the counters and the stamp. The
`Receiver` is what a datagram does to a connection: the checksum and lookup, the handshake, RFC 9293's order
on a synchronized connection, and `consume`; it owns nothing. The `AckProcessor` is what an acknowledgement
does to the send side: the entries it frees, the round-trip sample, loss recovery and the resends it asks for,
the peer's window and the zero-window probes; it owns nothing either, and tells the `Receiver` what moved.
The `Lifecycle` owns the connection table, the ISN generator and the event list: slots in and out, every
change of a connection's state from the open to `Closed`, `close`, `abort` and `release`. It is the only part
that writes the state: the `Receiver` tells it what arrived (the handshake answered, the peer's FIN or RST, our
FIN acknowledged), the `Transmitter` tells it a timer gave up, and it moves the connection on. The
`Transmitter` owns the segment writer, the send pool and the ACK, retry and deferred lists: every segment that
leaves, the retransmission queue's entries, the resends with the busy rule and the copy, the pure ACKs, probes
and resets, `writable`, `prepare`, `commit`, and the timers, both their arming and what an expiry resends; an
expiry that gives the connection up is returned as a verdict for the `Lifecycle` to carry out. The dependency
runs one way, read off the includes: the `Receiver` calls the other three, the `Lifecycle` and the
`AckProcessor` call the `Transmitter` and not each other, and the `Transmitter` calls none, as Linux's
`tcp_input.c` and `tcp_timer.c` call into `tcp_output.c` and
not back; the triggers from the receive side (an immediate ACK, a fast retransmit, the SYN-ACK) are direct
calls, and state defers work only where it must wait anyway: the ordinary ACK until `flush`, the retry hint
until the next `process`, a resend whose packet is lent until the next `flush`. The block is plain
single-thread memory: a connection's receive and transmit run on one shard thread, and nothing in it is
shared. Beneath the four: per shard `ConnectionTable`, `SegmentWriter`, `IsnGenerator` and one `NodePool`
each for held packets and retransmission-queue entries; per connection `ReceiveQueue`, `ReceiveWindow`, `AckPolicy`,
`SendSequence`, `RetransmissionQueue`, `LossRecovery` (over the policy's state), `RttEstimator`,
`ZeroWindowProbe` and `Timestamps`. The folders: `connection/` the handle and its read view, with the control
block and the connection's parts in `connection/detail/`; `stack/` the façade and the forward header, with the
four procedure classes, the context, the connection table, the ISN generator and the segment writer in
`stack/detail/`; `pool/detail/` the node pools; `config/` the configuration and counters; `congestion/` the
policies; `table/` the flow table. A header whose names are all in `tcp::detail` sits in the `detail/` folder
of the folder it belongs to and is included as `<detail/name.hpp>`.

**Zero-copy both ways.** A received segment's packet is held as it arrived and `unread()` points into it;
`consume` advances through it and returns each packet to the pool once it is empty. A sent segment is written
by the application into a fresh packet from IP; `commit` writes the frame's headers into its headroom, keeps
the packet in the connection's retransmission queue and lends it to the device, and a resend sends the same
packet again. The data path holds no stream buffer and no copy the application did not write itself; the one
copy the brick makes is the busy rule's fallback, below, and it is counted.

**Every segment that occupies sequence space is kept.** SYN, SYN-ACK, data and FIN each take an entry in the
connection's retransmission queue, a FIFO on nodes of the shard's `send_pool`: the packet with the whole frame
as it last left, the first sequence number and the length, the flags, the stamps of the first and the last
transmission, the transmission count, and the delivered count and the application-limited mark a rate
estimator reads later. The life of a data segment:

```
prepare(n)          a fresh packet; the application writes the payload into it
commit(n)           the headers go in front of the payload; the packet joins the queue; IP lends it to the
                    device; the retransmission timer is armed if it was not
device done         lent() turns false at the device's pace; the stack never waits for it
ACK covers it       the entry is freed; the packet goes back to the pool, now or when the device returns it
ACK covers a front  acknowledged() moves; the entry stays whole, trimmed only if it is resent
RTO, or 3 dupacks   the head entry is resent from its packet once the device has let go (below)
retries run out     TimedOut; or earlier, at the user timeout's deadline
release / abort     every entry is dropped; a lent packet is freed when the device returns it
```

A cumulative ACK frees every entry it covers from the head and stops at the first it does not, as Linux's
`tcp_clean_rtx_queue` does; an entry covered in part is trimmed when it is resent, as `__tcp_retransmit_skb`
trims, so the common case, a partial ACK followed by the full one, never touches the packet. `delivered()` counts
the payload bytes each ACK covers. The MSS is fixed at the handshake, so no kept segment is ever larger than what
may be sent; path MTU discovery, and the split a resend would then need, are step 6's. Pure ACKs, window updates
and zero-window probes occupy no sequence space: they leave in fresh packets through the same path and are
forgotten. A commit the queue or the device refuses takes no entry: the bytes are not accepted and a retry hint
follows. A connection keeps at most `send_segments` data entries, the send cap: `writable()` is zero while it is
at the cap, and the ACK that frees an entry raises `Writable`. The FIN is the stack's own and may take one entry
beyond the cap. After a terminal event, `Closed`, `Reset`, `TimedOut` or `Unresolved`, nothing is resent: the
queue is dropped at once, each packet back now or when the device returns it, exactly once either way.

**One frame template per connection.** The connection keeps the link and network headers `Ipv4::prebuild` built
at its first send: the Ethernet header with the next hop's MAC, the IPv4 header with the per-datagram fields
zero, the stamp they stop being valid at, and the IP brick's generation when they were built. A send, first or
repeated, copies them in front of the segment, writes the TCP header and its options, and hands the frame to
`Ipv4::send_frame`, which writes the total length, a fresh identification and the checksums or requests their
offload, lends the packet and transmits the loan; the packet stays the connection's. There is one send function
for every segment of a connection, the handshake's included, so a resend does to the headers exactly what the
first send did, and a checksum a device completed inside a lent buffer is computed again, never trusted. The TCP
header is written whole each time rather than patched, which costs the same and keeps one path. The headers are
rebuilt when the stamp reaches their validity, the ARP entry's confirmation plus `arp_reachable` or, while the
entry is stale, one `arp_request_interval` from the rebuild, or when the brick's generation moved: one compare
each per send. The rebuild runs the lookup `Ipv4::send` runs, so a refresh request goes when one is due, and a
rebuild that finds the next hop unresolved fails the send, counted as `send_unresolved`. Only RSTs leave through
the plain `Ipv4::send`: the reply to a segment with no connection has no template, and `abort`'s RST is not kept.

**A resend waits for its packet, then copies.** A lent packet is read, never written. A resend of the head takes
the first step that applies. Back from the device: the acknowledged front is trimmed, by moving the headers
forward over it, and the entry goes out as above. Still lent: the device is asked to reclaim
(`ShardQueue::reclaim()`), and when that frees nothing the busy rule applies, as Linux skips a resend whose
previous copy is still in a host queue (`skb_still_in_host_queue`): nothing goes now, the connection waits on a
deferred list, `retransmits_deferred` counts one, and the next `flush` tries again. After `retransmit_defers`
deferrals, one by default, the stack copies the unacknowledged part of the segment, read from the lent packet,
into a fresh packet, sends the copy as a second lent packet and counts `retransmits_copied`. The entry then holds
two packets until one is back, keeps whichever is back first and drops the other. With the default a driver
without reclaim costs a resend one tick; a deployment that would rather copy at once sets the bound to zero. The
timer backs off as if a deferred resend had left. None of the drivers CI runs on holds a transmitted mbuf once
its frame has gone (see the [device layer](device.md)), so on CI the deferral and the copy are reached only on a
fabric port with `Held` loans, in unit tests.

**The retransmission timer.** RFC 6298's estimator with Linux's floor. Before the first sample the RTO is
`rto_initial`, one second. The first sample sets `srtt = R` and `rttvar = R / 2`, each later one
`rttvar = 3/4 rttvar + 1/4 |srtt - R|` then `srtt = 7/8 srtt + 1/8 R`, and `RTO = srtt + max(4 rttvar, rto_min)`,
capped at `rto_max`: Linux's floor in place of the RFC's one second, without the per-round-trip maximum of the
deviation Linux also keeps, so after a sample Aloe's RTO can differ slightly from Linux's. The first sample comes
from the handshake. Karn's rule is applied as Linux's `tcp_clean_rtx_queue` applies it: an ACK gives a sample
from the oldest entry it freed, measured from that entry's last transmission, only when no entry it freed was
sent more than once; otherwise, on a connection with timestamps, the sample comes from the echo the ACK carries,
which names the transmission it answers (`tcp_ack_update_rtt`). A valid sample ends any backoff. The connection
has one wheel timer. It is armed at `now + RTO` when a segment goes into an empty queue, re-armed from now by
every ACK that advances `acknowledged()` while the queue holds more, and cancelled when the queue empties. Each
expiry resends the head, counts one retry, doubles the RTO up to `rto_max` and re-arms; the retry count resets on
every ACK that advances. The SYN and SYN-ACK are on the same timer and keep phase 1's schedule, which was an
initial RTO of one second doubling: with the defaults they go again 1, 3, 7, 15 and 31 seconds after the first
send and the connection times out at 63. Data and the FIN back off from the measured RTO, from 200 ms after a
handshake on a short path. Two of the RFCs' rules for a lost SYN or SYN-ACK are departed from, where Linux keeps
both in its own form. The connection starts with the full initial window, where RFC 5681 §3.1 asks for one segment
after a lost SYN or SYN-ACK; Linux cuts it to one after more than one handshake retransmission
(`tcp_init_transfer`). And when the handshake gave no round-trip sample, which after a resend only a connection
without timestamps lacks, the RTO stays where the handshake's backoff left it, where RFC 6298 §5.7 asks for 3
seconds, as Linux's `TCP_TIMEOUT_FALLBACK` in `tcp_init_metrics` gives.

**Recovery is bounded.** `retries` caps the timer-driven resends of the SYN and SYN-ACK, `data_retries` those of
data and the FIN, 15 by default as Linux's `tcp_retries2`. Running out ends an owned connection with `TimedOut`
and frees a passive open nobody owns, silently. With the defaults a peer that falls silent after the handshake is
given up after about fifteen minutes, the backoff from 200 ms reaching `rto_max`. The user timeout (RFC 5482,
Linux's `TCP_USER_TIMEOUT`), off by default, is the bound an application sets instead: when the timer fires and
the oldest unacknowledged segment was first sent at least `user_timeout` ago, the connection ends with `TimedOut`
whatever the retry count, and the timer is never armed later than that first transmission plus the timeout, so
the connection ends at the configured instant rather than at the next backoff (`tcp_clamp_rto_to_user_timeout`).
Such an end counts `connections_timed_out` and `connections_user_timed_out`. The user timeout bounds the
synchronized states and zero-window probing, not the handshake, which `retries` bounds; the runtime's
`connect(peer, deadline)` gives a caller a shorter bound there. Linux's `TCP_USER_TIMEOUT` differs in three ways
(`tcp_write_timeout`): it counts from the first retransmission (`retrans_stamp`), not the first transmission, so it
ends a connection about one RTO later and only after a resend; with the timeout set it ignores `tcp_retries2`,
where Aloe ends at whichever of `data_retries` and the timeout comes first; and since Linux 5.4 it bounds the
handshake too, once the retry count has not ended it.

**A next hop that never answers.** A SYN, SYN-ACK or FIN that finds no next hop on its first send, with nothing
else queued, keeps its entry with the packet unsent, and the timer polls every `unresolved_retry` (10 ms by
default) instead; those polls are not tries, and the RTO does not back off over them. The first send is the
open's or the close's as a rule, but it can also be the RTO's, when the device refused the segment before and
the next hop has gone by the expiry; the poll takes over the same way, and arms the RTO once the send leaves.
After `unresolved_polls` of them, 3 seconds by default, the
connection gives up: a passive open nobody owns is freed silently, an owned connection raises `Unresolved`, a
failure of its own, as Linux reports a host unreachable rather than a timeout when ARP goes unanswered. A data
segment that finds no next hop is not kept: its commit is refused with a retry hint. `abort` in SynSent sends
no RST, as RFC 9293's ABORT specifies: the peer holds nothing of ours to take back.

**Timestamps and PAWS.** RFC 7323 timestamps are on by default and negotiated per connection: offered in the SYN,
answered in the SYN-ACK only when the peer's SYN offered them, and on when both carried the option. The SYN and
SYN-ACK carry `mss, nop, nop, TS`; every later segment of a connection that negotiated them carries
`nop, nop, TS`, 12 bytes, the alignment Linux uses. The value is the milliseconds of the stack's stamp plus a
per-connection offset drawn at the open, as Linux offsets its clock per connection. The echo is `TS.Recent`, kept
by RFC 7323's rule in full: a segment that passes the sequence check replaces it when its value is not older and
it starts at or before `Last.ACK.sent`, the acknowledgement the connection last sent, which the connection tracks
as Linux tracks `rcv_wup`. PAWS runs before the sequence check, as in Linux's `tcp_validate_incoming`: a segment
other than a RST whose value is older than `TS.Recent`, in the option's signed 32-bit arithmetic, is dropped,
acknowledged at once and counted as `dropped_paws`, unless `TS.Recent` is older than 24 days. A segment without
the option on a connection that negotiated it is accepted, as Linux accepts it, where RFC 7323 would have it
dropped. `mss()` is the payload a segment carries, the negotiated MSS less the option, 1448 at an MSS of 1460,
while the SYN's MSS option keeps the full value as RFC 6691 prescribes: with timestamps a 1460-byte payload no
longer fits a 1500-byte MTU.

**Zero-window probes, with or without data.** When an acceptable ACK leaves the peer's window at zero with
nothing in flight, in `Established` or `CloseWait`, the connection probes: the first probe after one RTO, then at
doubling intervals up to `rto_max`. A probe is Linux's `tcp_xmit_probe_skb`: a pure ACK at `acknowledged() - 1`,
with the window and the timestamps every segment carries, which takes no entry; the peer, finding it outside its
window, answers with its current window. An ACK that opens the window ends probing and raises `Writable`. Every
acceptable ACK clears the count of unanswered probes and the stamp of the first one; `data_retries` unanswered
probes, or the user timeout since the first unanswered one, end the connection with `TimedOut`. A peer that
answers every probe keeps the connection open however long its window stays closed, as RFC 9293 requires; with a
user timeout set, the deadline its clamp armed stays armed across answers, so an answering peer is still probed
at least once per timeout. Here Aloe departs from Linux knowingly: Linux probes only while it holds unsent data
behind the window (`tcp_probe_timer` returns without a send head), because its socket buffer tells it the
application wants to send. Aloe holds no unsent data, so it probes while the window is zero whether or not the
application has data, which RFC 9293 allows and which costs an idle connection one segment per doubled RTO, at
most one every `rto_max`. The probe shares the connection's one timer.

**A FIN waits for the window.** `close` gives its FIN a sequence number and an entry at once but transmits it
only when the peer's window covers it, Linux's `tcp_snd_wnd_test`: a Linux peer drops a FIN that arrives inside
its zero window and answers with a duplicate ACK, after which only the RTO would resend it. A FIN the window does
not cover, at a zero window or behind data that fills it, waits in the queue unsent; with nothing in flight the
probes go on, in `FinWait1` or `LastAck` too, and the ACK that opens the window, or frees the data in front,
sends the FIN at once and starts its RTO. The congestion window does not gate a FIN, as it does not in Linux. The
peer's ACKs meanwhile are no duplicates, since nothing is in flight to be a duplicate of.

**Loss recovery.** RFC 5681 and RFC 6582 as written, with RFC 3042's limited transmit, all three of which Linux
runs. A duplicate ACK is an ACK with no payload, no SYN and no FIN, whose acknowledgement equals `acknowledged()`
and whose window equals the current one, arriving while the queue is not empty. The first and the second each let
one more segment out beyond the congestion window, within the peer's window. The third starts fast recovery when
its acknowledgement "covers more than recover", in RFC 6582's words, with `recover` starting at the initial
sequence number: the guard against a second fast retransmit for data a timeout already resent. Outside recovery
and the loss episode below, every ACK of new data slides `recover` to one before the new `acknowledged()`: the
guard compares in serial arithmetic, which would read a `recover` left more than 2^31 behind as ahead and refuse
every fast retransmit after 2 GiB without loss. Fast recovery records `recover`, the last sequence number sent,
tells the policy of the loss with the flight less what limited transmit sent beyond the windows, resends the head
at once and counts `fast_retransmits` and `recoveries`. Further duplicates are reported to the policy, which NewReno
answers by inflating its window. An ACK past `recover` is the full ACK and ends recovery; one short of it is a partial
ACK: the new head goes again at once, counted in `fast_retransmits`, and the policy hears how much was acknowledged.
Every ACK that advances re-arms
the timer, RFC 6582's Slow-but-Steady variant, as Linux re-arms it; and a fast retransmit or a partial-ACK resend
that leaves re-arms it too, as Linux's `tcp_xmit_retransmit_queue` does on resending the head, so the timer the
first send armed does not fire before the resend's ACK can return. The first timeout since an ACK of new data
leaves fast recovery, records `recover`, tells the policy, and starts a loss episode: every entry sent before the
timeout is treated as lost, and each ACK of new data short of `recover` resends the next head at once, counted in
`retransmits`. That is close to Linux's loss state without RACK, which step 5 brings, but slower: the loss episode
resends one head per ACK of new data, where Linux's loss state resends as many marked segments as `cwnd` allows.
A send into a queue that has been empty for longer than an RTO tells the policy of the idle period first, RFC 5681's
restart after idle, which Linux enables by default. A fast retransmit resends the head only, as Linux's does.

**A silent peer holds what the caps allow.** A peer that never acknowledges keeps a connection's entries, and
their packets, until `data_retries` or the user timeout ends the connection. `send_segments` bounds what one
connection holds and `send_pool` what the shard's connections hold together. When the pool is empty a `connect`
fails `TableFull`, a SYN to a listener is dropped as `dropped_no_node`, and a commit is refused with a retry hint
and counted in `allocation_failures`; a `close` is never short of a node, since Established set one aside for its
FIN, as Linux's FIN takes memory its accounting cannot refuse. So `send_pool / (send_segments + 1)` stalled
connections, 62 with the defaults, can hold the whole of a shard's pool, and with the user timeout off each holds
its share for about fifteen minutes, the
`data_retries` backoff, while the shard's other connections can send no data and open none. A program that talks
to peers it does not trust sets `user_timeout`, which bounds that hold to the timeout, or sizes `send_pool` for the
connections it expects to stall.

**Two limits on what is held, independent of each other.** The receive window is a byte window: the stack offers
`rcv_nxt + (B - unread)`, where `B` is the connection's budget, half of what `receive_segments` segments of its `mss()`
hold, or `receive_window` when set, at most 65535; it moves the edge it offered only by a full segment or half the
budget, whichever is smaller, and never retracts it. A read sends a window update of its own only when the window it
frees at least doubles the one on offer, and an ACK for a refused segment restates the edge already offered. Separately,
a connection holds at most `receive_segments` packets, and the shard's held packets come from one pool of `receive_pool`
nodes. Small segments can reach the packet cap before the byte window closes. Both chains, in order and above a gap,
count against the cap. A segment that arrives when either cap is reached takes the node of the highest segment held
above a gap, when that one starts above it: the lower sequence wins. Otherwise it is dropped, counted
(`dropped_no_slot` or `dropped_no_node`, or `dropped_out_of_order` above a gap) and not acknowledged: the immediate ACK
reports the last byte accepted, the peer's retransmission recovers it, and the offered window is never taken back. The
edge is extended only while a packet slot is free, so a connection at its cap stops offering more.

**The peer's window comes from current acknowledgements.** `writable()` is
`min(mss, min(snd_wnd, cwnd + allowance) - in flight)`, where `cwnd` is the policy's window and the allowance is
limited transmit's, and zero at the send cap. The peer's window is updated by RFC 793's rule, a newer sequence or
the same one with a newer acknowledgement, and only from a segment whose acknowledgement is at or past `snd_una`,
so a stale or forged acknowledgement cannot rewrite it. `commit` checks the credit again, because the peer may
have retracted its right edge, or a loss shrunk the congestion window, since the prepare; RFC 1122 asks the
sender to cope with a retracted edge by not sending into it.

**ACKs wait, with exceptions.** An ordinary acknowledgement of in-order data is queued and sent by `flush`, unless a
data segment leaving first carries it, so a request answered in the same tick costs one segment. It goes at once for
every second full-sized segment, a full-sized segment being the connection's `mss()`, the smaller of the two MSS less
the timestamp option on a connection that carries it; for anything that cannot be accepted (a duplicate, a segment out
of window or older than `TS.Recent`, data after the peer's FIN); for a segment above a gap, with the edge restated:
every one on a connection without SACK, the first three since `rcv_nxt` last moved on one with it, which leaves the
rest to one ACK at the `flush`; for in-order data while anything is held above it, and while recovering from a gap;
and for a FIN the connection is closing on. A refused immediate ACK stays queued for the next `flush`. Every pure ACK
of a connection that uses SACK carries the blocks it holds above a gap and a pending duplicate SACK; a data segment
carries none and leaves such an ACK queued.

**Ownership is logical state.** A passive open belongs to the stack until the handshake completes and `Accepted`
hands it to the application; one that fails before then is freed silently. A connection from `connect` is the
caller's at once. From `prepare` to `commit` (with any count, zero discards) or `release`, the prepared packet is
the application's: a `close`, `abort`, received RST or time-out in between leaves the span writable, and the
commit that follows drops the packet, returns false, counts `commits_refused` and raises no retry hint, since the
events already say why. A commit the windows no longer cover is refused the same way but with a retry hint:
prepare again for what `writable()` allows. After `Closed`, `Reset`, `TimedOut` or `Unresolved` the
retransmission queue is gone, the segments held above a gap go back to the pool, and the in-order held segments stay
readable until `release`, the owner's last call, which aborts a connection that is still open and frees the slot.

**The table hashes on its own; placement chooses ports.** The flow table mixes a base hash into its own
bucket hash. When the card hashes IPv4 TCP tuples the base hash is the card's RSS hash, so a received segment
needs no software hash; otherwise it is a software hash of the key. A connection must live on the shard whose
queue receives its segments. Inbound, the card has already chosen. Outbound, `connect` walks the ephemeral
range and takes the first free port whose replies the card's hash steers back to this queue. When the card
hashes addresses only, or steering is off, the port cannot change the queue: `connect` succeeds on the queue
the addresses select (queue 0 with steering off) and returns `Unplaceable` on every other, rather than open a
connection whose replies go elsewhere. With steering off that is every queue but 0, even with a free port:
every reply lands on queue 0.

**The terminal ACK goes before `Closed`.** When the last segment of a close needs an ACK from us, the ACK is
queued before `Closed` is raised. If IP refuses it, the connection keeps its state, `flush` retries the ACK,
and `Closed` follows the first ACK that leaves, whether `flush` sent it or a duplicate prompted it. A duplicate
FIN is acknowledged again without advancing the receive sequence twice; data after the peer's FIN is dropped,
counted as `dropped_unexpected` and acknowledged.

**Hot paths are bounded.** Every operation after construction is `noexcept` and allocates nothing but packets
from IP's pool and nodes from the stack's two fixed pools. Events go through one function; the event, ACK, retry
and deferred-resend lists are intrusive in the connection, so raising, queueing and draining are constant time.

**Compared with others.** Linux copies between socket buffers and user memory at both ends, and runs the stack in
the kernel behind system calls; its retransmit queue keeps each sent buffer and transmits a clone of it.
Seastar's native stack is also shard per core over DPDK, but owns the loop and exposes connections through
futures. TLDK, fd.io's DPDK TCP library, is the closest in shape: a library the application drives from its own
loop. Aloe keeps the received packets as the read buffer, lets the application write into the packet that leaves,
keeps that packet itself, lent, as the retransmission queue's entry, and leaves the loop to the program or to the
runtime written over the same bricks.

**What is still left out, and why.** The receive side holds segments above a gap as the packets they arrived in,
negotiates window scaling and reports what it holds with SACK blocks, but has no delayed ACK yet; it comes with the
rest of step 4 of phase 2. There is no TIME_WAIT: the slot is the owner's until `release`; it comes with step 6. The brick makes
no deployment or latency claim yet: the [tick-to-trade benchmark](../benchmarks/tick-to-trade.md) has a recorded
baseline, and its rerun with reliable send is owed, as its page says.

**Hardening is deferred too.** The brick is not yet meant for hostile peers. It has no SYN cookies, so a SYN
flood fills the fixed table until the half-open connections time out; its initial sequence numbers come from a
per-shard random engine, not RFC 6528's keyed hash; it accepts a RST anywhere in the window, without RFC 5961's
challenge ACKs; the flow table's software hash is unkeyed, so chosen 4-tuples can lengthen a probe, bounded by
the fixed table; and on a multi-queue device the IP brick accepts unsolicited ARP replies to our address, with no
rate limit. The initial sequence numbers, the challenge ACKs and the ARP rate limit come with step 6 of phase 2,
beside TIME_WAIT; SYN cookies and the keyed hash defend a server and come after phase 2, since the first
deployment is a client.
