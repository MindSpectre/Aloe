# TCP Module

The tcp module (`common/tcp/`) is the TCP brick: one `tcp::Stack` per shard, over that shard's
[`net`](net.md) IPv4 brick and its [loop](loop.md)'s timer wheel. It opens connections in both directions,
carries data both ways without copying it, holds what arrives above a gap as the packets it arrived in and
reports it with SACK blocks, scales its window, closes with FIN and RST, keeps every segment it sends, as the
packet it left in, until the peer acknowledges it, and recovers from a loss by what the peer's SACK blocks report
and by time, RACK-TLP: phase 1 of the [roadmap](../roadmap.md), and steps 3 to 5 of phase 2, reliable send, the
receive side and loss recovery with SACK. The first part of step 6 hardens it against a peer off the path: initial
sequence numbers no observer can predict, and RFC 5961's checks, under which a forged RST, SYN or old ACK moves counters
and nothing else. The brick has four verbs: `process` the segments IP sorted out,
`poll_event` to drain notifications, the connections' own non-blocking operations, and `flush` for the resends and
ACKs that waited. Every connection is a [stream](stream.md): the contract TLS and WebSocket
reuse later. Everything is reached through the umbrella `#include <aloe/tcp>` (`export/aloe/tcp`), and targets
link `Aloe::Common::Tcp`. It depends on [`net`](net.md), [`stream`](stream.md), [`loop`](loop.md),
[`device`](device.md), [`wire`](wire.md) and [`core`](core.md), and not on [`execution`](execution.md),
[`log`](log.md) or DPDK: nothing here names the asynchronous model, logs, reads a clock or blocks.

## Key types

- **`aloe::tcp::Stack<Ip, CongestionControl = NewReno>`** -- the brick, templated on the IP brick below it
  (`net::Ipv4<Device>`) and on a congestion control (see [below](#congestion-control)). Constructed from the IP brick, a
  `loop::TimerWheel&` and a `TcpConfig`, which it validates (throwing `std::invalid_argument`); it refuses the same way
  an IP brick that can resolve no neighbour, with learning off and an empty static table, whose message says to name
  the neighbours in `Ipv4Config::static_arp` or switch `arp_learning` on. Every slot, node and list is allocated there
  and never again, and the key of its initial sequence numbers is drawn there from the kernel (throwing
  `std::system_error` when the kernel gives no random bytes). `process(segments, now)` takes
  `ip.received(Ipv4Protocol::Tcp)` and leaves every packet moved out or released; `poll_event()` removes one pending
  notification; `flush(now)` retries the resends that waited for their packet, a tail probe's among them, and sends the
  ordinary ACKs that waited. `listen(port)` and `unlisten(port)` manage listeners; `connect(peer, now)` returns a
  `SynSent` connection the caller owns, or a `ConnectError`; the slot is the caller's even while its SYN waits for ARP.
  `counters()`, `pending_events()`, `table_size()`, `nodes_available()`, `send_nodes_available()`, `byte_budget()`, the
  largest receive budget the configuration allows, counted in the stack's own MSS without timestamps (373760 bytes with
  the defaults, where a timestamped connection's is 370688), and `sent_segment(connection, position)`, one entry of a
  connection's retransmission queue with its scoreboard marks, `reported` and `lost`, are for tests and monitoring;
  `force_next_isn(isn)` and `force_recover(connection, recover)` are for tests. The stack keeps the stamp of the last
  `process`, `flush` or `connect` and uses it for everything a connection does in between. It names its types
  `ConnectionType` (`Connection<Ip, CongestionControl>`), `EventType` (`ConnectionEvent<Ip, CongestionControl>`) and
  `Policy` (the congestion control).
- **`aloe::tcp::Connection<Ip, CongestionControl>`** -- one connection, a slot of its stack with a stable `index()` and
  address until `release`. It models `stream::IsStream`: `unread()` and `consume(n)` on the receive side; `writable()`,
  `prepare(n)`, `commit(n)` and the copying convenience `send(bytes)` on the transmit side; `close()`, `abort()` and
  `release()`. TCP adds `state()`, `local()`, `remote()`, `mss()`, `timestamps()`, `sack()`, `window_scaling()`,
  `committed()`, `acknowledged()` and `unacknowledged()`. For tests and monitoring it reports the receive side:
  `receive_budget()` (the budget the handshake settled), `receive_window()` (the bytes on offer from RCV.NXT to the edge
  already offered), `window_shift()` and `peer_window_shift()` (both scales, zero without scaling), and
  `out_of_order_segments()` and `out_of_order_bytes()` (what is held above a gap, outside `unread()`); and the send
  side: `queued_segments()` (the retransmission queue's length), `delivered()` (payload bytes the peer acknowledged,
  each once, when a SACK block first reported it or the cumulative ACK first covered it), `in_flight()` (what the
  congestion window is held against: `unacknowledged()` less what the peer reported received and less what is marked
  lost and not resent since), `reported_bytes()` and `lost_bytes()` (those two totals of the scoreboard), `srtt()` and
  `rto()` (the estimator, the backoff included), `retransmit_count()` (expiries of the retransmission timer since the
  last ACK of new data, a deferred or refused resend included), `congestion_window()` (the policy's window, and in
  recovery the rate reduction's on its way down to the policy's target) and `congestion_state()` (the policy's state),
  `recovery_state()` and `in_recovery()`, `duplicate_acks()` (since `acknowledged()` last moved; in recovery an ACK of
  new data does not clear the count, and without SACK it takes off the duplicates its advance accounts for),
  `reordering_window()` (RACK's, as the connection stands), `tail_probe_in_flight()` and `tail_probe_verdict_open()`
  (the tail loss probe's two facts), and `probing()` (zero-window probes running).
- **`aloe::tcp::ConnectionEvent<Ip, CongestionControl>`** -- what `poll_event` returns: the connection and the
  `stream::Events` taken from it. The flags are edges; `poll_event` and `release` are the only things that
  clear them.
- **`aloe::tcp::ReadView<Packet>`** -- `unread()`: a forward range of `std::span<const std::byte>`, one span
  per held packet's unconsumed payload, over the packets themselves. `size()` counts bytes. Invalidated by
  `consume` and by the next `process`.
- **`aloe::tcp::TcpConfig`** -- slots per shard (`connections`), `listeners`, the per-connection packet cap
  `receive_segments`, in order and above a gap together, which also sizes the default byte window, the byte
  window `receive_window` when one is set, the shard's shared `receive_pool` of held-packet nodes, the send cap
  `send_segments` and the shard's `send_pool` of retransmission-queue nodes, the ephemeral port range, the
  retransmission timer's `rto_initial`, `rto_min` and `rto_max`, the caps `retries` and `data_retries`, the
  `user_timeout`, the options `timestamps`, `sack` and `window_scaling`, the `delayed_ack`, the tail loss probe's
  `tail_probe_ack_delay`, `retransmit_defers`, the ARP wait's `unresolved_retry` and `unresolved_polls`, the
  limiter's `challenge_ack_interval` and the floor under the peer's MSS, `min_peer_mss`.
- **`aloe::tcp::TcpCounters`** -- monotonic, one per drop reason and one per send failure: what was sent by
  kind and resent by cause, challenge ACKs and the answers the limiter held back, recoveries, duplicate ACKs and
  round-trip samples, segments held above a gap and released for room, ACKs that carried SACK blocks or a duplicate
  SACK, sent and received, ACKs the delay timer sent, what RACK marked, reordering seen, reneging, tail probes and their
  verdicts, undos by their sign, connections opened, accepted, closed, reset and timed out, handshakes refused for the
  peer's MSS and SYNs whose payload was discarded, every reason a segment was dropped, and `send_refused`,
  `send_unresolved`, `commits_refused` and `allocation_failures`.
- **`aloe::tcp::RecoveryState`** -- where a connection's loss recovery stands: `Open`, nothing known lost;
  `Recovery`, from the first entry marked lost to the ACK that covers what was sent by then; `Loss`, from an expiry
  of the retransmission timer to the same.
- **`aloe::tcp::IsCongestionControl`**, **`AckInfo`**, **`NewReno`**, **`NoCongestionControl`** -- the policy
  concept, with the hooks `on_congestion`, `target` and `on_undo` for a recovery whose window the connection
  regulates, what an ACK of new data tells it, whether the window was in use among it, and its two models; see
  [below](#congestion-control).
- **`aloe::tcp::IsIp`** -- what the brick asks of the IP layer: `allocate`, `send`, `address`, `max_l4_size`,
  `next_hop`, `resolves_neighbours` and `queue`, and for the frame template `prebuild`, `send_frame` and `generation`
  (see the [net page](net.md)). `net::Ipv4<Device>` models it.
- **`aloe::tcp::FlowTable`** -- the fixed open-addressing index from a `FlowKey` (remote address, remote
  port, local port) to a slot: linear probing, backward-shift deletion, at most half full, no allocation
  after construction.
- **`aloe::tcp::State`** -- the RFC 793 states the brick has: `Closed`, `SynSent`, `SynReceived`,
  `Established`, `FinWait1`, `FinWait2`, `CloseWait`, `Closing`, `LastAck`. There is no `TimeWait`.
- **`aloe::tcp::ConnectError`** -- `TableFull` (no slot, or no retransmission-queue node for the SYN),
  `NoPort`, `Unplaceable`, `NoRoute` (no next hop: off the subnet with no gateway, or, with the IP brick's ARP learning
  off, a neighbour outside its static table). **`aloe::tcp::ListenError`** -- `InUse`, `TableFull`.
  **`aloe::tcp::Endpoint`** -- an address and a port.

## Usage

An echo server, a hand-written loop over one queue. One tick receives, lets the stack process, advances the
wheel, drains the events, then flushes the stack and the queue. This is the loop the two-brick test runs over
two fabric ports and `examples/tcp_echo` runs over a DPDK port:

```cpp
using Clock = aloe::core::Clock;
using Ip    = aloe::net::Ipv4<aloe::fabric::Port>;
using Tcp   = aloe::tcp::Stack<Ip>;

aloe::loop::ShardQueueCounters counters;
aloe::loop::ShardQueue<aloe::fabric::Port> queue{port, 0, 64, counters};
Ip ip{queue, {.address = {10, 0, 0, 10}, .prefix = 24, .arp_learning = true}};  // answer any host on the subnet
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

The IP brick resolves nothing on the wire unless its configuration says so. An echo server that answers whoever
connects from the subnet switches ARP learning on, as above; a client of venues it knows names its next hops in
`Ipv4Config::static_arp` instead and leaves learning off, the default, and then a SYN from a neighbour outside that
table is dropped as `dropped_no_route` and a `connect` to one returns `NoRoute` (see the [net page](net.md)). A brick
with neither, learning off and no static entry, can reach nobody, and the `Stack` constructor refuses it.

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
wheel. Every queue shares one address, so with learning on the ARP reply to a request one loop sent can arrive on
another queue: each loop sets `net::Ipv4Config::report_unsolicited_replies` and hands what `ip.resolved()` reports to
the other loops' `ip.apply_resolution`, as the [net page](net.md) describes. With learning off there is nothing to
forward.

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
tcp_echo <port> <address>/<prefix> [<gateway>] (--listen <port> | --connect <host>:<port>)
         [--static <address>=<mac>]... [--learn-arp] [-- <EAL arguments...>]

sudo ./build/debug/examples/tcp_echo/Aloe.Examples.TcpEcho net_tap0 10.78.0.2/24 --listen 7 --learn-arp \
    -- --vdev=net_tap0,iface=aloe0
ip addr add 10.78.0.1/24 dev aloe0 && ip link set aloe0 up && nc 10.78.0.2 7     # in another shell, as root

sudo ./build/debug/examples/tcp_echo_tasks/Aloe.Examples.TcpEchoTasks net_tap0 10.78.0.2/24 --connect 10.78.0.1:7007 \
    --learn-arp -- --vdev=net_tap0,iface=aloe0                # against a kernel echo server on port 7007
```

The brick knows its neighbours from the `--static` entries alone unless `--learn-arp` lets it resolve them on the
wire. A tap interface's MAC is new on every run, so the tap lines learn it; on a card whose router is known, a
gateway takes a `--static` entry for it, and without learning a gateway with no entry is refused at start. The
client sends one line, prints its echo and closes; the server echoes until interrupted. The kernel's
listener needs a port the host firewall opens: Fedora's firewalld opens only 1025-65535 on a new interface
and rejects the SYN with an ICMP message the stack does not read, so the client retries until it times out.

## Congestion control

The congestion control is a template parameter, the second of `tcp::Stack` and of `runtime::TcpStack`,
constrained by `IsCongestionControl`. A model is a stateless struct: a per-connection `State` the connection
embeds, default-constructible and trivially copyable, and static hooks the stack calls, so there is no virtual
call and no allocation, and a model whose hooks are empty leaves no code behind. The policy answers one
question, how much may be in flight, and is told what happened; the stack keeps every mechanic that is the
same under any policy, from the retransmission queue and its scoreboard to RACK, the tail loss probe, the
recovery states, the window in recovery, the undo, the `recover` guard, the retries and the user timeout (see the
notes from "Loss recovery" on, under [design notes](#design-notes)).

| Hook                                | Called                                                                                                                                         |
|-------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------|
| `reset(state, mss)`                 | When the connection reaches `Established`: the initial window.                                                                                 |
| `window(state)`                     | By `writable()` and the resend pass: the congestion window in bytes; in recovery the rate reduction's is used, unless the target is unbounded. |
| `on_ack(state, info)`               | On an ACK of new data, open or in the loss state; never in recovery. `info` says whether the window was in use.                                |
| `on_congestion(state, flight, mss)` | A loss was found, or a timeout is due a new reduction: the policy sets its target.                                                             |
| `target(state)`                     | By the rate reduction: the target, RFC 5681's `ssthresh`; the largest value when unbounded.                                                    |
| `on_recovery_end(state)`            | Recovery ended without an undo, or a tail probe repaired a loss: the window becomes the target.                                                |
| `on_timeout(state, in_flight, mss)` | At every expiry of the retransmission timer after the handshake: the window restarts at `in_flight` plus one segment.                          |
| `on_idle(state, mss)`               | On a commit into an empty queue more than an RTO after the last data left.                                                                     |
| `on_undo(state, prior)`             | A reduction was needless: `prior` is the state copied as the episode began.                                                                    |

The policy sets a target when a loss is found; the way from its window down to that target, while the loss is
repaired, is the connection's: Proportional Rate Reduction (RFC 9937), which hands the window back at the target with
`on_recovery_end`. A policy whose target is the largest 32-bit value has nothing to reduce: the rate reduction does not
run for it and its window stands through recovery, as Linux leaves the window alone when the threshold is infinite
(`tcp_end_cwnd_reduction`). That is `NoCongestionControl`, and it is the door a policy that owns its window in recovery,
BBR, will use. The connection copies the policy's `State` as an episode begins, which is why it must be trivially
copyable, and hands the copy back through `on_undo` when the episode's reduction turns out to have been needless.

`AckInfo` carries the bytes the ACK covers, the flight before it, the MSS, the round-trip sample when one was taken and
the stamp; whether the window was in use, `window_limited` and `largest_flight`, the connection's record of the round's
sends (see "The window grows only while it is used" under [design notes](#design-notes)); and the fields a delivery-rate
estimator reads: the last transmission of the newest entry the ACK freed, the connection's delivered bytes after the
ACK, which count the bytes a SACK block reported as well as those the cumulative ACK covered, and when that entry was
sent, and whether that send left window unused. No model reads the rate fields yet; they are there so a later model,
BBR's, reads them without a change to the concept or the queue.

- **`NewReno`**, the default: RFC 5681's slow start and congestion avoidance, while the window is in use, and RFC 6928's
  initial window, `min(10 mss, max(2 mss, 14600))`, Linux's `TCP_INIT_CWND`, with an unbounded initial target. Below the
  target an ACK grows the window by the bytes it covers, at most a segment; at or above it the acknowledged bytes are
  counted and each window of them adds a segment; neither happens on an ACK that finds the window was not in use. On
  congestion the target is `max(flight / 2, 2 mss)`, RFC 5681's, where Linux's reno halves the window, and the window is
  left to the rate reduction; the end of recovery sets the window to the target; a timeout restarts the window at what
  is in flight after the timeout's marking plus one segment, one segment when everything was marked; a send after an
  idle RTO restarts at `min(IW, cwnd)`, RFC 5681's restart window, and keeps the target, where Linux's
  `tcp_cwnd_restart` first raises it to at least three quarters of the window; an undo restores the larger of each pair,
  the window it has and the one it had, the target it has and the one it had, as Linux's `tcp_undo_cwnd_reduction` and
  `tcp_reno_undo_cwnd` do. Linux's reno runs the same rate reduction, counted in packets, dividing by the window it had
  where RFC 9937 divides by the flight recorded, and toward its own target, half the window, so its windows in recovery
  differ from Aloe's.
- **`NoCongestionControl`**: the window and the target are the largest 32-bit value and every hook is empty, so the
  peer's window is the only limit. Recovery still happens, RACK, the tail probe, the duplicate count, the RTO, the
  retries and the user timeout; what goes is the reaction to loss as a congestion signal: no rate reduction, an undo
  that restores nothing, and what is marked lost goes again as soon as it is marked. After a timeout the next ACK, of
  any kind, sends every entry the timeout marked at once. It is for private cross-connects, and it is as tested as
  `NewReno`: every scenario script that names no policy runs under both.

A program picks one by naming it; the default is `NewReno`:

```cpp
using Tcp      = aloe::tcp::Stack<Ip>;                                   // NewReno
using CrossTcp = aloe::tcp::Stack<Ip, aloe::tcp::NoCongestionControl>;  // the peer's window only
using Stack    = aloe::runtime::TcpStack<aloe::ethdev::Port, aloe::tcp::NoCongestionControl>;
```

## Counters

`TcpCounters`, one per stack, monotonic, read on the owning thread or after it has stopped. Every drop and every send
failure increments exactly one counter. Until step 6 adds an event for it, the counters are the signal that a connection
left the fast path: one that never does moves none of `retransmits`, `fast_retransmits`, `recoveries`,
`retransmits_deferred`, `retransmits_copied`, `tail_probes`, `reorder_timeouts`, `zero_window_probes` and
`dropped_paws`; one whose incoming segments each arrive once and in order moves none of `out_of_order_held`,
`out_of_order_released`, `sack_acks_sent` and `duplicate_sacks_sent`; and one whose own segments each arrive once and in
order moves none of `sack_acks_received`, `duplicate_sacks_received`, `rack_marked`, `reordering_seen` and the undos. A
tail probe can go with nothing lost, when the peer's ACK takes longer than the probe's wait, two round trips and, for a
lone segment, the allowance: `tail_probes_needless` counts those it learns of.

The counters changed meaning with loss recovery by SACK: `retransmits` now counts only the resends at an expiry of the
retransmission timer, one per expiry, where it also counted one per ACK of the loss episode after it; the resends of
entries marked lost, by RACK, by duplicate ACKs or by the timeout's marking, are all `fast_retransmits`, in recovery
and in the loss state; and `recoveries` counts the episodes entered by either detector.

| Group       | Counter                      | Counts                                                                                                                                                                                                      |
|-------------|------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Segments    | `segments_received`          | Segments handed to `process`.                                                                                                                                                                               |
|             | `data_segments_sent`         | Committed data segments queued.                                                                                                                                                                             |
|             | `pure_acks_sent`             | ACKs without data, from `flush`, the immediate-ACK exceptions, the challenges and the delay timer; probes are counted apart.                                                                                |
|             | `control_segments_sent`      | SYN, SYN-ACK and FIN transmissions: first sends, resends and copies alike.                                                                                                                                  |
|             | `resets_sent`                | RSTs: from `abort` and `release`, and the replies to a segment with no connection, to a bad ACK or a new SYN in `SynReceived` and to a handshake refused for the peer's MSS.                                |
|             | `challenge_acks_sent`        | Challenge ACKs the limiter let go (RFC 5961): sent at once, or left for the flush when the device refused them. Each is also one of `pure_acks_sent` once it leaves.                                        |
|             | `acks_rate_limited`          | Answers the limiter held back: challenge ACKs, ACKs for a refused segment that occupies no sequence space or carries a SYN, and the SYN-ACK again for the peer's SYN again in `SynReceived`.                |
|             | `window_updates`             | Segments that extended the advertised right edge; a restating ACK too, when rounding its field up moved it.                                                                                                 |
|             | `out_of_order_held`          | Segments held above a gap.                                                                                                                                                                                  |
|             | `out_of_order_released`      | Held out-of-order segments released to make room for a lower sequence.                                                                                                                                      |
|             | `sack_acks_sent`             | Pure ACKs that carried SACK blocks.                                                                                                                                                                         |
|             | `duplicate_sacks_sent`       | Of them, those whose first block reported data that arrived twice.                                                                                                                                          |
|             | `delayed_acks_sent`          | Of `pure_acks_sent`, the ACKs the delay timer sent.                                                                                                                                                         |
| Resends     | `retransmits`                | Resends at an expiry of the retransmission timer, of any kept segment, SYN, SYN-ACK, data or FIN: one per expiry, whether the resend left, waited or was refused.                                           |
|             | `fast_retransmits`           | Resends of entries marked lost, in recovery and in the loss state: one per entry per mark, at its first try, whether it left, waited or was refused. The expiry's own resend of the head is `retransmits`'. |
|             | `recoveries`                 | Episodes of recovery entered, by RACK on an ACK or at the reordering timer, or by duplicate ACKs, from the loss state too; a timeout's loss state is not one.                                               |
|             | `retransmits_deferred`       | Resends that found their packet still lent after a reclaim and waited for the next flush: one per deferral, and one per tick while a segment's packet and its copy are both lent.                           |
|             | `retransmits_copied`         | Resends sent as a copy, after `retransmit_defers` deferrals.                                                                                                                                                |
|             | `tail_probes`                | Tail loss probes sent (RFC 8985 7.3), each counted when its resend leaves: one the busy rule holds counts at the flush that sends it, and one that never leaves counts nothing.                             |
|             | `tail_probes_needless`       | Probes found needless: both copies arrived, as a duplicate report of the probe or a plain duplicate ACK showed.                                                                                             |
|             | `tail_probe_recoveries`      | Losses a probe repaired: the window cut to the target once, nothing resent, no episode. Counts `tail_probes_early` too.                                                                                     |
|             | `tail_probes_early`          | Of them, verdicts a second probe closed as a loss before an ACK said which: how often the departure from RFC 8985 7.3 and 7.4.2 was used.                                                                   |
|             | `zero_window_probes`         | Zero-window probes sent.                                                                                                                                                                                    |
|             | `duplicate_acks`             | Duplicate ACKs received, by RFC 5681's definition.                                                                                                                                                          |
|             | `rtt_samples`                | Round-trip samples taken, the handshake's included.                                                                                                                                                         |
| Loss        | `sack_acks_received`         | ACKs received whose SACK option was read: on a connection that uses SACK, from an ACK that passed the bound on old ACKs.                                                                                    |
|             | `duplicate_sacks_received`   | Of them, those whose first block was a believed duplicate report.                                                                                                                                           |
|             | `rack_marked`                | Segments RACK marked lost.                                                                                                                                                                                  |
|             | `lost_retransmits`           | Of them, resends marked lost again.                                                                                                                                                                         |
|             | `reorder_timeouts`           | Expiries of the reordering timer.                                                                                                                                                                           |
|             | `reordering_seen`            | ACKs that showed reordering.                                                                                                                                                                                |
|             | `sack_reneging`              | Expiries of the retransmission timer that found the head reported received: the peer reneged, and every received mark was cleared.                                                                          |
|             | `undos_duplicate_sack`       | Reductions taken back because every resend of the episode was reported as a duplicate.                                                                                                                      |
|             | `undos_original_ack`         | Reductions taken back because an ACK that moved `acknowledged()` answered an original: its echo is older than the episode's first resend, or nothing of the episode had been resent.                        |
| Connections | `connections_opened`         | Active opens that reached `Established`.                                                                                                                                                                    |
|             | `connections_accepted`       | Passive opens handed to the application by `Accepted`.                                                                                                                                                      |
|             | `connections_closed`         | Normal closes, both FINs acknowledged.                                                                                                                                                                      |
|             | `connections_reset`          | RSTs received on a connection the application owns; an active open refused for the peer's MSS is not one.                                                                                                   |
|             | `connections_timed_out`      | Owned connections whose retries or probes ran out, or whose user timeout passed.                                                                                                                            |
|             | `connections_user_timed_out` | Of `connections_timed_out`, those the user timeout ended.                                                                                                                                                   |
|             | `connections_unresolved`     | Owned connections whose next hop never answered ARP within `unresolved_polls` polls.                                                                                                                        |
|             | `handshakes_failed`          | Passive opens nobody owned yet that were reset, ended by a SYN at a new number, timed out or never resolved, freed silently.                                                                                |
|             | `handshakes_refused_mss`     | Handshakes refused because the peer's MSS, stated or assumed, was below `min_peer_mss`: a SYN to a listener that took no slot, or an active open whose SYN-ACK drew a RST.                                  |
|             | `syn_payload_discarded`      | SYNs a listener took a slot for that carried payload, neither held nor acknowledged: the peer sends it again. A SYN refused before it takes a slot counts under its refusal alone.                          |
| Drops       | `dropped_bad_header`         | Unparseable headers and malformed option lists.                                                                                                                                                             |
|             | `dropped_bad_checksum`       | A `Bad` verdict, or an `Unknown` one that failed the software check.                                                                                                                                        |
|             | `dropped_no_connection`      | No connection and no listener; answered with RST unless it was one or its source has no next hop.                                                                                                           |
|             | `dropped_closed`             | Segments for a `Closed` connection not yet released; answered with RST.                                                                                                                                     |
|             | `dropped_unexpected`         | Valid segments the state does not admit: a stray answer in `SynSent`, a bad ACK in `SynReceived`, an ACK of unsent data, no ACK flag, data after the peer's FIN or past one remembered.                     |
|             | `dropped_duplicate`          | Data entirely before `rcv_nxt`, or above a gap where held data covers every byte.                                                                                                                           |
|             | `dropped_out_of_order`       | Data above a gap with no room for it: at the cap or with the pool empty, and above every segment held out of order.                                                                                         |
|             | `dropped_out_of_window`      | Segments outside the window, and data at `rcv_nxt` with no room for it, whose ACK was read first.                                                                                                           |
|             | `dropped_no_slot`            | Data for a connection already holding `receive_segments` packets.                                                                                                                                           |
|             | `dropped_no_node`            | Data when the shard's `receive_pool` is empty, and SYNs to a listener when `send_pool` has no node for the SYN-ACK.                                                                                         |
|             | `dropped_table_full`         | SYNs to a listener with no free connection slot.                                                                                                                                                            |
|             | `dropped_no_route`           | SYNs to a listener whose source has no next hop: off the subnet with no gateway, or, with IP's learning off, outside its static table. No slot is taken and nothing is sent.                                |
|             | `dropped_paws`               | Segments whose timestamp is older than `TS.Recent`, never kept: answered at once, through the limiter when empty or a SYN; a SYN past `SynReceived` counts as challenged instead.                           |
|             | `dropped_reset_challenged`   | RSTs inside the window at none of the positions that reset (RFC 5961 3.2); each challenged.                                                                                                                 |
|             | `dropped_syn_challenged`     | SYNs on a connection past `SynReceived` (RFC 5961 4.2), wherever their sequence number lay and whether or not they passed PAWS; each challenged.                                                            |
|             | `dropped_old_ack`            | Segments whose ACK was below `snd_una - min(max_wnd, bytes_acked)` (RFC 5961 5.2), dropped whole, payload and FIN included, and challenged.                                                                 |
| Send        | `send_refused`               | Segments IP, the queue or a pool refused, a FIN that found no queue node included; the numbers stay as they were.                                                                                           |
|             | `send_unresolved`            | Segments that waited for ARP, a rebuild of the frame template that found no next hop included.                                                                                                              |
|             | `commits_refused`            | Commits after the connection left `Established` and `CloseWait`, or whose count the windows no longer cover; the prepared packet is dropped.                                                                |
|             | `allocation_failures`        | `prepare` calls the packet pool could not serve and commits that found no queue node, both with a retry hint, and copies the packet pool could not serve.                                                   |

The runtime's `TcpStack` adds two counters of its own, `forwards_dropped()` and `forwards_rejected()`, for
ARP forwards between shards (see the [runtime page](runtime.md)).

## Configuration

`TcpConfig` is validated by the `Stack` constructor, which throws `std::invalid_argument` for a config it
cannot run, and for an IP brick that can resolve no neighbour. Every shard gets the same config.

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
| `data_retries`                      | 15           | Timer-driven resends of data and the FIN, and unanswered zero-window probes and resends into a closed window, before `TimedOut`; Linux's `tcp_retries2`.             | zero, or 63 and more                                                                                                  |
| `user_timeout`                      | 0 (off)      | RFC 5482's bound: a synchronized connection whose oldest unacknowledged segment was first sent, or whose first unanswered probe went, this long ago ends `TimedOut`. | negative, or above a quarter of `core::Duration`'s range                                                              |
| `timestamps`                        | true         | Offer and use RFC 7323 timestamps.                                                                                                                                   | never                                                                                                                 |
| `sack`                              | true         | Offer and use SACK-permitted: report what is held and what arrived twice, read the peer's blocks for RACK and the tail probe; off, duplicate ACKs and the timer.     | never                                                                                                                 |
| `window_scaling`                    | true         | Offer and use RFC 7323 window scaling.                                                                                                                               | never                                                                                                                 |
| `delayed_ack`                       | 0 (off)      | How long an ACK for in-order data may wait, at most 500 ms; zero sends it at the flush of the same tick.                                                             | negative, above 500 ms, or set below the wheel's resolution                                                           |
| `tail_probe_ack_delay`              | 40 ms        | What a tail probe waits beyond two round trips when exactly one segment is in flight, for the peer's delayed ACK; Linux's 200 ms, below.                             | negative, or above 500 ms                                                                                             |
| `retransmit_defers`                 | 1            | Deferred resends of one segment before the copy; zero copies at once.                                                                                                | never                                                                                                                 |
| `unresolved_retry`                  | 10 ms        | The poll interval while a SYN, SYN-ACK or FIN waits for ARP; not a try.                                                                                              | not positive                                                                                                          |
| `unresolved_polls`                  | 300          | Polls before `Unresolved`: 3 s by default, the bound Linux's three ARP probes give.                                                                                  | zero                                                                                                                  |
| `challenge_ack_interval`            | 500 ms       | The least time between two answers through the limiter, per connection; zero sends every one. Linux's `tcp_invalid_ratelimit`.                                       | negative                                                                                                              |
| `min_peer_mss`                      | 48           | The least MSS a peer may have, its option's value or 536 without one: below it a handshake is refused, passive or active.                                            | below 48, or above the stack's MSS                                                                                    |

The constructor also refuses an MTU that leaves no room for a TCP header. The bounds on `rto_initial`, `rto_max` and
`user_timeout` keep every deadline within a quarter of `core::Duration`'s range, so no arithmetic on the timer path can
overflow, and `rto_min` below the wheel's resolution would ask for a timer finer than the wheel fires; `delayed_ack` has
the same floor. Size the device's packet pool for the receive descriptors, `receive_pool` held packets, `send_pool` kept
packets and the copies out beside them, the transmit ring and what the card holds, and the open preparations: neither
node pool is carved out of it. With the defaults a connection offers 370688 bytes over 512 slots, half of 512 segments
of 1448 bytes, to a peer that scales its window and sends timestamps (373760 without timestamps, 65535 to a peer that
does not scale). `receive_pool` at 2048 is what four connections stalled at the default cap hold between them, and the
cap is per connection, so one stalled connection cannot take another's nodes. A window the configuration derives beyond
what the largest shift, 14, can state is capped there.

`tail_probe_ack_delay` departs from Linux by default. RFC 8985 lets a sender wait longer before it probes a lone
segment, for the peer's delayed ACK, and leaves the amount open; Linux, Onload and VPP add their minimum RTO, 200 ms, so
a lone segment's probe fires at the moment the RTO would have, which saves the backoff and the window but not the time.
An order is usually a lone segment, so Aloe adds 40 ms, Linux's shortest delayed ACK, as FreeBSD's RACK stack does: a
lost lone order goes again two round trips and 40 ms after it left. The smoothed round trip is measured from the peer's
own ACKs, so a peer that always delays is already allowed for. A peer that delays its ACK longer draws a needless probe,
one duplicate segment, which its TCP drops by sequence number and reports, and which `tail_probes_needless` counts; its
application never sees the order twice. A deployment whose peer answers every segment at once sets zero. One whose peer
acknowledges only with its next message, as the tick-to-trade benchmark's venue does, sets up to 500 ms: that does not
turn the probe off, it puts a lone segment's probe at the RTO's moment, as Linux's 200 ms does, where it still saves the
backoff.

## Design notes

**One class per concern.** `tcp::Stack` is the brick's façade and owner: the four verbs, the accessors, and the mapping
from a connection's state to its public handle. What it does is four procedure classes in `tcp::detail`, each in its own
header in a `detail/` folder, over one `detail::ControlBlock` per connection, RFC 9293's transmission control block as a
struct with one part per concern, and one `detail::StackContext` per shard, what every part reads: the IP brick, the
config, the held-segment pool, the counters and the stamp. The `Receiver` is what a datagram does to a connection: the
checksum and lookup, the handshake, the first checks of a synchronized connection in Linux's order with their challenge
ACKs and the limiter's decision, the hold of a segment above a gap and the join when the gap fills, what gives way at
the packet cap, and `consume`; it owns nothing. The `AckProcessor` is what an acknowledgement does to the send side, in
the order of Linux's `tcp_ack` and `tcp_fastretrans_alert`: the peer's SACK blocks into the scoreboard, the entries the
cumulative part frees, the round-trip sample, RACK's state, the tail probe's verdict, the undo, detection, the recovery
state and the resends it allows, the peer's window and the zero-window probes, and the send timer; it owns nothing
either, and tells the `Receiver` what moved. The `Lifecycle` owns the connection table, the ISN generator and the event
list: slots in and out, every change of a connection's state from the open to `Closed`, `close`, `abort` and `release`.
It is the only part that writes the state: the `Receiver` tells it what arrived (the handshake answered, the peer's FIN
or RST, our FIN acknowledged), the `Transmitter` tells it a timer gave up, and it moves the connection on. The
`Transmitter` owns the segment writer, the send pool and the ACK, retry and deferred lists: every segment that leaves,
the retransmission queue's entries, the resend of any entry with the busy rule and the copy and the resend pass over
those marked lost, what is in flight and the window it is held to, the pure ACKs with the SACK option, probes and
resets, the scaled window every segment states and the edge it records, `writable`, `prepare`, `commit`, and both
timers, the choice and arming of the send timer's kind and what each expiry marks and sends, the tail probe among them;
an expiry that gives the connection up is returned as a verdict for the `Lifecycle` to carry out. The dependency runs
one way, read off the includes: the `Receiver` calls the other three, the `Lifecycle` and the `AckProcessor` call the
`Transmitter` and not each other, and the `Transmitter` calls none, as Linux's `tcp_input.c` and `tcp_timer.c` call into
`tcp_output.c` and not back; the triggers from the receive side (an immediate ACK, a resend, the SYN-ACK) are direct
calls, and state defers work only where it must wait anyway: the ordinary ACK until `flush` or its delay, the retry hint
until the next `process`, a resend whose packet is lent until the next `flush`. The block is plain single-thread memory:
a connection's receive and transmit run on one shard thread, and nothing in it is shared. Beneath the four: per shard
`ConnectionTable`, `SegmentWriter`, `IsnGenerator` and one `NodePool` each for held packets and retransmission-queue
entries; per connection `ReceiveQueue`, `ReceiveWindow`, `ReassemblyQueue`, `AckPolicy`, `SendSequence`,
`RetransmissionQueue`, `LossRecovery` (over the policy's state, holding `RateReduction`), `RttEstimator`, `Rack`,
`TailProbe`, `ZeroWindowProbe`, `Timestamps`, `WindowUse`, `SackBlocks` and `AnswerLimiter`, and two wheel timers, one
for the sends and one for the delayed ACK. On the receive side, `ReceiveQueue` keeps RCV.NXT, the in-order chain, the
unread count and whether the peer's FIN is in; `ReassemblyQueue` the chain above a gap and the number of a FIN that
arrived above it; `SackBlocks` whether SACK is in use, the four blocks in the order they are reported and the pending
duplicate; `ReceiveWindow` the budget, our scale and the edge already offered; and `AckPolicy` when the ACK of data
goes, in order and above a gap: at once, at the `flush` or after the delay, with the count of answers above a gap; and
`AnswerLimiter` the stamp of the last answer that went through the limiter. On the send side, `SendSequence` keeps
SND.UNA, SND.NXT, the peer's window with its scale, the largest window it offered, and the sum of SND.UNA's advances
since `Established`, which the bound on an old ACK reads; `RetransmissionQueue` the entries and the scoreboard on them,
the two marks, reported and lost, with their totals and what the ACKs delivered; `LossRecovery` the recovery state,
open, recovery or loss, `recover`, the duplicate count for a peer without SACK, the policy's state and the undo's
record; `RateReduction` the rate reduction's counts, what the ACKs of an episode delivered and what it sent, the flight
on entry and the window in recovery; `Rack` RACK's segment and round trip, the minimum round trip, whether reordering
was seen and the reordering window's multiplier and its persistence; `TailProbe` whether a probe is in flight and where
it ends, whether its verdict is open and whether a round-trip sample was taken since; and `WindowUse` whether the window
limited a send of the round and the most in flight in it. The parts an in-order segment and an in-order ACK touch sit
together at the front of the block, the window's use, the blocks, the limiter's stamp and the delayed ACK's timer behind
them. The folders: `connection/` the handle and its read view, with the control block and the connection's parts in
`connection/detail/`; `stack/` the façade and the forward header, with the four procedure classes, the context, the
connection table, the ISN generator and the segment writer in `stack/detail/`; `pool/detail/` the node pools; `config/`
the configuration and counters; `congestion/` the policies; `table/` the flow table. A header whose names are all in
`tcp::detail` sits in the `detail/` folder of the folder it belongs to and is included as `<detail/name.hpp>`.

**Zero-copy both ways.** A received segment's packet is held as it arrived and `unread()` points into it; `consume`
advances through it and returns each packet to the pool once it is empty. A segment above a gap is held the same way, on
a second chain, and joins the first by a change of links when the gap fills; an overlap is trimmed by moving an offset
and a length. A sent segment is written by the application into a fresh packet from IP; `commit` writes the frame's
headers into its headroom, keeps the packet in the connection's retransmission queue and lends it to the device, and a
resend sends the same packet again. The data path holds no stream buffer and no copy the application did not write
itself; the one copy the brick makes is the busy rule's fallback, below, and it is counted.

**Every segment that occupies sequence space is kept.** SYN, SYN-ACK, data and FIN each take an entry in the
connection's retransmission queue, a FIFO on nodes of the shard's `send_pool`: the packet with the whole frame
as it last left, the first sequence number and the length, the flags, the stamps of the first and the last
transmission, the transmission count, the scoreboard's two marks, reported received and lost, with where the resend
a lost mark owes stands, and the delivered count and the application-limited mark a rate estimator reads later. The
life of a data segment:

```
prepare(n)          a fresh packet; the application writes the payload into it
commit(n)           the headers go in front of the payload; the packet joins the queue; IP lends it to the
                    device; the send timer is armed: the tail probe from now when one may be scheduled
                    (SACK in use, recovery open, nothing reported), else the RTO if the queue was empty
device done         lent() turns false at the device's pace; the stack never waits for it
a block covers it   marked reported; the entry keeps its packet and its place in the queue
ACK covers it       the entry is freed; the packet goes back to the pool, now or when the device returns it
ACK covers a front  acknowledged() moves; the entry stays whole, trimmed only if it is resent
marked lost         by RACK, by the third duplicate ACK or a partial ACK without SACK, or by the RTO's expiry;
                    resent from its packet once the device has let go (below), lowest sequence first
the tail is silent  the last segment sent goes again as the tail probe, ahead of the RTO
retries run out     TimedOut; or earlier, at the user timeout's deadline
release / abort     every entry is dropped; a lent packet is freed when the device returns it
```

A cumulative ACK frees every entry it covers from the head and stops at the first it does not, as Linux's
`tcp_clean_rtx_queue` does; an entry covered in part is trimmed when it is resent, as `__tcp_retransmit_skb`
trims, so the common case, a partial ACK followed by the full one, never touches the packet. An entry a SACK block
reported stays, with its packet, until a cumulative ACK frees it: a receiver may discard what it reported (RFC 2018),
and Aloe's own does when it needs room for a lower sequence, so the packet may be needed again; and nothing is ever
unlinked from the middle of the queue. `delivered()` counts each payload byte once, when a block first reports it or
when a cumulative ACK first covers it. The MSS is fixed at the handshake, so no kept segment is ever larger than what
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

**A resend waits for its packet, then copies.** A lent packet is read, never written. A resend of any entry, the
head at the RTO's expiry, an entry marked lost, or the segment a tail probe sends again, takes the first step that
applies. Back from the device: when `acknowledged()` passed the entry's first byte, which only the head can need, the
acknowledged front is trimmed, by moving the headers forward over it, and the entry goes out as above. Still lent: the
device is asked to reclaim (`ShardQueue::reclaim()`), and when that frees nothing the busy rule applies, as Linux skips
a resend whose previous copy is still in a host queue (`skb_still_in_host_queue`): nothing goes now, the connection
waits on a deferred list while any of its entries waits, `retransmits_deferred` counts one, and the next `flush` tries
again. After `retransmit_defers` deferrals, one by default, the stack copies the unacknowledged part of the segment,
read from the lent packet, into a fresh packet, sends the copy as a second lent packet and counts
`retransmits_copied`. The entry then holds two packets until one is back, keeps whichever is back first and drops the
other. With the default a driver without reclaim costs a resend one tick; a deployment that would rather copy at once
sets the bound to zero. The timer backs off as if a deferred resend had left. None of the drivers CI runs on holds a
transmitted mbuf once its frame has gone (see the [device layer](device.md)), so on CI the deferral and the copy are
reached only on a fabric port with `Held` loans, in unit tests.

The resend pass, Linux's `tcp_xmit_retransmit_queue`, sends the entries marked lost, lowest sequence first, each through
that resend, and stops when the congestion window allows no more (RFC 8985 9.3 holds a resend to the window as it holds
new data), or at the first that cannot leave; with nothing marked it returns without a walk. It runs on every ACK but
one that shows reneging and a partial ACK the undo holds back, at the `flush` for a connection whose resend waits, and
at the reordering timer. A resend that leaves clears the entry's lost mark, so the entry counts as in flight again,
counts against the rate reduction in recovery, and counts among the episode's resends for the undo; a tail probe's
resend clears and counts none of these. When the entry that left is the head, the RTO is armed from now, as Linux
re-arms it on resending the head, so that the timer an earlier send armed does not fire before the resend's ACK can
return; a pending reordering timer stays, and only the RTO's moment moves.

**Resends go before new data.** While any entry is marked lost and not yet resent, `writable()` is zero. Such an entry
is not in flight, so its share of the window would otherwise be offered to new data, which would then hold the window
against the resend, at the flush when its packet comes back or at the next ACK; Linux too runs its resends before it
sends new data. A sender that writes right after a loss is found therefore sees nothing accepted until the marked
entries have left. When the last of them leaves outside an ACK, at the flush or at a timer, `Writable` is raised at the
next `process`, as for a refused commit's hint; when it leaves on an ACK, that ACK raises it.

**The retransmission timer.** RFC 6298's estimator with Linux's floor. Before the first sample the RTO is
`rto_initial`, one second. The first sample sets `srtt = R` and `rttvar = R / 2`, each later one
`rttvar = 3/4 rttvar + 1/4 |srtt - R|` then `srtt = 7/8 srtt + 1/8 R`, and `RTO = srtt + max(4 rttvar, rto_min)`,
capped at `rto_max`: Linux's floor in place of the RFC's one second, without the per-round-trip maximum of the
deviation Linux also keeps, so after a sample Aloe's RTO can differ slightly from Linux's. The first sample comes
from the handshake. Karn's rule is applied as Linux's `tcp_clean_rtx_queue` applies it: an ACK gives a sample
from the oldest entry it freed, measured from that entry's last transmission, only when no entry it freed was
sent more than once; otherwise, on a connection with timestamps, the sample comes from the echo the ACK carries,
which names the transmission it answers (`tcp_ack_update_rtt`); an echo of the tick the ACK arrives in counts one
tick, 1 ms, as Linux's `tcp_rtt_tsopt_us` counts it. A valid sample ends any backoff.

The connection has one wheel timer for its sends, beside the delayed ACK's, and a kind that says what it does when it
fires. RFC 8985 (8) expects exactly that: the reordering timer, the tail probe, the RTO and the zero-window probe are
never needed at once, "one timer with an additional state variable", and Linux keeps one pending kind for the same
four. So the five kinds are never armed together:

| Kind              | Armed when                                                                                                                                                                            | Deadline                                                                                                                                                     | When it fires                                                                                                                  |
|-------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------|
| Tail probe        | Data was sent, or an ACK moved `acknowledged()` and left data out, on a connection that uses SACK, open, with nothing reported received; not while a reordering timer waits           | Two smoothed round trips from now, `rto_initial` before a sample, plus `tail_probe_ack_delay` for exactly one segment in flight; never past the RTO's moment | The last segment sent goes again, when no probe is in flight and a sample was taken since the last; then the RTO from now      |
| Reordering        | RACK left entries waiting whose time has not passed, in any state                                                                                                                     | The longest wait left, never past the RTO's moment                                                                                                           | Detection again; what it marks begins recovery or goes on under the open one, and is resent; then the RTO at the moment it had |
| RTO               | Something is in flight and neither kind above applies: in the handshake, without SACK, after a probe, in recovery or the loss state while RACK has nothing waiting; short on reneging | RFC 6298 with Linux's floor, doubled per expiry, under the user timeout                                                                                      | The head is resent and what is out marked lost, the loss state entered, the RTO doubled and armed again                        |
| Zero-window probe | The peer's window is zero with nothing in flight                                                                                                                                      | One RTO, then doubling up to `rto_max`                                                                                                                       | A probe at `acknowledged() - 1`                                                                                                |
| ARP poll          | A SYN, SYN-ACK or FIN found no next hop on its first send                                                                                                                             | `unresolved_retry`                                                                                                                                           | The send again; `Unresolved` after `unresolved_polls`                                                                          |

The connection keeps the moment its RTO would fire, set wherever the RTO is armed from now: a send into an empty queue,
an ACK that moves `acknowledged()` while anything is still in flight, a resend of the head, a tail probe. The tail probe
and the reordering timer never pass that moment, and when either gives way, to an ACK or after its expiry, the RTO takes
the moment it had, not a fresh one; a probe due exactly then is still a probe. A data segment sent while others are
unacknowledged schedules the probe again from now, so a second order sent while the first is out costs an arm of the
wheel of its own; a lone order arms once and its ACK cancels once. An expiry of the RTO counts one retry, marks and
resends as the loss-recovery notes below say, doubles the RTO up to `rto_max` and re-arms; the retry count resets on
every ACK that advances. An expiry of the tail probe or of the reordering timer is no retry: neither moves the retry
count or the backoff, and neither counts against `data_retries`. The SYN and SYN-ACK are on the same timer and keep
phase 1's schedule, which was an initial RTO of one second doubling: with the defaults they go again 1, 3, 7, 15 and 31
seconds after the first send and the connection times out at 63. Data and the FIN back off from the measured RTO, from
200 ms after a handshake on a short path. Two of the RFCs' rules for a lost SYN or SYN-ACK are departed from, where
Linux keeps both in its own form. The connection starts with the full initial window, where RFC 5681 §3.1 asks for one
segment after a lost SYN or SYN-ACK; Linux cuts it to one after more than one handshake retransmission
(`tcp_init_transfer`). And when the handshake gave no round-trip sample, which after a resend only a connection without
timestamps lacks, the RTO stays where the handshake's backoff left it, where RFC 6298 §5.7 asks for 3 seconds, as
Linux's `TCP_TIMEOUT_FALLBACK` in `tcp_init_metrics` gives.

**Recovery is bounded.** `retries` caps the timer-driven resends of the SYN and SYN-ACK, `data_retries` those of
data and the FIN at an open window, 15 by default as Linux's `tcp_retries2`, and the expiries at a closed window that
go unanswered, as the note on zero-window probes says. Running out ends an owned connection with `TimedOut`
and frees a passive open nobody owns, silently. With the defaults a peer that falls silent after the handshake is
given up after about fifteen minutes, the backoff from 200 ms reaching `rto_max`; a tail probe before the first
expiry takes no retry and backs nothing off, so it adds only its own wait to that. The user timeout (RFC 5482,
Linux's `TCP_USER_TIMEOUT`), off by default, is the bound an application sets instead: when the timer fires and
the oldest unacknowledged segment was first sent at least `user_timeout` ago, the connection ends with `TimedOut`
whatever the retry count, and the timer is never armed later than that first transmission plus the timeout, so
the connection ends at the configured instant rather than at the next backoff (`tcp_clamp_rto_to_user_timeout`).
Such an end counts `connections_timed_out` and `connections_user_timed_out`. The tail probe and the reordering timer
never pass the RTO's moment, so the user timeout bounds them through it: an expiry of either at the user timeout's
moment ends the connection there and sends nothing. The user timeout bounds the synchronized states and zero-window
probing, not the handshake, which `retries` bounds; the runtime's `connect(peer, deadline)` gives a caller a shorter
bound there. Linux's `TCP_USER_TIMEOUT` differs in three ways (`tcp_write_timeout`): it counts from the first
retransmission (`retrans_stamp`), not the first transmission, so it ends a connection about one RTO later and only after
a resend; with the timeout set it ignores `tcp_retries2`, where Aloe ends at whichever of `data_retries` and the timeout
comes first; and since Linux 5.4 it bounds the handshake too, once the retry count has not ended it.

**A next hop that never answers.** A SYN, SYN-ACK or FIN that finds no next hop on its first send, with nothing
else queued, keeps its entry with the packet unsent, and the timer polls every `unresolved_retry` (10 ms by
default) instead; those polls are not tries, and the RTO does not back off over them. The first send is the
open's or the close's as a rule, but it can also be the RTO's, when the device refused the segment before and
the next hop has gone by the expiry; the poll takes over the same way, and arms the RTO once the send leaves.
After `unresolved_polls` of them, 3 seconds by default, the
connection gives up: a passive open nobody owns is freed silently, an owned connection raises `Unresolved`, a
failure of its own, as Linux reports a host unreachable rather than a timeout when ARP goes unanswered. A data
segment that finds no next hop is not kept: its commit is refused with a retry hint. `abort` in SynSent sends
no RST, as RFC 9293's ABORT specifies: the peer holds nothing of ours to take back. All of this needs the IP brick's ARP
learning on: with it off, the default, a next hop is in the static table or there is none, so `connect` returns
`NoRoute` at once for a neighbour outside the table, nothing polls, and a connection that opened never waits for ARP.

**A source with no next hop is not answered.** A SYN to a listener whose source has no next hop, one from off the
subnet with no gateway, or, with learning off, from a neighbour outside the static table, is dropped before it takes a
slot and counted in `dropped_no_route`: no SYN-ACK could reach it, and the slot would wait out the whole retry
schedule. The same question is asked before a reset is sent to a segment's source; with no next hop nothing is sent
and nothing more is counted, the segment having been counted under its own drop reason.

**Timestamps and PAWS.** RFC 7323 timestamps are on by default and negotiated per connection: offered in the SYN,
answered in the SYN-ACK only when the peer's SYN offered them, and on when both carried the option. Every later segment
of a connection that negotiated them carries `nop, nop, TS`, 12 bytes, the alignment Linux uses; the SYN and SYN-ACK
carry it beside the other options, in the layout the note on the handshake's options gives. The value is the
milliseconds of the stack's stamp plus an offset from the keyed hash of the connection's flow (see the next note), so
one flow keeps one offset, and its timestamps stay monotonic across its connections, for as long as the stack lives. The
echo is `TS.Recent`, kept by RFC 7323's rule: a segment replaces it when its value is not older and it starts at or
before `Last.ACK.sent`, the acknowledgement the connection last sent, which the connection tracks as Linux tracks
`rcv_wup`, and only once it has passed every check up to the bounds on its ACK, as Linux's `tcp_ack` replaces
`ts_recent` after its tests for an ACK too old and of unsent data: a forged segment inside the window that could set
`TS.Recent` would make PAWS refuse the real peer. PAWS runs before the sequence check, as in Linux's
`tcp_validate_incoming`: a segment other than a RST whose value is older than `TS.Recent`, in the option's signed 32-bit
arithmetic, is dropped, answered at once and counted as `dropped_paws`, unless `TS.Recent` is older than 24 days; the
answer goes through the limiter when the segment occupies no sequence space or carries a SYN, and a SYN that fails PAWS
past `SynReceived` is challenged and counted as a SYN (see the note on the first checks). A segment without the option
on a connection that negotiated it is accepted, as Linux accepts it, where RFC 7323 would have it dropped. `mss()` is
the payload a segment carries, the negotiated MSS less the option, 1448 at an MSS of 1460, while the SYN's MSS option
keeps the full value as RFC 6691 prescribes: with timestamps a 1460-byte payload no longer fits a 1500-byte MTU.

**Initial sequence numbers from a keyed hash.** A connection's initial sequence number and its timestamp offset come
from one SipHash-2-4 (`core::siphash24`) of its flow, the local address, the remote address, the local port and the
remote port, twelve bytes, under a 128-bit key the `Stack` draws from the kernel (`core::random_bytes`) at
construction: RFC 6528's function, and the hash Linux's `secure_tcp_seq_and_ts_off` computes. The sequence number is
the hash's low 32 bits plus the stamp's nanoseconds since the clock's epoch shifted right by six, Linux's unit of 64
ns, modulo 2^32; the offset is the high 32 bits. So one flow opened again later starts further on by the time that
passed, two flows that differ in any field start at unrelated numbers, and an observer off the path cannot predict
either number of a connection from any number of others it opens or watches. Nothing reads the key back: no accessor,
no counter and no log line, and a stack cannot be given one; `force_next_isn` replaces the next sequence number alone,
for tests. The clock term is the stack's stamp, since the brick reads no clock, and the cost is twelve bytes hashed
once per open. The key is per stack, so per shard: a flow lands on one shard while the card's steering stays as it is.

**The first checks of a segment, in Linux's order.** A segment on a synchronized connection meets its checks in the
order of Linux's `tcp_validate_incoming` followed by `tcp_ack`, which is RFC 5961's, and each may end it.

0. In `SynReceived` only, an ACK that does not acknowledge the SYN-ACK, on any segment but a RST, is answered with
   `<SEQ=SEG.ACK><CTL=RST>` (RFC 9293 3.5.2) and dropped as `dropped_unexpected`. The half-open connection stays as it
   was and its SYN-ACK's timer runs on, as Linux's `tcp_check_req` hands such a segment back to the listener before it
   checks the window.
1. PAWS, as the note above says.
2. The sequence check against RCV.NXT and the offered edge, with four outcomes. *Refused*: entirely before RCV.NXT,
   starting past the edge, or above a gap with payload that ends past the edge. A RST here is dropped in silence,
   except at RCV.NXT - 1 after the peer's FIN. A SYN past `SynReceived` is challenged; in `SynReceived` the peer's SYN
   again draws the SYN-ACK again and any other SYN an ACK, both through the limiter. Anything else is counted and
   answered with an ACK that restates the edge. *In order*, which also takes an empty segment exactly at the edge,
   where a peer that filled the window above a hole sends its ACKs and its RST from, and a bare FIN at RCV.NXT while
   the window is zero, which a stalled reader then learns of from `PeerClosed`. *Above a gap*. *No room*: the
   segment's first new byte is at RCV.NXT, after the old prefix is trimmed, and its payload ends past the edge, at a
   zero window any payload; it goes on like an in-order segment and its payload is refused at the payload's check
   below. A FIN's own number is never held against the edge, since it takes no slot, so a FIN may also stand one past
   it.
3. RST. A RST resets the connection at RCV.NXT; at RCV.NXT - 1 in `CloseWait`, `Closing` or `LastAck`, whatever the
   sequence check said, since macOS sends its RST with the number of the FIN before it; and, on a connection that uses
   SACK, at the end of the highest range held above a gap, a remembered FIN's number included, which is where a peer
   whose segments below that range were lost resets from, its SND.NXT. Any other RST that passed the sequence check is
   counted in `dropped_reset_challenged` and challenged. In `SynReceived` a reset frees the half-open slot in silence.
4. SYN. Past `SynReceived` a SYN is counted in `dropped_syn_challenged` and challenged, wherever its sequence number
   lies and whether or not it passed PAWS: a SYN never ends a synchronized connection. In `SynReceived` a SYN at a new
   number ends the half-open connection, answered with RFC 793's reset for a segment with no connection and counted in
   `handshakes_failed`, as Linux's request does; a SYN with ACK at the first SYN's number that carries payload or a FIN
   loses its SYN to the trim and goes on as the handshake's ACK, as `tcp_check_req` and RFC 9293 3.10.7.4 take it.
5. ACK. A segment without the flag is dropped. With SND.UNA as the segment finds it, an ACK above SND.NXT, of what
   was never sent, is counted in `dropped_unexpected`, and one below `SND.UNA - min(max_wnd, bytes_acked)` in
   `dropped_old_ack` (RFC 5961 5.2, in the form of Linux's `tcp_ack`); either drops the whole segment, payload and FIN
   included, and is challenged. `max_wnd` is the largest window the peer offered, and `bytes_acked` the sum of
   SND.UNA's advances since `Established`, where it is set to zero, so until the peer has acknowledged something every
   ACK below SND.UNA is too old, the SYN's own number included. The bound is read only for an ACK below SND.UNA. A
   forged data or FIN segment is therefore refused unless its acknowledgement also falls in that range, and a segment
   above a gap is held only once its ACK passed, so the end of what is held, where a RST resets, is never set by one
   whose ACK failed. Anything else goes on to the acknowledgement's steps (see "Loss recovery" below).
6. Payload. A *no room* segment, its ACK read, has its payload refused whole, as Linux trims no segment to the window
   either: counted in `dropped_out_of_window` and answered at once with the edge restated. A FIN behind it is not
   taken.
7. FIN.

**The challenge ACK and the limiter.** A challenge is the connection's pure ACK with the edge restated: SND.NXT,
RCV.NXT, the window, the timestamps and whatever SACK blocks it holds, counted in `challenge_acks_sent`. A real peer
that restarted answers it with a RST at our acknowledgement number, RCV.NXT, which resets the connection at step 3; a
forger off the path never sees it. Between two Aloe stacks, an abort whose RST leaves from an SND.NXT ahead of what the
peer has, after a loss, is challenged the same way and resets the connection one round trip later. Each challenge goes
through one limiter per connection, a stamp and `challenge_ack_interval`, 500 ms by default, Linux's
`tcp_invalid_ratelimit`: an answer leaves when none went through the limiter within the interval, and stamps it;
otherwise `acks_rate_limited` counts it. The ACK for a segment the sequence check or PAWS refused goes through it too
when that segment occupies no sequence space or carries a SYN, which is Linux's test, and so does the SYN-ACK sent again
for the peer's SYN again in `SynReceived`. The ACK for a refused segment with payload or a FIN never does: a duplicate,
data past the edge, data with no slot or no node, a FIN sent again because our ACK of it was lost. Nor do an ordinary
ACK, a window update or any RST, each of which answers one segment and is no larger than it. The stamp is shared, as
Linux's is, so a challenge holds back the answer to an empty segment out of the window and the other way round; a
slot's next connection starts with none, and a setting of zero sends every answer. An answer the device refuses waits
for the flush, as any immediate ACK does, and the stamp stands. Two things follow. A Linux peer's zero-window probes
and keepalives are empty segments at RCV.NXT - 1, so they are answered at most twice a second, as Linux answers its
own. And Aloe's own probes back off from one RTO, so against an Aloe peer with the defaults the second probe, 400 ms
after the first, goes unanswered and every later one is answered: the count of unanswered probes never passes one.

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
most one every `rto_max`. The probe shares the connection's timer for its sends.

With data in flight the same rule holds, since RFC 9293's MUST-37 (RFC 1122 4.2.2.17) keeps a connection open while
the peer keeps answering with a zero window. An expiry of the retransmission timer on a synchronized connection that
finds the peer's window zero does what every expiry does, the loss state, the resend of the head and the backoff, but
counts with the probes: against the count of unanswered probes, which every acceptable ACK clears, and not against the
retries `data_retries` bounds, which stand where they stood once the window opens. So such a connection ends
`TimedOut` after `data_retries` expiries in a row with no acceptable ACK between them, or at the user timeout, checked
first as always. Linux's `tcp_retransmit_timer` too stops counting these resends, "our retransmits become zero
probes", and bounds them by time instead, 240 seconds since the last ACK it accepted and since the resends began; the
count is used here so that one rule covers a closed window with data in flight and without.

**A FIN waits for the window.** `close` gives its FIN a sequence number and an entry at once but transmits it
only when the peer's window covers it, Linux's `tcp_snd_wnd_test`: a Linux peer drops a FIN that arrives inside
its zero window and answers with a duplicate ACK, after which only the RTO would resend it. A FIN the window does
not cover, at a zero window or behind data that fills it, waits in the queue unsent; with nothing in flight the
probes go on, in `FinWait1` or `LastAck` too, and the ACK that opens the window, or frees the data in front,
sends the FIN at once and starts its RTO. The congestion window does not gate a FIN, as it does not in Linux. The
peer's ACKs meanwhile are no duplicates, since nothing is in flight to be a duplicate of.

**Loss recovery.** The sender learns of a loss from what the peer reports and from time, as Linux does by default. On a
connection that uses SACK it reads the peer's blocks into a scoreboard on the retransmission queue and marks a segment
lost once a segment sent after it was delivered and a short reordering window has passed (RACK, RFC 8985), and it
probes a silent tail by sending the last segment again after about two round trips, ahead of the timer (the tail loss
probe, the same RFC). On a connection that does not use SACK it counts duplicate ACKs (RFC 5681, RFC 6582). On both,
the retransmission timer is the last resort. What follows a loss is one machine for every connection: three states,
the window in recovery by Proportional Rate Reduction (RFC 9937), resends from the kept packets, lowest sequence
first, and an undo when a resend turns out to have been needless. The policy is told that congestion happened and
sets its target; the rest is the stack's, the same under every policy. An acceptable ACK on a synchronized connection
runs these steps in order, the order of Linux's `tcp_ack` and `tcp_fastretrans_alert`, each as far as it applies:

1. its SACK blocks into the scoreboard, a duplicate report recognised first; an ACK below `acknowledged()` is read too,
   its window left alone, since one below the bound on old ACKs never gets this far (see the first checks above);
2. the cumulative part: the entries it covers freed from the head, `acknowledged()` moved, the round-trip sample;
3. RACK's state, from the entries the blocks and the cumulative part newly report, and whether reordering showed;
4. what the ACK says of a tail probe whose verdict is open;
5. reneging: an ACK that leaves at the head an entry the peer had reported received arms the RTO short, at
   `max(srtt / 2, 10 ms)` (Linux's `tcp_check_sack_reneging`), and does nothing more until step 10;
6. the undo's checks, which may end the episode the ACK found, before anything more is marked;
7. the end of an episode the ACK covers;
8. detection: RACK's on a connection that uses SACK, the duplicate count's on one that does not;
9. the recovery state, which begins, goes on or ends, with the window in recovery; then the resend pass, as far as
   that window allows, unless the undo held back a partial ACK;
10. the peer's window, the zero-window probes and the FIN that waited for the window;
11. after an ACK that ran detection or moved `acknowledged()`, the send timer: the reordering timer, the tail probe or
    the RTO, at the moment the cumulative part recorded. The wheel is armed here, and before only for a resend of the
    head or for a FIN that waited for the window and that step 10 sends, armed as every first send is. Any other ACK
    leaves the timer as it was.

**The scoreboard is two marks on the retransmission queue.** Each entry can be marked reported received, by a SACK
block, and lost, which owes a resend; the queue keeps the totals, the bytes and the segments reported, the bytes lost
and not resent since, and the highest sequence the peer acknowledged either way, so nothing walks the entries to count
them. A block marks an entry only when it covers the entry whole: one that covers part of an entry marks nothing, as
Linux leaves a buffer of one segment (`tcp_match_skb_to_sack`) and Onload leaves its packets. Part covers are rare,
since the entries are the segments as they left, and their cost is bounded: the entry may be resent needlessly, and
the peer's duplicate report undoes the reduction. A block touches the entries only when it is valid by Linux's
`tcp_is_sackblock_valid`: it starts before it ends, ends at or before SND.NXT and starts above the ACK, where
`acknowledged()` is about to be, or above `acknowledged()` for an older ACK; a block that fails is ignored and the
others are applied, lowest first. A duplicate report (RFC 2883) is recognised first, from the option alone, as Linux's
`tcp_check_dsack` and `tcp_dsack_seen` recognise it: the first block, when it starts below the ACK or the second block
contains it, and is no longer than the largest window the peer offered. It is believed unless the peer has by then
reported more duplicate segments than the connection ever resent; a believed one marks nothing received, and what it
does is the probe's, the undo's and RACK's, below. A SACK option whose length is
not two plus eight bytes per block, one to four of them, is ignored and the segment kept, as Linux's
`tcp_parse_options` does, where a bad length of any other option drops the segment.

What is in flight, the number the congestion window is held against, is SND.NXT - SND.UNA less the bytes reported
received and less the bytes marked lost and not resent since: RFC 9937's estimate for a sender that detects by RACK,
Linux's `tcp_packets_in_flight` in bytes, and `in_flight()`. A resend clears its entry's lost mark, so the entry counts
as in flight again. Without SACK nothing is reported, and one segment per duplicate ACK stands in for it: the duplicates
counted since `acknowledged()` last moved, which in recovery an ACK of new data does not clear but takes the segments
its advance covers off (`tcp_remove_reno_sacks`), capped so that the holes, at least one segment, stay counted, as Linux
emulates the blocks (`tcp_add_reno_sack`, `tcp_limit_reno_sacked`); after a timeout a duplicate stands for nothing.
`writable()` holds the congestion window against what is in flight and the peer's window against the sequence space
outstanding. That is where RFC 3042's limited transmit comes from: a report, or a duplicate ACK, lowers what is in
flight and lets new data out, with no separate allowance and no cap of two segments, as in Linux (`tcp_cwnd_test`); the
recovery that begins at the third report or duplicate is what bounds it.

An entry reported received keeps its packet and its place until the cumulative ACK frees it. When an ACK moves
`acknowledged()` and leaves such an entry at the head, the peer discarded what it reported: the ACK arms the RTO short
and does nothing else, and if the head is still reported when it fires, every received mark is cleared and everything
is lost (Linux's `tcp_timeout_mark_lost`), counted in `sack_reneging`. A byte a block had reported is then counted
again in `delivered()` when the cumulative ACK covers it, as Linux counts it again: the peer delivers it twice.

**RACK marks by time.** On a connection that uses SACK, each entry an ACK newly reports, by a block or by the
cumulative part, updates RACK: its round trip is taken, and it becomes RACK's segment, the most recently sent one the
peer acknowledged either way, when it was sent later, by stamp and then by sequence, since the segments of one tick
share a stamp. An entry that was resent is skipped when the ACK cannot be for the resend: its round trip is below the
minimum round trip, or, for a cumulative ACK, the echo is older than the resend. An entry neither reported nor already
lost is then lost once it was sent before RACK's segment and `sent + RACK's round trip + the reordering window` has
passed; an entry whose time has not passed yet sets the reordering timer, for the longest wait left, so one expiry
settles them all (`rack_marked`, `reorder_timeouts`). An entry already resent is judged by its resend's stamp, so a
resend that is lost again is found by the next report and resent under the same episode, counted in
`lost_retransmits`, where it used to wait for the timer. The walk over the queue ends at the first entry that was never
resent and was not sent before RACK's segment, so an in-order ACK with nothing reported or resent pays one comparison
at the head.

The reordering window, `reordering_window()`, is zero while reordering was never seen and the connection is in recovery
or the loss state or has three segments or more reported received: with no evidence of reordering, RACK is as quick as
three duplicates were. Otherwise it is a quarter of the minimum round trip, kept over two windows of five minutes, times
a multiplier, and never more than the smoothed round trip; zero before any round trip is known. Reordering is seen
(`reordering_seen`) when a block newly reports a never-resent entry below the highest sequence already reported, when
the cumulative ACK fills such a hole with a never-resent entry, when a partial ACK in recovery answers an original by
its echo, and when a duplicate report names a resent entry a block had reported. The multiplier starts at one and grows
by one in a round trip that brought a believed duplicate report once reordering was seen, but not for a tail probe's; it
persists for 16 recoveries, its count dropping at the end of each episode, recovery or loss state, that was not undone
and rising at each undo by duplicate report, and at zero it is one again. These are Linux's `tcp_rack_reo_wnd` and
`tcp_rack_update_reo_wnd`, with RFC 8985's constants.

What reordering costs. Reordering shallower than three segments and shorter than a quarter of the minimum round trip
costs nothing: the late segment's ACK arrives before the reordering timer and cancels it. Deeper reordering on a
connection that has seen none causes one needless resend, since three segments reported make the window zero, and the
echo of the late original's ACK undoes the reduction it caused. When more data was outstanding above the late segment,
that ACK is a partial ACK and records reordering, so from then on RACK waits a quarter of the minimum round trip before
it marks. When the late segment ended an isolated flight, with nothing sent after it, the ACK that answers it ends
recovery and records nothing, as Linux's `tcp_try_undo_recovery` records nothing there: each such reordering costs one
needless resend, undone, and RACK's window stays zero for the next.

**The tail loss probe resends a silent tail.** On a connection that uses SACK, open and with nothing reported received,
a data segment sent and an ACK that moves `acknowledged()` and leaves data out each schedule the probe: two smoothed
round trips from that moment, `rto_initial` before any sample, plus `tail_probe_ack_delay`, 40 ms by default, when
exactly one segment is in flight, for the peer's delayed ACK; never later than the RTO's moment, and a probe due then is
still a probe. The wheel rounds the deadline up to its tick, the only floor. When the timer fires, the probe goes if
none is in flight and a round-trip sample was taken since the last one (RFC 8985 7.3): the last segment sent goes again
whole, through the resend of an entry, so the busy rule and the copy apply to it. It is a tail probe (`tail_probes`) and
nothing else: it takes no retry, backs nothing off, clears no mark, starts no episode and is not one of an episode's
resends. The RTO runs from the probe; when none went, the RTO keeps its moment. A probe is one once its resend leaves,
and not before. When the device still holds its packet, the busy rule holds the resend and the probe is pending: not in
flight, no verdict, counted nowhere, no other scheduled, and the RTO keeps its moment. It becomes a probe when the flush
sends it, in place or as a copy, counted then and with the RTO from then; when its wait ends otherwise, an ACK freeing
its entry, a block reporting it, the send refused, or a recovery or a timeout beginning, it never was one.

RFC 8985 keeps one variable, `TLP.end_seq`, for two facts that the connection keeps apart (`tail_probe_in_flight()`,
`tail_probe_verdict_open()`): the probe is in flight until a cumulative ACK reaches its end, and its verdict is open
until it is known whether it was needless or repaired a loss. An ACK that lands exactly on the probe's end may answer
the original or the probe, so the verdict usually outlives the flight. An ACK at or past the probe's end settles it
(RFC 8985 7.4, Linux's `tcp_process_tlp_ack`): a duplicate report of at most one segment that ends at the probe's end,
believed or not, as Linux reads it, says both copies arrived, and so does a plain duplicate ACK with no SACK option,
so the probe was needless (`tail_probes_needless`); an ACK past the probe's end with no such report says it repaired a
loss: the policy is told of one loss and its window becomes its target at once, the cut a fast recovery would make,
with nothing resent, no state entered and no undo kept (`tail_probe_recoveries`). When the probe's ACK reports the
tail received while earlier segments are still missing, RACK marks them, since the probe was sent after them: the
probe has turned a silent flight into an ordinary recovery. A recovery or a timeout that begins forgets the probe.

Two lone orders lost one after the other are both probed: where RFC 8985's procedure and Linux send no probe while the
first's verdict is open, so that the second order waits for the RTO, Aloe sends the second probe once the first's end is
acknowledged and a round-trip sample was taken since, and closes the first verdict as a loss repaired
(`tail_probes_early`), when the second probe leaves. That is its one departure from RFC 8985 by the author's decision,
set out with what it keeps and what it needs, timestamps among them, in the
[note on departures](#where-the-brick-departs-from-the-rfcs-and-from-linux).

**One recovery, three states.** The connection is open, in recovery or in the loss state (`recovery_state()`).

- *Entering recovery.* The first entry marked lost while open begins it: by RACK, on an ACK or at the reordering
  timer; without SACK, by the third duplicate ACK whose acknowledgement covers more than `recover`, RFC 6582's guard
  against a second fast retransmit for what a timeout already resent. On entry the connection records `recover`, the
  last sequence sent, opens the undo with a copy of the policy's state, tells the policy of the congestion, which sets
  its target from the flight less what was sent beyond its window (RFC 5681 3.2 leaves out what limited transmit
  sent), and starts the rate reduction; `recoveries` counts it.
- *In recovery.* Each ACK runs the undo's checks, then detection, then a step of the rate reduction, then the resend
  pass. Without SACK, a partial ACK, one that moves `acknowledged()` short of `recover`, marks the new head lost, as
  RFC 6582 has it and Linux's `tcp_newreno_mark_lost` does. A resend found lost again is resent under the same episode
  and reduces nothing further. New data leaves as far as the window in recovery exceeds what is in flight, after the
  resends. No ACK reaches the policy's `on_ack` in recovery, the one that ends it included.
- *Leaving recovery.* At the ACK that covers `recover`: the window becomes the policy's target, the rate reduction's
  last step, unless the episode was undone. An entry marked lost that this ACK does not cover begins the next episode
  on the same ACK.
- *The timeout.* The RTO's expiry gives the connection up or resends the head as before: the user timeout, the retry
  cap, the backoff, one segment at the expiry itself (`retransmits`). What it marks: the head; on a connection that uses
  SACK every other entry not reported received and last sent at least RACK's round trip ago, later ones left to RACK;
  on reneging everything; without SACK everything. The connection enters the loss state, records `recover` and forgets
  a probe that was out. The policy's target is set again only when this window was not reduced already: the
  connection was open, or the last episode's `recover` is covered, or this is the loss state's first expiry since an
  ACK of new data. Its window restarts at what is in flight after the marking plus one segment, which is one segment
  when everything was marked.
- *The loss state.* Each ACK of new data reaches the policy, which grows its window by slow start while the window is
  in use; the entries the timeout marked leave by the resend pass, lowest first, as that window allows; RACK keeps
  marking, and a resend it finds lost again moves the connection to recovery with a new target, as Linux does. No tail
  probe is scheduled. The state ends at the ACK that covers `recover`, or by an undo. A reordering timer that marks
  something outside recovery begins recovery there, from the loss state too.

Every ACK that moves `acknowledged()` while anything is in flight re-arms the RTO from now, RFC 6582's Slow-but-Steady
variant, as Linux re-arms it. While open, every ACK of new data slides `recover` to just below `acknowledged()`: the
guard compares in serial arithmetic, which would read a `recover` left more than 2^31 behind as ahead and refuse every
recovery after 2 GiB without loss. An episode that ends at exactly the ACK that covers `recover` leaves it in place, so
on a connection without SACK duplicates at that acknowledgement begin no recovery until an ACK of new data moves
`acknowledged()` above it (RFC 6582): a loss right there is repaired by the timer.

**The window in recovery is RFC 9937's rate reduction, in bytes.** With SACK, what an ACK delivered is its advance of
`acknowledged()` plus the change in bytes reported received. Without SACK it is one segment for a duplicate ACK, and
for an ACK that moves `acknowledged()` in recovery the advance less one segment per duplicate counted before it, and at
least the segment that filled the hole, never more than the episode may still see delivered (RecoverFS, the flight
recorded on entry). While what is in flight is above the target, sending follows the proportional part, the target's
share of what was delivered, divided by RecoverFS and rounded up; at or below it, the conservative bound, what was
delivered and not yet sent, plus a segment on an ACK that moved `acknowledged()` and marked nothing new lost (the RFC's
SafeACK), never past the target. The first step of an episode always allows a segment, so the first resend leaves at
once. The window, `congestion_window()`, is what is in flight plus what the step allows, and resends and new data
both count against it and into what the episode sent. Because it counts bytes, the rate reduction lets out part of a
segment where a stack that counts packets lets out one segment every second ACK: `writable()` in recovery can offer
less than `mss()`, and a sender that writes only whole messages waits for room for one. A policy whose target is
unbounded, `NoCongestionControl`, has no rate reduction: what is marked lost goes again as soon as it is marked.

**The undo gives a needless reduction back.** An episode that set a target, a recovery or a timeout, keeps a record from
its start, as Linux's `tcp_init_undo` does: `acknowledged()` then, the policy's state then, the stamp of its first
resend, and how many resends may still be reported as duplicates, starting from earlier resends still out and a tail
probe still in flight. Two signs say the reduction was needless (Linux's `tcp_may_undo`). Every resend of the episode
was reported as a duplicate (`tcp_try_undo_dsack`); that sign holds after the episode ended too, until the next one
begins, since the reports trail the resends. Or an ACK that moves `acknowledged()` answers an original: it echoes a
timestamp older than the episode's first resend, or nothing of the episode has been resent yet, which the busy rule
makes possible (`tcp_packet_delayed`); that sign is read on a partial ACK in recovery, on the ACK that ends recovery,
and on an ACK in the loss state, where it recognises a needless timeout. An undo hands the policy its state from the
start (`on_undo`; NewReno takes the larger of each pair), at most once per episode, and is counted by its sign
(`undos_duplicate_sack`, `undos_original_ack`). On a partial ACK it records reordering for RACK at once, and waits while
a resend of the episode is still out, that ACK marking and resending nothing more; then every lost mark is cleared and
the connection is open. On the ACK that ends recovery the connection is open with its window restored in place of the
target. In the loss state every lost mark and the retry count are cleared and the connection is open, and the policy
then hears the ACK from the window restored, as Linux's `tcp_cong_control` grows after the undo. By duplicate
report in recovery the episode ends there and the marks stay. After an undo RACK looks at the queue again, so a real
loss behind a reordering starts a new episode. The timestamp clock ticks in milliseconds, so an original and its resend
sent in the same millisecond cannot be told apart and no undo follows: that errs on the side of the reduction. Without
timestamps only the duplicate reports, and an ACK before any resend of the episode left, undo.

**The window grows only while it is used.** NewReno grows its window on an ACK of new data only when the window limited
what was sent (Linux's `tcp_is_cwnd_limited`, which `tcp_reno_cong_avoid` asks first): when a send of the round left
less than a segment of the window or was held back by it, or, in slow start, when the window is less than twice the
most in flight in the round. `WindowUse` keeps that record at each send of new data, Linux's `tcp_cwnd_validate` in
bytes, and `AckInfo` passes it to the policy; a resend leaves it as it is. As before, a send into a queue that has been
empty for longer than an RTO tells the policy of the idle period first, RFC 5681's restart after idle, which Linux
enables by default. A venue client that sends an order at a time keeps its initial window, ten segments, for good, and
at the default `send_segments` of 32 a burst of small orders fits it many times over.

What such a client must know is what a loss leaves. A recovery, at its end, and a tail probe that repaired a loss set
the window to the target, RFC 5681's half of the flight and at least two segments: with an order or two in flight, two
segments. A timeout restarts the window at one segment, and slow start grows it only while a send leaves less than a
segment of it unused, so with one small order at a time it settles at one segment and one order: 1548 bytes for orders
of 100. From there the window grows only when a burst leaves less than a segment of it unused, which small orders sent
one at a time never do. Linux, which halves the window, would leave five of the ten after a recovery. So a client that
sends an order at a time keeps ten segments until its first loss and two, or after a timeout little more than one,
after it, and a burst larger than that then takes more than one round trip. A deployment that cannot accept that runs
`NoCongestionControl` on a private path it owns, or writes a policy of its own.

**What an in-order ACK and a send cost.** For a connection that uses SACK, with nothing lost or reordered, an ACK of an
order frees its entry, writes RACK's fields for it and feeds its round-trip sample to RACK's minimum and to the probe; a
pending probe, the probe's verdict, reneging and the undo's record are a branch each; detection runs, its walk stops at
the head, and it computes no reordering window and writes no counter; the policy's growth returns at once when the
window was not in use; the resend pass returns without a walk; and the timer is cancelled when nothing is left in
flight, else armed once, for the probe, at a deadline the RTO's moment bounds. The send of an order makes two
subtractions more in `writable()`, reads what is in flight and the congestion window once for its check, its entry and
the window's use, records that use, computes the probe's deadline and arms the wheel for it in place of the RTO, with
the RTO's moment kept beside it. No entry is walked, no block read, no state entered, nothing copied. It is not free,
and it misses the bar of 10 to 20 ns set for the step: on the [tick-to-trade benchmark](../benchmarks/tick-to-trade.md),
against `main` at this step's base, the 64-byte tick at level 0, cross-core, is 40 ns slower at p50, 450 to 490 ns, and
60 ns at p99, 670 to 730 ns, 30 of the 40 in the `tcp` leg that acknowledges the previous order. That is after the work
a review found plainly unnecessary here was taken out: against the first rerun the gap is the same 40 ns at p50, `main`
having come out 10 ns lower too, 460 to 450 ns, and 10 ns narrower at p99, 70 to 60 ns. The benchmark page records both
reruns, what the diagnostics measured of the cause and what they did not isolate, and the changes that could win the
rest back.

The first checks add little to that path: an in-order segment makes the comparison that tells *no room* from in order,
which the edge test already makes, and its RST and SYN tests are flag tests; an in-order ACK adds one addition, to the
sum of SND.UNA's advances, since the bound on an old ACK is read only for an ACK below SND.UNA; a send adds nothing; and
the limiter is read only by the answers to a segment that failed a check. Measured against `main` at its base, the
64-byte tick at level 0, cross-core, is 490 ns at p50 on both sides and 720 against 740 ns at p99, the `tcp` leg 220
against 230 ns at p99. A first rerun's headline runs had given 770 against 720 ns at p99, which neither its breakdown
runs nor a second rerun reproduced. The benchmark page records both.

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

**The handshake's options.** Window scaling (RFC 7323) and SACK-permitted (RFC 2018) are negotiated as the timestamps
are: with `window_scaling` and `sack` on, the defaults, every SYN offers both, a SYN-ACK answers each option the peer's
SYN offered, and each is in use only when both ends sent it. A resent SYN or SYN-ACK carries the same options; an offer
never answered, or an active open that ends before the answer, negotiated nothing. The block is Linux's
(`tcp_options_write`), so a capture or a script reads as a Linux one does: the MSS; then the timestamps as
`nop, nop, TS`, with `sackOK` taking the place of the two `nop`s when SACK is offered, or `nop, nop, sackOK` without
timestamps; then `nop, wscale`. That is 20 bytes with every option and 12 without timestamps. The peer's window scale
and SACK-permitted are read from a SYN or SYN-ACK only, as its MSS is, and a wrong length makes the option list
malformed, as a wrong MSS length does; a peer's shift above 14 is used as 14, as RFC 7323 requires. The SACK blocks the
peer sends are read on every ACK of a connection that uses SACK (see the scoreboard above); a SACK option whose length
is wrong is skipped and the segment kept, as Linux's `tcp_parse_options` skips it.

**A floor under the peer's MSS.** The peer's MSS is the value of its option, or RFC 1122's 536 when its SYN or SYN-ACK
carries none, and a handshake with a peer whose MSS is below `min_peer_mss`, 48 by default, a stated zero included, is
refused. On a passive open the SYN takes no slot and is answered with RFC 793's reset for a segment with no connection.
On an active open the SYN-ACK is answered with `<SEQ=SEG.ACK><CTL=RST>`, the connection is closed and `Reset` is
raised, so a hand-written loop and the runtime's `connect`, which reports `Refused`, see an open that was refused; it is
not counted in `connections_reset`, which counts RSTs received. Both count in `handshakes_refused_mss`, and the
constructor refuses a floor below 48 or above the stack's own MSS. Linux raises such an MSS to its floor,
`tcp_min_snd_mss`, and goes on, which sends the peer segments larger than it said it accepts (RFC 9293's MUST-16) and
leaves a venue that states a broken MSS running on segments nobody notices are tiny; refusing fails the open at once
with a counter that names the cause. With the floor at its default the smallest `mss()` a connection can have is 36
bytes with timestamps and 48 without, so its receive budget and `writable()` are never zero.

**The payload of a SYN** to a listener is neither held nor acknowledged, as in Linux without Fast Open: the SYN-ACK
acknowledges the SYN's number alone and the peer sends the payload again after the handshake. `syn_payload_discarded`
counts such SYNs once the listener took a slot for them; one refused before that counts under its refusal alone.

**A budget per connection, counted in `mss()`.** The receive window is a byte window over a budget fixed once the
handshake has settled the peer's MSS and the options: half of what `receive_segments` segments of the
connection's `mss()` hold, or `receive_window` when set, never more than the slots hold, and never more than the
field states, 65535 shifted by our scale to a peer that accepted scaling and 65535 to one that did not. With the
defaults that is 512 * 1448 / 2 = 370688 bytes with timestamps and 373760 without. Both opens compute it only
after the peer's MSS is stored and the connection has left `SynSent`, because `mss()` reads both. The half is
the margin for what real streams carry: a short segment takes a whole slot, and every message that ends with a
push ends with one, so a stream of full-sized segments closes the window with half the slots still free, and the
slots outlast the window for any stream whose segments average half a full one or more. The same `mss()` is the
full-sized segment wherever the receive side asks, so a peer with a small MSS is acknowledged every second
segment too.

**The offer moves by full segments, and never back.** A segment that carries an ACK offers the edge
`rcv_nxt + (budget - unread)`, with `unread` the in-order bytes; what is held above a gap sits between RCV.NXT
and the edge and changes nothing. That edge replaces the one already offered only while a packet slot is free,
counting both chains, so a connection at its cap stops offering more, and only when it lies at least
`min(budget / 2, mss())` beyond it, RFC 1122's rule against silly windows (4.2.3.3), which also keeps a closed
window closed until a full segment fits. A read queues a window update of its own only when the window it could
offer is at least twice the one on offer, Linux's rule in `__tcp_cleanup_rbuf`: that covers a window that had
closed or nearly closed, and leaves every other movement of the edge to the segments that leave anyway, so a
small read costs nothing.

**The scale is the smallest that states the budget, and the field rounds up.** Our scale is one shift per stack,
computed at construction: the smallest, at most 14, under which the 16-bit field states the largest budget the
configuration allows, counted in the stack's MSS without timestamps. With the defaults that budget is 373760, the shift
3 and the unit 8 bytes. With `window_scaling` on, every SYN offers it, a zero shift included, since offering is also
what lets the peer scale its own window, and a SYN-ACK answers with it only when the peer's SYN offered it. The field of
a SYN or SYN-ACK is never scaled: a SYN, written before the connection has a budget, states the stack's largest budget
or 65535, whichever is smaller, and a SYN-ACK the connection's budget or 65535. The first segment with an ACK after the
handshake makes the first offer from the connection's budget, scaled: 370688 >> 3 = 46336 with the defaults. Under a
small cap or against a small MSS that can be less than the handshake's field said, which no data has relied on yet. The
field is the distance from RCV.NXT to the edge shifted right by the scale and rounded up to the unit, as Linux's
`tcp_select_window` and `__tcp_select_window` round, and the connection records the edge the peer reconstructs from the
field, never the one it computed, so the edge never moves left as the peer sees it. It can move right with nothing
consumed: each segment that leaves after RCV.NXT moved rounds again from the new RCV.NXT and can add up to a unit less a
byte, so over many segments the edge can stand ahead of the budget by more than a unit, and a stream of segments no
larger than the unit, each acknowledged alone, never closes the byte window. Linux's edge creeps the same way. It is
harmless because the budget is not the memory limit, the packet cap is, and RCV.NXT moves only while a slot is free; it
is why the shift is the smallest that states the budget. Rounding one computed edge down each time instead would move
the reconstructed edge left by up to a unit.

**A segment above a gap is held as the packet it arrived in.** A segment whose payload or FIN starts above RCV.NXT
inside the window goes on through the first checks. A RST on it is challenged unless it stands at one of the positions
that reset, and a SYN is challenged; its ACK field must pass the bound on old ACKs, and then it and the window are
processed like any other, so a hole in one direction does not stall the other. Its payload goes on a second chain of the
shard's held-segment nodes, the reassembly queue, sorted by sequence, each node the packet with the offset, length and
first sequence of what is kept, counted in `out_of_order_held`. Three rules keep the ranges disjoint without a copy, the
first two Linux's (`tcp_data_queue_ofo`): a newcomer that held data covers entirely is a duplicate, dropped and counted
in `dropped_duplicate`; a held node the newcomer covers entirely is released and the newcomer takes its place; where the
two overlap in part the held bytes win, and the newcomer loses its front up to the end of the node before it and its
tail from the start of the node after it, by moving its offset, its length and its first sequence. A segment at or past
the end of the chain's last node, the run after a loss, is appended without a walk, as Linux keeps `ooo_last_skb`; any
other placement walks from the head, a walk bounded by `receive_segments`, and no tree is built. An in-order segment
asks one question before it is held, whether anything is held above, and a clean stream never takes the branch. When
something is, the run that now follows RCV.NXT joins the in-order chain by a change of links: a node RCV.NXT passed
entirely is released, one it passed in part loses its front, and every node that starts at RCV.NXT moves to the tail of
the in-order chain, RCV.NXT and `unread()` with it. `Readable` covers the whole run, and the ACK goes at once. The join
takes no node and moves no packet, so it cannot fail. Until then `unread()` shows nothing of what is held above;
`out_of_order_segments()` and `out_of_order_bytes()` report it. A pure ACK above RCV.NXT, which the peer sends for as
long as a hole is open, is processed and never answered: an answer would hand the peer duplicate ACKs for segments it
never sent.

**A FIN above a gap is one number.** A FIN on a segment above the gap, or a bare one, is remembered as the sequence it
occupies, as TLDK represents it, rather than kept with a packet, as Linux keeps it with its buffer (`tcp_ofo_queue`). It
is remembered once every byte below it is held, newly or already, so a FIN on a segment whose payload duplicates held
data counts too. When RCV.NXT reaches it, the FIN is taken as an in-order FIN is, through the `Lifecycle`, and whatever
is still held above it is released, as Linux's `tcp_fin` purges its queue. RCV.NXT never passes it: the join stops
there, and a held segment that straddles it gives the run only its bytes below it. Data whose payload ends past a
remembered FIN, in order or above the gap, is refused as data after the peer's FIN is (`dropped_unexpected`); a FIN
above the remembered one is refused the same way, and a FIN below it replaces it, the replaced number leaving the SACK
blocks. An in-order segment that carries data and the peer's FIN while something is held above does not join: its FIN
step releases what is held, so no hole is reported filled, and its ACK goes as an in-order FIN's does, at the `flush` in
`Established`. That ACK is the segment's only one: none leaves for its data before the FIN step has dropped the blocks.

**Two limits on what is held, independent of each other.** The byte window is the limit the peer obeys: when
unread bytes fill it the peer stops and nothing is lost. Separately, a connection holds at most
`receive_segments` packets, in order and above a gap together, and the shard's held packets come from one pool of
`receive_pool` nodes. The cap is the limit a stream of short segments the application leaves unread can reach
first: a window of W bytes over S slots cannot overflow the cap while the segments average W / S bytes or more,
half a full segment with the default window. Below that, a segment that arrives at either limit takes the node of
the highest segment held above a gap when that one starts above it, as the next note says; otherwise it is
dropped, counted (`dropped_no_slot` or `dropped_no_node`, or `dropped_out_of_order` above a gap) and answered at
once with the edge restated, the peer's retransmission recovers it, and the offered window is never taken back.
`receive_window` and `receive_segments` are the controls for a feed of small segments: more slots under the same
window. Where a backlog sits depends on who has stopped. When one thread runs the loop and the application is
busy, the loop does not run, so the backlog waits in the device's receive ring and the peer can send no more than
the window it was last offered; the slots fill when the stack runs and the application does not read, across
ticks or beside a stalled reader on another core. Linux answers an in-order segment it cannot queue for memory
with a zero window, the one case where its edge moves left (`tcp_select_window`), and collapses small buffers into
fewer under memory pressure, as TCPDirect and Onload copy theirs; whether Aloe closes the window at the cap, or
copies there, is open as issue #39, and the rule lives in one function of the `Receiver` so the answer changes
one place.

**At the cap the lower sequence wins.** Both chains count against one cap and take nodes from one pool. A segment
that needs a node when the connection is at its cap, or the pool is empty, takes the node of the highest segment
held above a gap when that one starts above it, as Linux's `tcp_prune_ofo_queue` prunes from the highest sequence
down and never below the incoming segment. One node goes for one segment, where Linux frees an eighth of its
buffer: Aloe's unit is the packet, and a second release would give up data nothing asked for. A newcomer that
needs no fresh node costs nothing: a duplicate of held data is dropped, and one that replaces a node it covers
takes that node's slot. The release is counted in `out_of_order_released`, forgets a remembered FIN, which the
peer resends, and cuts the released node's SACK block back in the same step, so no later ACK reports what is
gone; its end is remembered as a refused segment's is, so in-order data is acknowledged at once until RCV.NXT
reaches what the peer was told had arrived. A segment above the gap that would itself be the highest is dropped,
counted in `dropped_out_of_order`, and its end remembered the same way; an in-order segment with nothing held
above is dropped as on a clean stream. A terminal event (`Closed`, `Reset`, `TimedOut`, `Unresolved`) returns the
out-of-order packets at once, since nothing can fill the gap any more.

**SACK blocks ride on pure ACKs only.** A connection that negotiated SACK keeps up to four blocks, Linux's
`TCP_NUM_SACKS`, in RFC 2018's order: the block that holds the segment just received first, then the others, most
recently changed first. A new segment above the gap extends the block it touches, merges blocks it joins or starts a
block at the front, and the oldest falls off past four; blocks at or below RCV.NXT go when it moves (`tcp_sack_remove`).
A release from the middle of a block ends the block at the release: it may under-report what is held, and never reports
what is not. A remembered FIN's number is part of the block that ends at it, or a block of its own for a bare FIN, as
Linux's blocks cover a held buffer's FIN; without it a Linux sender would take the segment that carried the FIN for not
yet received and send it again. Every pure ACK the connection sends while it holds a block or owes a duplicate SACK
carries the option after the timestamps, `nop, nop, sack` and eight bytes per block: three blocks fit beside the
timestamps and four without (`tcp_established_options`). A data segment never carries blocks, and neither does a probe
or a RST. A connection's data segments have one header size, so a kept packet is rewritten in place on a resend and a
prepared payload sits at a fixed offset; an option that grew the header would move the payload. Linux carries blocks on
data segments too and shrinks the payload for them (`tcp_current_mss`); Onload and lwIP keep them to pure ACKs, as Aloe
does. For the same reason a data segment that leaves does not cancel an ACK that owes blocks or a duplicate: that ACK
goes as a pure ACK at the next `flush`. Every segment above a gap is answered within its tick, so the peer learns of
each change of the blocks in the tick it happens. Only an ACK that reports builds the option, so an ordinary ACK pays
nothing for it.

**Duplicate SACK.** RFC 2883, which Linux sends by default; `sack` turns both on. The first block of the next pure
ACK reports payload that arrived twice, and the regular blocks follow it. There are four sources: a segment
refused as a duplicate or by PAWS whose payload starts before RCV.NXT, its payload up to RCV.NXT, with the ACK at once
(`tcp_send_dupack`); the old prefix of an in-order segment, from its first byte of payload to the RCV.NXT it met, with
the ACK at its ordinary time (`tcp_data_queue`); a segment, or the part of one, already held above the gap, followed by
the block that holds it, with the ACK timed as for any segment above a gap (`tcp_data_queue_ofo`); and at the
join, the part of a held node that RCV.NXT had already passed, with the ACK the join sends (`tcp_ofo_queue`). A SYN's
or a FIN's number is not payload and is never reported as a duplicate, so a SYN-ACK the peer sends again after the
handshake is answered with no block, and a RST reports nothing; Linux's `tcp_send_dupack` counts an old FIN's number
in its range. One duplicate is pending at a time: a second range extends it when the two touch and is dropped otherwise,
as `tcp_dsack_extend` does. It is forgotten once an ACK carried it, or when the next segment with payload arrives, as
Linux clears it on entry to `tcp_data_queue`. A Linux sender uses the report to undo a retransmission that reordering
made spurious, as Aloe's own sender does (see the undo above), which keeps a venue's stream fast on a path that
reorders.

**The peer's window comes from current acknowledgements.** `writable()` is the MSS, within what the two windows allow
beyond what is out: the peer's window less `unacknowledged()`, and the congestion window less `in_flight()`; it is zero
at the send cap and while an entry is marked lost and not yet resent. The peer's window is updated by RFC 793's rule, a
newer sequence or the same one with a newer acknowledgement, and only from a segment whose acknowledgement is at or past
`snd_una`, so a stale or forged acknowledgement cannot rewrite it. `commit` checks the credit again, because the peer
may have retracted its right edge, or a loss shrunk the congestion window, since the prepare; RFC 1122 asks the sender
to cope with a retracted edge by not sending into it. The window field of every segment on a synchronized connection is
shifted left by the peer's scale before anything reads it, the window update, the duplicate-ACK test, zero-window
probing and `writable()`; the field of a SYN or SYN-ACK is taken as it is, and so is that of a SYN-ACK that carries
data, since the peer's shift is stored only once the SYN-ACK has been processed. A peer that did not offer scaling has a
shift of zero. `snd_wnd` was already 32 bits wide, so a window past 65535 lets `writable()` and the congestion window
decide.

**ACKs wait, with exceptions.** With `delayed_ack` zero, the default, an ordinary acknowledgement of in-order data
is queued and sent by `flush`, unless a segment carrying an ACK leaves first, so a request answered in the same
tick costs one segment. It goes at once for every second full-sized segment, a full-sized segment being the
connection's `mss()`, the smaller of the two MSS less the timestamp option on a connection that carries it. A venue
that left Nagle on would stall behind a delayed ACK, and a client that answers ticks within the tick gains nothing
from one, which is why the delay is off by default. With `delayed_ack` set, at most 500 ms, the ACK of in-order
data waits instead on the connection's second wheel timer, counted from the first segment it covers and never
extended, until a segment carrying an ACK leaves, which cancels the timer, or two full-sized segments' worth of
bytes is unacknowledged, counted in bytes so that a stream of short segments is acknowledged as often as one of
full segments. Linux acknowledges once more than its estimate of the peer's segment size is unacknowledged
(`__tcp_ack_snd_check`), which follows short segments more closely still, and adapts its delay; Aloe's delay is one
fixed duration. The timer is armed only while a delayed ACK is pending, and never with the delay off. When it
fires the pure ACK goes at once, counted in `delayed_acks_sent` as well as `pure_acks_sent`; a refused one waits on
the ACK list for the `flush`. Only the ACK of in-order data is delayed: the handshake's ACK, a FIN's and a window
update stay where they were. A data segment that ends the delay while blocks or a duplicate SACK are owed leaves
the connection queued for the `flush`, which sends them on a pure ACK.

In both modes an ACK goes at once, before the next segment is processed: for a segment refused with an answer (a
duplicate below RCV.NXT, a segment out of window, with no room or older than `TS.Recent`, data after the peer's FIN or
past one remembered above a gap, a second FIN, a segment refused at the cap or for want of a node), through the limiter
when that segment occupies no sequence space or carries a SYN, while a segment without ACK is dropped unanswered; as a
challenge, through the limiter, for a segment the first checks challenge; for a segment above a gap, every one on a
connection without SACK and the first three since RCV.NXT last moved on a connection with it; for in-order data while
anything is held above it, and until RCV.NXT reaches the end of a segment above a gap that was refused or released for
room, except that such data on a segment with the peer's FIN is acknowledged by the FIN's ACK alone; and for a FIN the
connection is closing on. A refused immediate ACK stays queued for the next `flush`.

On a connection that uses SACK, three duplicates start the peer's fast retransmit and the blocks carry everything
else, so from the fourth segment above a gap on, until RCV.NXT moves, the answer is owed to the `flush` of the same
tick: one pure ACK per connection, with the blocks as they then stand. A burst of thirty segments behind one loss
costs four ACKs, not thirty, and the peer still learns everything within the tick. Linux sends three and batches
the rest on a timer of at most a millisecond (`__tcp_ack_snd_check`); the `flush` is the boundary a polling stack
already has. An answer that also acknowledges in-order data not yet acknowledged goes at once and is not one of
the three: it moves the peer's `snd_una`, so the peer does not count it as a duplicate, and three true duplicates
follow it. Linux counts it; Aloe departs because a sender that counts duplicates even with SACK, as Onload's does,
needs three true ones. On a connection without SACK every segment above a gap is answered at once, as Linux answers
them (`__tcp_ack_snd_check` sends at once when SACK is off): such a sender reads the duplicates themselves, the
third starting fast retransmit and each one, whether it inflates the window by a segment (RFC 5681 3.2) or, as Aloe's
and Linux's do, takes a segment out of what it counts in flight, letting it send new data while it recovers.

A duplicate ACK stays a duplicate. The ACK sent at once for a segment above a gap, a duplicate or any refused segment
restates the edge already offered, whatever `consume` has freed since: RFC 5681 counts an ACK as a duplicate only when
its window equals the last one's, and Onload freezes the edge the same way. The ACK owed to the `flush` is an ordinary
one and may move the edge. A restating ACK that leaves never puts a connection on the ACK list. When the connection was
already queued for the flush and its ordinary offer would move the edge, it stays queued and the flush sends that
update; otherwise the restating ACK does the queued ACK's work and nothing more leaves, so the peer is not handed an
identical ACK it would count. Like any segment that carries an ACK, a restating ACK also cancels a pending delayed ACK.
An ACK at once that the device refuses is retried by the `flush` as an ordinary ACK, which may move the edge, and the
count of three is taken when the answer is decided, not when it leaves: under device back-pressure a sender that counts
duplicates can see fewer than three.

**Ownership is logical state.** A passive open belongs to the stack until the handshake completes and `Accepted` hands
it to the application; one that fails before then is freed silently. A connection from `connect` is the caller's at
once. From `prepare` to `commit` (with any count, zero discards) or `release`, the prepared packet is the application's:
a `close`, `abort`, received RST or time-out in between leaves the span writable, and the commit that follows drops the
packet, returns false, counts `commits_refused` and raises no retry hint, since the events already say why. A commit the
windows no longer cover is refused the same way but with a retry hint: prepare again for what `writable()` allows. After
`Closed`, `Reset`, `TimedOut` or `Unresolved` the retransmission queue is gone, the segments held above a gap go back to
the pool, and the in-order held segments stay readable until `release`, the owner's last call, which aborts a connection
that is still open and frees the slot.

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

**What is still left out, and why.** Four parts of loss recovery that Linux has are not built. Three wait for the
phase's comparison against Linux under netem, the first place a bulk sender's recovery is measured, to ask for them.
F-RTO (RFC 5682): a needless timeout is recognised by the echoed timestamp instead, on a connection that uses
timestamps, the default, as FreeBSD and VPP recognise it, and nobody but Linux ships F-RTO. Splitting an entry that a
SACK block covers in part: such a block marks nothing, the entries being the segments as they left, so part covers are
rare; a lent packet cannot be split in place, so a split would need a copy; and the cost of not splitting is bounded, a
needless resend that the peer reports and the undo gives back. Shrinking a window the application left unused (RFC 2861,
Linux's `tcp_cwnd_application_limited`): the restart after an idle RTO covers the idle case, and the rest is throughput
tuning for a bulk sender; the window no longer grows while unused in the first place. The fourth is Linux's habit of
recording the old target for the undo as at least three quarters of the old window (`tcp_current_ssthresh`), which is
not copied: the undo gives the policy back its state as the episode began, and `NewReno` takes the larger of the window
it has and the one it had, and of the two targets. Not built and not planned: receive-window auto-tuning, since the
window is what the configuration says from the handshake on; and Linux's timer for compressing the ACKs of out-of-order
arrivals and its adaptive delayed ACK, since the `flush` of the tick does the first job and the delay is one fixed
duration. Not built yet: copying small held segments into fewer packets, as Linux's `tcp_collapse` does under memory
pressure, which waits on the question of the window at the cap above, issue #39. There is no TIME_WAIT: the slot is the
owner's until `release`; it comes with the next part of step 6, the life cycle, beside keepalive and the FIN-WAIT-2
timer. An empty segment between the last ACK sent and RCV.NXT, an ACK the network delivered after the data that followed
it, is refused as out of the window and its answer limited, where Linux accepts it by testing against `rcv_wup`. The
brick makes no deployment or latency claim yet: the [tick-to-trade benchmark](../benchmarks/tick-to-trade.md) has a
recorded baseline; its reruns for loss recovery with SACK found the 64-byte tick 40 ns slower at p50 than before it,
after the cleanup of the tick path too, as the note on what an in-order ACK and a send cost says, and winning that back
is work of its own; its reruns for the first checks found the p50 unchanged, and the p99 too in the second, where the
first's 50 ns at p99 in its headline runs did not recur; its reruns with reliable send and with the receive side alone
are owed, as its page says.

**What hardening is left.** The brick is hardened against a peer off the path: its initial sequence numbers and
timestamp offsets cannot be predicted, a RST resets only at an exact position, a SYN never ends a synchronized
connection, a forged data or FIN segment needs an acknowledgement inside a bounded range, and the answers all this
draws are limited per connection. Two defences of a server are not built. It has no SYN cookies, so a SYN flood fills
the fixed table until the half-open connections time out; and the flow table's software hash is unkeyed, so chosen
4-tuples can lengthen a probe, bounded by the fixed table. Both come after phase 2, since the first deployment is a
client, and the keyed hash will take `core`'s SipHash with a key drawn the same way. The limit on challenge ACKs is per
connection only: Linux has one across connections too, per network namespace, and ships it switched off. What the IP
brick trusts of ARP is the [net page](net.md)'s.

## Where the brick departs from the RFCs and from Linux

The RFCs are the specification, and where they leave a choice Aloe does what Linux does. Loss recovery and the first
checks of a segment depart from that in the places below; the other parts' departures are in their design notes above
(the handshake's window and RTO in the note on the retransmission timer, the user timeout's count, PAWS, zero-window
probing, the answers above a gap), and ARP's are on the [net page](net.md).

Departures decided against an RFC's text, each with who decided it (where Aloe follows Linux against the letter of an
RFC is a group of its own below):

- **The initial sequence number's clock counts 64 ns from the stack's own stamp** (RFC 6528 3, a timer of 4
  microseconds), a choice made when the brick was hardened: Linux's unit, which wraps the sequence space every 275
  seconds, taken from the stack's stamp because the brick reads no clock of its own.
- **A second tail probe before the first's verdict is known** (RFC 8985 7.3, `TLP_send_probe`, and 7.4.2), by the
  author's decision. RFC 8985's procedure sends no probe while `TLP.end_seq` is set, and Linux's `tcp_send_loss_probe`
  returns while `tlp_high_seq` is. A probe that rescued a lost lone order is acknowledged exactly at its end, which
  leaves its verdict open until a later segment is acknowledged; if that later segment is a second order and is lost
  too, the RFC and Linux give it no probe and it waits for the RTO, 200 ms and more. Aloe, when the probe's timer fires
  with the earlier probe's end acknowledged and its verdict open, closes that verdict as a loss repaired, with the cut
  the next ACK past its end would have brought, and sends the new probe, whose end and verdict replace the old. It keeps
  the limit 7.3 states, "no other previous loss probe still in flight", so that the sender has at most one segment in
  flight beyond the congestion window: a probe whose end is acknowledged is not in flight. It keeps 7.3's other
  condition too, a round-trip sample since the last probe, so it needs timestamps: the rescued order's ACK gives that
  sample only through its echo, and without timestamps the second order waits for the RTO as under the RFC. What changes
  is the moment of one congestion response, at the second probe instead of the ACK after it, and one more probe on the
  wire; a first probe that was in fact needless, whose duplicate report was lost or is late, is cut for as a loss, as
  the RFC's procedure would also cut it at the next ACK past its end. `tail_probes_early` counts each use, and
  `tail_probe_recoveries` counts it among the repaired losses.
- **Without SACK, what is in flight and what an ACK delivered follow Linux's emulation** (RFC 9937 6.2; Linux's
  `tcp_add_reno_sack`, `tcp_remove_reno_sacks`, `tcp_limit_reno_sacked`), by the author's decision. The RFC says a
  sender without SACK MUST subtract one segment per duplicate ACK of the whole episode from what is in flight, at most
  RecoverFS, and count an advance as delivering the change in SND.UNA less one segment per preceding duplicate. After a
  partial ACK that count subtracts again what the advance already took out of the flight, so it under-reads what is in
  flight, and what the next advance delivered, by the segments between the holes. Aloe takes the duplicates an advance
  covers out of the count, and counts the advance as delivering at least the segment that filled the hole, as Linux
  does, in bytes.
- **`recover` starts one below the initial sequence number** (RFC 6582 3.2), a choice made while building the brick.
  RFC 6582 starts it at the initial sequence number itself, and under its own test, `ack - 1 > recover`, the loss of a
  connection's first data segment would then never be admitted to recovery; one below admits it, as Linux admits it.

Where an RFC leaves the choice and Aloe does not do what Linux does:

- **The lone segment's allowance is a setting**, `tail_probe_ack_delay`, 40 ms by default (RFC 8985 7.2, "MAY inflate
  the PTO"); Linux adds its minimum RTO, 200 ms (`tcp_schedule_loss_probe`), which puts a lone segment's probe at the
  RTO's moment. Before the first round-trip sample the allowance is added too, where the RFC's `TLP_calc_PTO` and Linux
  take the initial wait alone; no probe goes before a sample, so that wait only says when the timer gives way to the
  RTO.
- **No floor under the probe's wait or the reordering timer** beyond the wheel's tick; Linux adds 2 ms
  (`TCP_TIMEOUT_MIN_US`) to the probe for a flight of several and to `tcp_rack_mark_lost`'s wait.
- **A probe never carries new data** (RFC 8985 7.3: the sender "SHOULD transmit a previously unsent data segment, if one
  exists"): none exists, since the brick has no send buffer. A probe whose packet the device still holds is deferred and
  then copied, as every resend is; Linux skips it (`skb_still_in_host_queue`). One whose entry an ACK frees, a block
  reports, or a recovery or a timeout finds still held, or whose send is refused, was never a probe: pending until it
  leaves, it is counted, in flight and given a verdict only then.
- **A send does not replace a pending reordering timer**: the entries RACK left waiting keep their wait, which a send
  cannot settle; Linux's `tcp_schedule_loss_probe` replaces whatever timer is pending.
- **After an expiry of the probe's timer that sent no probe, the RTO keeps the moment it was last armed for**; Linux
  arms what is left of it counted from the head's last transmission (`tcp_rearm_rto`, `tcp_rto_delta_us`).
- **An expiry of the tail probe or of the reordering timer that falls on the user timeout's moment ends the
  connection**, and sends nothing; Linux reads the user timeout only at the RTO (`tcp_retransmit_timer`,
  `tcp_write_timeout`), so it sends the probe or the resend and times out at the RTO's expiry after it.
- **A reordering expiry in recovery lets one segment out**: the window in recovery becomes what is in flight plus a
  segment, so an expiry that marks the episode's next loss lets its resend go without waiting for an ACK; Linux's
  `tcp_rack_reo_timeout` steps nothing in recovery.
- **The ACK that shows reneging takes no step of the policy or of the rate reduction**; Linux returns from
  `tcp_fastretrans_alert` the same way and then still runs `tcp_cong_control`.
- **The policy hears no ACK in recovery, neither the one that ends it nor one whose undo ends it**; Linux's
  `tcp_cong_control` grows on either, its state being open by then. And **it hears the ACK that ends the loss state
  even when a new recovery begins on it**, and grows before the new episode sets its target, where Linux, finding the
  state in recovery, grows nothing.
- **An undo in the loss state opens a connection without SACK at once**, with `recover`, which the undo leaves where the
  episode set it, as the guard against duplicates its needless resends draw; Linux's `tcp_try_undo_loss` holds it in the
  loss state until the episode's data is acknowledged (`tcp_is_non_sack_preventing_reopen`).
- **In the loss state a duplicate ACK stands for nothing on a connection without SACK**; Linux counts it once new data
  was sent after the timeout, which its F-RTO sends (`tcp_process_loss`).
- **A duplicate report of the highest reported entry, when it was resent, counts as reordering**; Linux's
  `tcp_check_sack_reordering` counts one only below the highest sequence reported.
- **The undo's count of resends**: a resend counts once it has left, where Linux's `tcp_retransmit_skb` counts the
  attempt, one the busy rule defers included; a duplicate report counts one resend, where Linux counts the segments it
  covers; a tail probe counts while it is in flight, where `tcp_init_undo` counts it while its verdict is open; and the
  probe's own duplicate report, which settles its verdict, takes nothing off an episode's count, where Linux's takes it.
- **The rule on growth records the most in flight in the round**, `in_flight()`; Linux's `tcp_cwnd_validate` records
  everything outstanding, reported segments included (`packets_out`).
- **A round-trip sample is taken from the echo of a partial ACK that frees no whole entry**; Linux takes none there,
  since `tcp_clean_rtx_queue` sets no `FLAG_DATA_ACKED` for it, so an RTO after such an ACK can fall a tick away from
  Linux's.
- **RACK's minimum round trip is kept over two windows of 300 s**, coarser than Linux's filter over three estimates
  (`minmax_running_min`), and takes every sample as it is, where Linux's `tcp_update_rtt_min` passes over one a delayed
  ACK may have stretched; RFC 8985 leaves the filter open.
- **Under `NoCongestionControl` nothing holds back what a timeout marked**: the window is unbounded, so the next ACK of
  any kind after the expiry sends every marked entry at once. Linux has no such policy: its window after a timeout is
  one segment whatever the congestion control.
- **A peer whose MSS is below the floor is refused**, by the author's decision, which keeps the roadmap's text: Linux
  raises such an MSS to `tcp_min_snd_mss` and goes on (`__tcp_mtu_to_mss`), sending segments larger than the peer said
  it accepts. The note on the floor says what each open does.
- **Resends into the peer's zero window are bounded by a count of unanswered expiries**, `data_retries` in a row with no
  acceptable ACK between them, a choice made when the brick was hardened (RFC 9293's MUST-37 asks only that the
  connection stay open while the peer answers); Linux bounds them by time, 240 seconds since the last ACK it accepted
  and since the resends began (`tcp_rtx_probe0_timed_out`).
- **The sum of SND.UNA's advances, which bounds an old ACK, leaves out the SYN on both opens**, a choice made when the
  brick was hardened and found while building it: right after an active open an ACK of the initial sequence number is
  too old, dropped and challenged, where Linux, whose processing of the SYN-ACK counts the SYN in `bytes_acked`
  (`tcp_rcv_synsent_state_process`, `tcp_snd_una_update`), takes it as old. On a passive open the two agree.
- **The RST position at the end of what is held reads the highest range held**, a choice made when the brick was
  hardened: Linux reads the up-to-four blocks it is reporting (`tcp_validate_incoming`), so with more than four ranges
  held the two can differ. A peer resets from its SND.NXT, which is the end of the highest range held whether or not
  that range was among the blocks last reported.
- **The key of the initial sequence numbers is drawn once per stack**, so once per shard and process, a choice made
  when the brick was hardened; Linux draws its `net_secret` once per boot. A flow keeps one shard, and so one key, while
  the card's steering stays as it is.

Where an RFC is definite and Linux differs, Aloe follows the RFC:

- **The rate reduction's proportional part divides by the flight recorded on entry** (RFC 9937 6.2, RecoverFS); Linux's
  `tcp_cwnd_reduction` divides by the window it had.
- **The target on congestion is half of the flight** (RFC 5681 3.1, equation 4), at least two segments; Linux's reno
  halves the window (`tcp_reno_ssthresh`). This is what leaves an application-limited flow two segments after a
  recovery or a repaired probe.
- **The flight that target is computed from at the start of a recovery leaves out what was sent beyond the policy's
  window** (RFC 5681 3.2: data sent by limited transmit "MUST NOT be included"); Linux halves its window instead.
- **A connection without SACK guards the entry to recovery by `recover`** (RFC 6582 3.2): the third duplicate begins
  recovery only when `ack - 1 > recover`, to the byte; Linux holds its recovery state until something above its mark is
  acknowledged, to the same end. Where `recover` starts is in the first group above.
- **A plain duplicate ACK closes a probe's verdict as needless only when it carries no SACK option** (RFC 8985 7.4.2,
  "a DupAck without any SACK option"); Linux's test is that no block newly reported data, so a duplicate that repeats
  old blocks closes the verdict there and leaves it open here. Neither needs data in flight, as RFC 5681's duplicate
  ACK does: a lone segment's probe leaves nothing in flight once its end is acknowledged.
- **The acknowledgement of a segment the window refuses is read** (RFC 9293 3.10.7.4: "special allowance should be made
  to accept valid ACKs, URGs, and RSTs"), the roadmap's item: the *no room* outcome of the first checks. Linux drops
  such a segment whole, its acknowledgement with it, unless its receive queue is empty (`tcp_sequence`), so a peer that
  sends into a closed window while it acknowledges what it got leaves Linux's retransmission timer to fire for segments
  it has.
- **A RST below RCV.NXT is dropped in silence** (RFC 5961 3.2), a choice made when the brick was hardened; Linux, whose
  left edge is the last ACK it sent (`rcv_wup`), challenges one between that and RCV.NXT.
- **A pure ACK below RCV.NXT that fails PAWS is answered, through the limiter** (RFC 7323 5.3: send an acknowledgement
  and drop the segment), a choice made when the brick was hardened; Linux's `tcp_disordered_ack_check` drops it in
  silence.

Where Aloe follows Linux against the letter of RFC 8985:

- **The timeout marks with a reordering window of zero**: the head, and every other entry sent at least RACK's round
  trip ago (`tcp_timeout_mark_lost`); RFC 8985 6.3's `RACK_mark_losses_on_RTO` adds the reordering window.
- **RACK's round trip is that of the last entry in the ACK's walk**, which goes in sequence order, as Linux calls
  `tcp_rack_advance`; the RFC takes the entries in the order they were sent, so its round trip is the most recently
  sent one's.
- **The reordering window grows on a duplicate report only once reordering was seen** (Linux's `tcp_dsack_seen`); the
  RFC's pseudocode grows it on any.
- **Reordering is recorded at an undo by the echo only on a partial ACK** (`tcp_try_undo_partial`), not at the ACK that
  ends recovery (`tcp_try_undo_recovery`); the note on RACK above says what that costs an isolated flight.

Where Aloe follows Linux against the letter of RFC 5961 and RFC 9293, each a choice made when the brick was hardened:

- **A RST resets at two positions besides RCV.NXT** (RFC 5961 3.2): RCV.NXT - 1 in `CloseWait`, `Closing` or `LastAck`,
  since macOS resets with its FIN's number, and, on a connection that uses SACK, the end of the highest range held above
  a gap, where a peer whose earlier segments were lost resets from (`tcp_validate_incoming`, `tcp_reset_check`). Both
  are exact numbers, and the second is set only by a segment whose ACK passed the bound on old ACKs.
- **The throttle is one answer per interval per connection** (RFC 5961 7, a count per window of seconds in its
  example): one stamp shared by every limited answer, and 500 ms (`__tcp_oow_rate_limited`, `tcp_invalid_ratelimit`).
- **An empty segment exactly at the offered edge is acceptable** (RFC 9293 3.10.7.4, whose test stops below it): a peer
  that filled the window above a hole sends its ACKs and its RST from there (`tcp_sequence`).
- **A FIN's number is not held against the offered edge** (RFC 9293 3.10.7.4, which holds every number against the
  window and accepts nothing that occupies sequence space at a zero window): a FIN may stand one past the edge, and a
  bare FIN at RCV.NXT is taken at a zero window, for stacks that resend one forever there (`tcp_sequence`,
  `tcp_data_queue`).

And where the RFCs say nothing, one rule of Linux's: **a round-trip sample from a timestamp echo of zero ticks counts
as one tick, 1 ms** (`tcp_rtt_tsopt_us`). The clock cannot tell a round trip under a millisecond from none, and a zero
through RACK's minimum would hold the reordering window at zero for five minutes on the fastest paths.

Not built, where Linux has them: F-RTO (RFC 5682), shrinking a window the application left unused (RFC 2861), and
recording the old target for the undo as at least three quarters of the old window (`tcp_current_ssthresh`); the note on
what is left out says why the first two wait, and what the undo restores in place of the third.
