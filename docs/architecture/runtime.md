# Runtime Module

The runtime module (`common/runtime/`) is the loop written for you: Aloe's second product, built only
from the [`loop`](loop.md) bricks. A shard owns one queue of a device and a context with the run queue, the
inbox, the timer wheel and a counting scope, and runs the tick that polls the queue, hands the burst to the
stack, fires due timers and runs ready work, all on one thread. The module also holds the scheduler that
gives senders a home on a shard, the shard-bound coroutine task, and the runtime that launches one shard per
device queue on its own pinned thread. Everything is reached through the umbrella `#include <aloe/runtime>`
(`export/aloe/runtime`), which includes `<aloe/loop>`, and targets link `Aloe::Common::Runtime`. It
depends on [`execution`](execution.md) for the execution facilities, on [`log`](log.md) for logging, on
[`core`](core.md) for the clock, on [`loop`](loop.md) for the bricks,
on [`device`](device.md) for the device concept, and on [`tcp`](tcp.md), [`stream`](stream.md) and
[`net`](net.md) for the connections its stream senders wait on. It never names DPDK.

Nothing here is required to use Aloe. A program that wants the loop under its own control writes it over
the bricks, as the [loop page](loop.md) shows, and keeps every line of protocol code. The runtime adds
threads, the drain protocol and senders for the code that waits, and pays for them per awaited event, never
per packet.

## Key types

### The context

- **`aloe::runtime::ShardContext`** -- everything of a shard that does not touch the device: the run queue,
  the inbox, the timer wheel and the counters from [`loop`](loop.md), the task scope, and the tick stamp.
  `run_once(now)` is its one verb: move the inbox onto the run queue, advance the wheel to `now`, run the
  work that was queued when the step began. `set_now(now)` records the step's stamp before any stack
  callback and runs nothing. `request_stop()` sets the stop flag and stops the scope; `drained()` observes
  whether anything is left (stop requested, scope empty, both queues empty) and changes nothing.
  `try_finish()` is the loop's exit: false without a stop request, otherwise it rechecks `drained()` under
  the control lock and closes control admission in the same critical section. `siblings()` lists every
  other context of the runtime, bound before any thread starts and empty for a standalone shard.
  `ShardContext::current()` names the context the calling thread is running.
- **Control posts.** `post_control(work)` is the guarded way to hand a cold-path node to another shard:
  callable from any thread, it takes a mutex that only this path and `try_finish` use. True means the node
  was accepted and will run exactly once on the target; false means the target has closed admission and
  the node is still the caller's to free or reuse. A node is never posted to an inbox nobody will read.
  The ordinary `Inbox::push`, and so the scheduler's cross-shard `schedule()`, keeps its contract and takes
  no lock.
- **ARP sink.** `set_arp_sink({object, learn})` registers, at setup and teardown, where a resolution learned
  by another shard goes; `deliver_arp(address, mac)` hands it over on the owning thread with the current
  stamp. The sink names only `wire` values, no device and no `net` type. A default sink discards.
- **`aloe::runtime::TaskScope`** -- the per-shard counting scope, shaped like the standard's
  `counting_scope`: `spawn`, `request_stop`, `join`, with no atomics because one thread uses it. With
  `TcpStack`, one task per connection runs in it. A spawned task's exception is logged and counted; it does not take the
  shard down.

### Scheduler and task

- **`aloe::runtime::Scheduler`** -- one pointer to a context, a value type, never type-erased. A stdexec
  scheduler with the timed shape added: `schedule()`, `schedule_after(duration)`, `schedule_at(time_point)`,
  `now()`. Same-shard `schedule()` pushes onto the run queue; from another thread it goes through the inbox.
  `spawn(sender)` starts work in the shard's scope. Not default-constructible, so a task started without a
  shard fails to compile.
- **`aloe::runtime::task<T>`** -- the C++26 coroutine task bound to a shard: `execution::task<T, ShardEnvironment>`,
  where the environment names `Scheduler` as the scheduler type. Awaiting a shard sender costs nothing beyond
  the sender itself: no reschedule, no allocation. A child task awaited by a parent inherits scheduler and
  stop token.

### Shard and runtime

- **`aloe::runtime::IsStack`** -- what sits above a shard: a type constructed from the context and a
  `loop::ShardQueue<Device>` with one function, `on_receive(span<Packet>)`, called on the shard thread with
  each burst. It moves out what it keeps; the shard frees the rest.
- **`aloe::runtime::Shard<Device, Stack>`** -- a context, a queue and the stack, with the tick. `step(now)`
  sets the stamp, receives a burst and hands it to `on_receive`, calls the stack's `on_tick(now)` if it has
  one, flushes the ring early, runs `run_once`, calls `on_flush(now)` if the stack has one, and flushes
  again. The two hooks are optional and detected independently; a stack with neither keeps the plain tick.
  `run()` ticks until `try_finish()` succeeds. `ShardConfig` sets the wheel resolution, burst and ring
  sizes, and the idle policy, `Spin` or `Yield`.
- **`aloe::runtime::Runtime<Device, Stack>`** -- one shard per device queue, each on its own thread with an
  optional CPU to pin to and a hook that runs first (`ethdev::register_thread` for DPDK). `start`, `stop`
  from any thread, `join`; `scheduler(i)` and `spawn(i, sender)` from any thread; `counters(i)` after `join`.
  Every context's siblings are bound in the constructor. `stop` posts one owned stop node per shard through
  `post_control`; a shard that already finished rejects it and the runtime keeps the node. A thread whose
  hook fails never polls its device: it requests stop on its context, runs the control posts already
  accepted and closes admission before it reports the failure.
  Post work between `start` and `stop`: once a shard has drained nothing reads its inbox, and work posted then
  never runs. A `start` that throws leaves the runtime stopped and unusable; destroy it.

### Streams

- **`aloe::runtime::Streams<Stack>`** -- the runtime's side of a `tcp::Stack`, constructed over the stack and
  the shard's context with every slot allocated there: one wait slot per connection, and one listener record
  per listener of the stack's config, each with a backlog ring as long as the connection table. `wake(now)`
  drains the brick's `poll_event()` and, per flag, detaches the matching parked wait and pushes it onto the
  run queue. `accept(port)` and `connect(peer)` return senders that complete with a `Stream`. `cancel(index)`
  requests the slot's stop source and aborts the connection; `deadline(index, when)` arms the slot's one
  timer, whose fire is `cancel`; `release(index)` is the owner's last call. `tcp()` and `context()` reach what
  it was built over.
- **`aloe::runtime::Stream<Stack>`** -- a move-only owner of one connection. The synchronous members
  (`unread`, `consume`, `peer_closed`, `writable()`, `prepare`, `commit`, `committed`, `acknowledged`,
  `unacknowledged`, `events`, `state`, `local`, `remote`, `mss`) forward to the brick's connection. The
  senders are `readable(n)`, `writable(n)`, `acked(sequence)`, `closed()`, `send(bytes)` and `close()`.
  `cancel()`, `deadline(when)` and `release()` go through the owner, and the destructor releases, so a task
  that returns for any reason frees its slot. A moved-from or released handle is inert.
- **`connection()` versus the senders.** On the handle, `send(bytes)` and `close()` are senders and
  `writable()` is the byte query while `writable(n)` is a sender. The brick's synchronous `send` and `close`
  are reached through `connection()`. The brick's connection models `stream::IsStream`; the handle does not,
  by design, because its same-named members wait.
- **All lifecycle calls go through the owner.** A connection managed by `Streams` is cancelled, given a
  deadline and released through `Streams` or its handle, never through the brick directly, so the wait slot
  is cleaned in the same call that frees the connection.

### The ready-made TCP stack

- **`aloe::runtime::TcpStack<Device>`** -- the IP brick, the TCP brick and the stream owner composed for one
  shard, with the tick glue; it models `IsStack` with both hooks. It is constructed by the shard from the
  context, the queue, a `net::Ipv4Config` and a `tcp::TcpConfig`, and names its parts `Ip`
  (`net::Ipv4<Device>`), `Tcp` (`tcp::Stack<Ip>`), `StreamsType` (`Streams<Tcp>`) and `StreamType`
  (`Stream<Tcp>`); `ip()`, `tcp()` and `streams()` reach them. The members are declared IP, TCP, Streams, so
  they are destroyed in the reverse order, and the destructor removes the ARP sink before any of them goes.
  On a device with more than one queue it copies the IP config and sets `accept_unsolicited_replies`, so
  queue 0 learns the replies its siblings solicited. `forwards_dropped()` counts the forwards that could
  not be allocated and `forwards_rejected()` those a finished sibling refused.
- **Its tick.** `on_receive(burst)` runs IP then TCP at the context's stamp, then one forwarding pass over
  `ip.resolved()`. `on_tick(now)` runs an empty TCP `process`, which publishes retry hints, then the wake
  pass. `on_flush(now)` runs the wake pass again, for the events timers and tasks raised, then TCP's
  `flush`. A program that wants another composition writes its own `IsStack` over the same bricks.
- **ARP forwarding.** Each resolution in `ip.resolved()` goes to every sibling once, after the `process`
  that reported it; an empty tick forwards nothing. The sibling applies it through its ARP sink, which calls
  `ip.learn(address, mac, now)` on its own thread and reports nothing, so shards never echo each other. One
  forward is one `detail::ArpForwardWork` node per sibling, allocated with `std::nothrow` on that cold path
  and posted with `post_control`. An accepted node runs once on the target, delivers through `deliver_arp`
  and frees itself; a rejected node, whose target has already finished, is freed by the sender at once and
  counted in `forwards_rejected()`; a node that cannot be allocated is counted in `forwards_dropped()`, and
  the sibling resolves the address itself when it needs it.

## Usage

The whole of a stack that answers Ethernet frames, as `examples/ethernet_echo/ethernet_echo.cpp` has it:

```cpp
class EchoStack {
public:
    using Packet = aloe::ethdev::Packet;

    EchoStack(aloe::runtime::ShardContext& context, aloe::loop::ShardQueue<aloe::ethdev::Port>& queue) noexcept
        : queue_{&queue} {}

    void on_receive(std::span<Packet> burst) noexcept {
        for (Packet& packet : burst) {
            auto data = packet.data();
            std::swap_ranges(data.begin(), data.begin() + 6, data.begin() + 6);  // destination <-> source
            std::ignore = queue_->transmit(std::move(packet));
        }
    }

private:
    aloe::loop::ShardQueue<aloe::ethdev::Port>* queue_;
};

aloe::ethdev::Port port{{.name = "net_tap0", .queues = 4}};
aloe::runtime::Runtime<aloe::ethdev::Port, EchoStack> runtime{
    {.shard = {}, .threads = {}, .thread_hook = aloe::ethdev::register_thread}, port};
runtime.start();
```

Work on a shard is a task spawned through its scheduler. Timers are senders of that scheduler:

```cpp
aloe::runtime::task<void> heartbeat(aloe::runtime::Scheduler scheduler) {
    for (int beat = 0; beat < 10; ++beat) {
        co_await scheduler.schedule_after(std::chrono::milliseconds{100});
    }
}

runtime.spawn(0, heartbeat(runtime.scheduler(0)));  // from any thread: hops to shard 0 and spawns there
runtime.stop();                                     // every task parked on a timer completes stopped
runtime.join();
```

A TCP echo server over `TcpStack`, one task per connection, on every queue of a port:

```cpp
using Stack = aloe::runtime::TcpStack<aloe::ethdev::Port>;

aloe::runtime::task<void> echo(Stack::StreamType stream) {
    while (co_await stream.readable(1)) {
        const auto chunk = stream.unread().front();
        if (!co_await stream.send(chunk)) {
            co_return;
        }
        stream.consume(chunk.size());
    }
    std::ignore = co_await stream.close();
}

aloe::runtime::task<void> serve(Stack::StreamsType* streams, aloe::runtime::Scheduler scheduler) {
    while (auto stream = co_await streams->accept(7)) {
        scheduler.spawn(echo(std::move(*stream)));
    }
}

aloe::runtime::Runtime<aloe::ethdev::Port, Stack> runtime{
    {.shard = {}, .threads = {}, .thread_hook = aloe::ethdev::register_thread}, port,
    aloe::net::Ipv4Config{.address = {10, 0, 0, 2}, .prefix = 24, .gateway = aloe::wire::Ipv4Address{10, 0, 0, 1}},
    aloe::tcp::TcpConfig{}};
runtime.start();
for (std::uint16_t index = 0; index < runtime.shard_count(); ++index) {
    runtime.spawn(index, serve(&runtime.shard(index).stack().streams(), runtime.scheduler(index)));
}
```

`runtime.shard(index)` is read here before the shard has run anything that touches its streams; the task
itself runs on the shard. A task awaits only its own shard's senders. The echo awaits `send(chunk)` before it
consumes, because the sender copies from `chunk` until every byte is committed; the consume then moves the
window's edge after the segment has left, which costs one window update per echoed segment at the flush. An
echo that wants the reopening to ride on its data writes through the handle's synchronous members instead:
`prepare`, copy, `consume`, `commit`. `examples/tcp_echo_tasks` is this server, with the client beside it.

## Threading contract

Four rules make the data path lock-free; debug builds assert them.

1. Every member of a context, its scheduler's senders, its scope and its wheel runs on the owning thread,
   except `Inbox::push` and `post_control`, and therefore `schedule()` from another thread, `Runtime::stop`
   and `Runtime::spawn`.
2. A stop token handed to an operation on a shard is requested on that shard. The scope's stop source
   satisfies this because `Runtime::stop` arrives through the inbox, posted with `post_control`.
3. Operations complete on the shard that owns them, so the receiver runs there too.
4. A shard task is affine to its shard. The task awaits every sender directly, with no reschedule, so it
   awaits only its own shard's senders and child tasks; another shard's `schedule()` is for operation
   states and `Runtime::spawn`. The scope asserts in debug builds that a task completes on the thread
   that spawned it.

## The tick and its hooks

A stack with the hooks sees one step in this order:

1. `set_now(now)`: every callback of the step reads the same stamp through the context.
2. Receive and `on_receive(burst)`.
3. `on_tick(now)`: drain the events packets raised, publish retry hints.
4. An early flush, so control segments and immediate ACKs leave before any task runs.
5. `run_once(now)`: the inbox, due timers and one chain of ready work. Work queued while the chain runs
   waits for the next step; there is no recursive drain.
6. `on_flush(now)`: drain the events timers and tasks raised, emit the remaining ordinary ACKs.
7. The final flush.

An event raised inside `run_once`, by a timer or a task, is drained by `on_flush`, and the waiter it wakes
is pushed onto the run queue, so its completion runs on the next step, whether or not traffic arrives.

## Stream senders and wait slots

Each sender is a concrete operation state over one wait node, with no allocation and no type erasure. A
connection's slot holds one parked operation per kind (readable, writable, acked, connected, closed; debug
builds assert it), a list of its active waits, a stop source, the terminal error once one is seen, and the
deadline timer. A listener holds one parked accept. `send(bytes)` parks on the writable kind, so a
`send(bytes)` and a `writable(n)` do not wait on one connection at once, and `close()` and `closed()` share
the closed kind the same way.

A sender checks the level when it starts: data already unread, credit already open or a terminal state
already seen completes it inline, without parking and without a run-queue push. Otherwise it parks. `wake`
runs after each phase that may raise events, drains the events and, for each flag, takes the parked wait of
the matching kind, saves its outcome on the node and pushes it onto the run queue. The completion runs inside
`run_once`. A sender's attributes say so: it completes on its shard's scheduler, and it may complete inline
from `start`, so it answers `asynchronous_affine | inline_completion`, not the timer senders' plain
`asynchronous_affine`. A level read at start also covers a terminal event raised but not yet drained: a
`closed()` started right after a retransmit timer timed the connection out inside `run_once` reports
`TimedOut`, not a normal close. No receiver is ever called from a wake pass, and processing a later burst
never consumes an event: the drained flags become outcomes saved on nodes, and the levels are read again when a wait starts.

Values travel in the value channel as `std::expected<T, stream::Error>`. Cancellation, deadlines and scope
shutdown use the stopped channel. An operation registers two stop callbacks: one on its receiver's token,
which is how scope shutdown reaches it, and one on its slot's stop source, which is how `cancel` and the
deadline reach every wait of the connection. An operation started after `cancel` completes stopped at once.

The lifetime rules that keep a slot safe to reuse:

- A wait is parked on its slot's kind pointer and linked on the slot's active list.
- A wake, or a stop request, detaches the parked pointer before queueing, so a second wake cannot queue it
  twice.
- A queued completion stays on the active list until it runs, so `release` can turn its saved outcome into
  stopped and unlink it.
- `release` drops every callback of every active wait, queues the parked ones stopped, clears the slot, then
  rebuilds the slot's stop source, so no callback is ever left on a source that is replaced.
- A queued completion that runs after its slot was released and reused uses only its own outcome and
  receiver; it never touches the slot again.

A `connect` operation owns its pending connection until it hands it over: a refused, timed-out or stopped
open releases the slot before it completes. An accept that was queued with a connection and then stopped
gives that connection back to the brick. A reset before `Connected` reports `Refused`; after it, `Reset`.

## Design notes

**Built on the bricks, never beside them.** Every primitive the runtime drives is a `loop` type a
hand-written loop drives the same way, and the step is that loop with the stack call and `run_once` in it.
A capability lands in the bricks first, as a plain call or an event a caller drains, and the runtime wraps
it in a sender afterwards; the runtime never has a capability the bricks lack.

**A direct call, not a receive sender.** The layer above gets frames by one call from the loop, run to
completion, and transmits by one call into the queue. A receive sender completing with a burst would put an
operation state and stdexec on the per-frame path. Senders begin one layer up, at the connection, where a
parked operation is worth an operation state, and in phase 1 they park in wait slots the runtime keeps and
wake from the event list the TCP brick reports.

**A device-free context.** The scheduler points at a `ShardContext`, not at a `Shard<Device, Stack>`, so the
scheduler, the timer senders and the task are one concrete type for every backend, and tasks never carry a
device type in their signatures. The context is tested without a device, and time is a stamp the loop passes
in, so a test drives the wheel and the drain with any clock it likes.

**One wheel per shard, never a timer per operation.** Timers are intrusive nodes swept once per tick. TCP
arms and re-arms them on every segment, which is why arm and cancel are constant-time and allocation-free.

**The task environment.** stdexec's P3552 task takes its scheduler type from its environment. With the
default environment the scheduler is type-erased and every await may reschedule through it. `ShardEnvironment`
names the concrete `Scheduler`, whose senders say they complete where they start, so the task awaits them
directly. The switch applies to every awaited sender, which is why threading contract rule 4 exists. A test
pins the saving: a task awaiting a timer costs one wheel entry and no run-queue push.

**Admission closes once, with the drain check.** A producer on another shard must know whether its node
was taken, or it either leaks the node or frees one a shard will still run. `post_control` and `try_finish`
share one mutex: a post that wins the lock before the final check leaves the inbox non-empty, so the check
fails and the node runs; a post that loses finds admission closed and keeps the node. `drained()` stays a
query, so nothing else can close a context by looking at it. The mutex is on the cold path only; packets,
timers, tasks and the scheduler's `schedule()` never take it.

**A forward node is owned by exactly one side.** A resolution is sent to a sibling in a node allocated for
it. Either the target accepts the node and runs it, and the run frees it, or the target has closed admission,
`post_control` returns false, and the sender frees it on the spot. No node is posted to an inbox nobody will
read, and stop racing a burst of forwards leaks nothing. The allocation is `std::nothrow` because it runs
inside `on_receive`, which must not throw; a failed one costs the sibling one ARP round trip later.

**Logging behind an alias.** Shards log only on cold paths, through [`log`](log.md)'s quill alias: a shard
starting and draining, a task failing, a hook or pin failing. Nothing logs inside a tick.
