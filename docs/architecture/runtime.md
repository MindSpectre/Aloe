# Runtime Module

The runtime module (`common/runtime/`) is the loop written for you: Aloe's second product, built only
from the [`loop`](loop.md) bricks. A shard owns one queue of a device and a context with the run queue, the
inbox, the timer wheel and a counting scope, and runs the tick that polls the queue, hands the burst to the
stack, fires due timers and runs ready work, all on one thread. The module also holds the scheduler that
gives senders a home on a shard, the shard-bound coroutine task, and the runtime that launches one shard per
device queue on its own pinned thread. Everything is reached through the umbrella `#include <aloe/runtime>`
(`export/aloe/runtime`), which includes `<aloe/loop>`, and targets link `Aloe::Common::Runtime`. It
depends on [`execution`](execution.md) for the execution facilities, on [`log`](log.md) for logging, on
[`core`](core.md) for the clock, on [`loop`](loop.md) for the bricks
and on [`device`](device.md) for the device concept. It never names DPDK.

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
  `counting_scope`: `spawn`, `request_stop`, `join`, with no atomics because one thread uses it. One task
  per connection will run in it. A spawned task's exception is logged and counted; it does not take the
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

**Logging behind an alias.** Shards log only on cold paths, through [`log`](log.md)'s quill alias: a shard
starting and draining, a task failing, a hook or pin failing. Nothing logs inside a tick.
