# Runtime Module

The runtime module (`component/runtime/`) is the shards: the unit everything above the device layer
runs in. A shard owns one queue of a device, a run queue, a cross-shard inbox, a timer wheel, a counting
scope and its counters, and runs a loop that polls the queue, fires due timers and runs ready work, all
on one thread. The module also holds the scheduler that gives senders a home on a shard, the shard-bound
coroutine task, and the runtime that launches one shard per device queue. Everything is reached through
the umbrella `#include <aloe/runtime>` (`export/aloe/runtime`), and targets link `Aloe::Component::Runtime`.
It depends on [`core`](core.md) for the execution facilities and logging, and on [`device`](device.md)
for the device concept. It never names DPDK.

## Key types

### The context and its primitives

- **`aloe::runtime::ShardContext`** -- everything of a shard that does not touch the device: the run queue,
  the inbox, the timer wheel, the task scope, the tick stamp and the counters. `run_once(now)` is its one
  verb: move the inbox onto the run queue, advance the wheel to `now`, run the work that was queued when
  the step began. `request_stop()` sets the stop flag and stops the scope; `drained()` is the loop's exit
  condition: stop requested, scope empty, both queues empty. `ShardContext::current()` names the context
  the calling thread is running.
- **`aloe::runtime::Work`** -- the intrusive node every unit of ready work is: a next pointer and a function.
  Operation states derive from it. **`RunQueue`** is the same-shard FIFO of them, with no synchronisation.
  **`Inbox`** is the multi-producer, single-consumer queue other threads push into: one atomic exchange per
  push, one load per pop, no allocation.
- **`aloe::runtime::Timer`, `TimerWheel`** -- an intrusive timer node and the hierarchical wheel it lives
  in: four levels of 256 slots, arm and cancel in constant time, swept by `advance(now)`. Deadlines are
  rounded up to the resolution, one millisecond by default, so a timer never fires early and is at most
  one resolution plus one tick late.
- **`aloe::runtime::TaskScope`** -- the per-shard counting scope, shaped like the standard's
  `counting_scope`: `spawn`, `request_stop`, `join`, with no atomics because one thread uses it. One task
  per connection will run in it. A spawned task's exception is logged and counted; it does not take the
  shard down.
- **`aloe::runtime::ShardCounters`** -- ticks, frames, work, timers and tasks, all monotonic, read on the
  shard's thread or after it has stopped.

### Scheduler and task

- **`aloe::runtime::Scheduler`** -- one pointer to a context, a value type, never type-erased. A stdexec
  scheduler with the timed shape added: `schedule()`, `schedule_after(duration)`, `schedule_at(time_point)`,
  `now()`. Same-shard `schedule()` pushes onto the run queue; from another thread it goes through the inbox.
  `spawn(sender)` starts work in the shard's scope. Not default-constructible, so a task started without a
  shard fails to compile.
- **`aloe::runtime::task<T>`** -- the C++26 coroutine task bound to a shard: `core::task<T, ShardEnvironment>`,
  where the environment names `Scheduler` as the scheduler type. Awaiting a shard sender costs nothing beyond
  the sender itself: no reschedule, no allocation. A child task awaited by a parent inherits scheduler and
  stop token.

### Shard and runtime

- **`aloe::runtime::ShardQueue<Device>`** -- one device queue as the stack sees it: `allocate()`,
  `transmit(packet)`, the device's facts. `transmit` appends to a bounded ring flushed at the end of every
  tick; when the ring is full and the device refuses, it returns false and the caller keeps the packet.
- **`aloe::runtime::IsStack`** -- what sits above a shard: a type constructed from the context and the queue
  with one function, `on_receive(span<Packet>)`, called on the shard thread with each burst. It moves out
  what it keeps; the shard frees the rest.
- **`aloe::runtime::Shard<Device, Stack>`** -- a context, a queue and the stack, with the tick: `step(now)`
  receives a burst, hands it to the stack, runs `run_once`, flushes the ring. `run()` ticks until drained.
  `ShardConfig` sets the wheel resolution, burst and ring sizes, and the idle policy, `Spin` or `Yield`.
- **`aloe::runtime::Runtime<Device, Stack>`** -- one shard per device queue, each on its own thread with an
  optional CPU to pin to and a hook that runs first (`ethdev::register_thread` for DPDK). `start`, `stop`
  from any thread, `join`; `scheduler(i)` and `spawn(i, sender)` from any thread; `counters(i)` after `join`.
  Post work between `start` and `stop`: once a shard has drained nothing reads its inbox, and work posted then
  never runs. A `start` that throws leaves the runtime stopped and unusable; destroy it.

## Usage

The whole of a stack that answers Ethernet frames, as `examples/ethernet_echo/ethernet_echo.cpp` has it:

```cpp
class EchoStack {
public:
    using Packet = aloe::ethdev::Packet;

    EchoStack(aloe::runtime::ShardContext& context, aloe::runtime::ShardQueue<aloe::ethdev::Port>& queue) noexcept
        : queue_{&queue} {}

    void on_receive(std::span<Packet> burst) noexcept {
        for (Packet& packet : burst) {
            auto data = packet.data();
            std::swap_ranges(data.begin(), data.begin() + 6, data.begin() + 6);  // destination <-> source
            std::ignore = queue_->transmit(std::move(packet));
        }
    }

private:
    aloe::runtime::ShardQueue<aloe::ethdev::Port>* queue_;
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
   except `Inbox::push`, and therefore `schedule()` from another thread, `Runtime::stop` and `Runtime::spawn`.
2. A stop token handed to an operation on a shard is requested on that shard. The scope's stop source
   satisfies this because `Runtime::stop` arrives through the inbox.
3. Operations complete on the shard that owns them, so the receiver runs there too.
4. A shard task is affine to its shard. The task awaits every sender directly, with no reschedule, so it
   awaits only its own shard's senders and child tasks; another shard's `schedule()` is for operation
   states and `Runtime::spawn`. The scope asserts in debug builds that a task completes on the thread
   that spawned it.

## Design notes

**A direct call, not a receive sender.** The layer above gets frames by one call from the loop, run to
completion, and transmits by one call into the queue. A receive sender completing with a burst would put an
operation state and stdexec on the per-frame path. Senders begin one layer up, at the connection, where a
parked operation is worth an operation state.

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

**Logging behind an alias.** Shards log only on cold paths, through [`core`](core.md)'s quill alias: a shard
starting and draining, a task failing, a hook or pin failing. Nothing logs inside a tick.
