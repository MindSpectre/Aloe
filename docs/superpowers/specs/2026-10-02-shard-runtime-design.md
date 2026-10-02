# Shard runtime

Status: approved by the author on 2026-10-02 ("write the plan"), amended the same day
with the spot-check findings listed under "Findings from the spot checks". The plan is
`docs/superpowers/plans/2026-10-02-shard-runtime.md`. Spec 2 of the four that lead to
minimal TCP, and the last piece of roadmap phase 0. Tracks GitHub issue #4
`[RUNTIME]`. The device layer (spec 1, PRs #2 and #3) is merged on `main` and is what
this builds on. Everything under "Open questions" is left for the plan or for a later
phase.

## Goal

The shards that run everything above the device layer: one shard per core over one
queue pair of an `IsDevice`, a run loop the stack owns, a concrete per-shard scheduler
for senders, a timer wheel with a timer sender on top, a counting scope for graceful
drain, a coroutine task type bound to the shard scheduler, stop plumbing, counters
and logging. The layer above sees a shard as "received frames come in here, frames to
transmit go out there, and here is the scheduler", and nothing device-specific.

## Done when

1. Tasks and timers run on shards, and work can be submitted onto a shard from any
   thread. The cross-shard paths, the inbox and the cross-shard steering test, pass
   under the `tsan` preset.
2. An Ethernet echo runs on several shards on both backends: on the fabric with
   receive-side scaling, where every frame lands on the shard `queue_for` predicts
   and the test checks it; on `net_ring` with one ring per queue in CI; on a real
   card by hand through the example program.
3. A task that awaits a shard timer costs one wheel entry and no run-queue push,
   measured by the shard counters in a test. This is the proof that the concrete
   scheduler keeps stdexec's task from rescheduling.
4. Tests still need no root, no hugepages and no network card, and `debug`,
   `gcc-debug`, `asan` and `tsan` pass, with the format check.
5. `docs/architecture/runtime.md` exists, the overview and roadmap are updated, and
   the README and AGENTS.md list the module.

## Context

What exists and is relied on:

- `aloe::device::IsDevice`: queue `i` is used from one thread; `receive(queue, span)`
  fills from the front and returns the count; `transmit(queue, span)` accepts a
  prefix, moves from the accepted packets and leaves the rest untouched; `allocate`
  returns an optional packet; `counters(queue)` is read from the queue's thread. The
  hot path never throws and allocates nothing but packets.
- `aloe::fabric::Port` and `aloe::ethdev::Port` model it. Ethdev moves at most
  `Port::max_burst` (64) packets per call. The ring driver loops a queue's transmit
  back into the same queue's receive and has no RSS; the null driver has RSS and
  produces frames on receive by itself. The EAL starts once per process; tests use
  `aloe::testing::EalEnvironment` and `probe_vdev` for fresh virtual devices.
- `aloe::core::ex` is the one alias for stdexec, and `aloe::core::task<T>` is today an
  alias for `exec::task`. This spec changes the latter, see "Core additions".
- `aloe::utils` holds dependency-free helpers for headers that must not include
  `<aloe/core>`.
- The vcpkg-pinned stdexec (port version-date 2026-05-25) ships the standard-track
  `stdexec::task<T, Environment>` from P3552, `stdexec::counting_scope` from P3149, and
  the `affine` algorithm that the task applies to every awaited sender. Details under
  "Verified facts".

## Decisions

Made in the design session of 2026-10-02, with the alternatives that lost.

| Decision                        | Chosen                                                                                                                                                                                            | Rejected                                                                                                                                                                                                                                     |
|---------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Where the runtime lives         | A new module `component/runtime`, namespace `aloe::runtime`, depending on `core` and `device`.                                                                                                    | Growing `common/core`, which would make a foundation module depend on a component; splitting scheduler and shard across two modules, which cuts one unit in half.                                                                            |
| How the layer above gets frames | A direct call into a stack policy, `on_receive(span<Packet>)`, from the loop, run to completion. Senders begin at the connection seam in the TCP spec.                                            | A receive sender completing with a burst: an operation state per burst and stdexec on the per-frame path.                                                                                                                                    |
| Logging                         | quill through an alias in `core`, the way stdexec is aliased, so the library can be swapped or replaced by our own later.                                                                         | A minimal own facility; spdlog; porting Crow from Menagerie.                                                                                                                                                                                 |
| Shape of the runtime            | A device-free `ShardContext` that owns the scheduler's state, and a thin `Shard<Device, Stack>` that adds one queue and the loop. The scheduler and task are one concrete type for every backend. | One shard template holding everything, which puts the device type into every task signature; assembling from `exec::run_loop`, `async_scope` and `timed_thread_scheduler`, which take mutexes, atomics and an extra thread on the data path. |
| Task type                       | `stdexec::task<T, Env>` from P3552 with an environment that names the concrete shard scheduler.                                                                                                   | `exec::task`, whose default context stores a type-erased scheduler and reschedules through it; `exec::basic_task`, which has no standard counterpart.                                                                                        |
| Thread model                    | The runtime launches `std::jthread` per shard with optional pinning and one hook per thread; ethdev supplies the hook that registers the thread with DPDK.                                        | Launching on EAL worker lcores, which needs DPDK inside the runtime and a second launch path for the fabric.                                                                                                                                 |
| Clock                           | The loop reads `std::chrono::steady_clock` once per tick and passes the stamp into the context.                                                                                                   | A clock template parameter or interface; tests pass stamps instead.                                                                                                                                                                          |

## Module layout

`component/runtime/`, target `Aloe.Component.Runtime` with alias
`Aloe::Component::Runtime`, umbrella `<aloe/runtime>`. Links `Aloe::Common::Core`,
`Aloe::Common::Utils` and `Aloe::Component::Device`. It is the first module that
includes `<aloe/core>`, which the rules allow for the runtime. Non-template parts (run queue, inbox, wheel, scope,
context) compile in `.cpp` files; the shard and the
runtime are templates.

| Header              | Holds                                                                     |
|---------------------|---------------------------------------------------------------------------|
| `work.hpp`          | `Work`: the intrusive node every unit of ready work is.                   |
| `run_queue.hpp`     | `RunQueue`: same-shard FIFO of `Work`.                                    |
| `inbox.hpp`         | `Inbox`: multi-producer single-consumer intrusive queue of `Work`.        |
| `timer_wheel.hpp`   | `Timer` node and `TimerWheel`.                                            |
| `task_scope.hpp`    | `TaskScope`: the per-shard counting scope and its stop source.            |
| `shard_context.hpp` | `ShardContext`: the five above, the tick stamp, the counters, `run_once`. |
| `scheduler.hpp`     | `Scheduler` and its three senders.                                        |
| `shard_task.hpp`    | `ShardEnvironment` and `task<T>`.                                         |
| `shard_queue.hpp`   | `ShardQueue<Device>`: one device queue as the stack sees it.              |
| `shard.hpp`         | `IsStack`, `ShardConfig`, `Shard<Device, Stack>`.                         |
| `runtime.hpp`       | `RuntimeConfig`, `Runtime<Device, Stack>`, `RuntimeError`.                |
| `counters.hpp`      | `ShardCounters`.                                                          |

Header basenames stay unique across the repository. `task_scope.hpp` and
`shard_task.hpp` are named so as not to collide, in readers' minds, with stdexec's
`exec/scope.hpp` and `exec/task.hpp`.

### Core additions

`common/core/execution/execution.hpp` changes:

- `task<T, Env = ex::env<>>` aliases `stdexec::task`, the P3552 task. `exec::task` is no
  longer named anywhere; the `<exec/task.hpp>` include goes.
- `TaskEnvironment<Scheduler, StopSource, Errors>` is a struct template that spells
  the environment members the way the current implementation wants them. Today
  stdexec says `start_scheduler_type` where P3552 says `scheduler_type`; that
  spelling lives only here, like the namespace alias.
- `completion_behavior` and `get_completion_behavior`, the names a sender uses to
  advertise that it completes on its scheduler, aliased from
  `<exec/completion_behavior.hpp>`. If the prototype finds that customising `affine`
  through the scheduler's domain is the mechanism that works instead, the alias
  changes to that and this spec is amended. No other file names `exec::`.

`common/core/log/log.hpp` is new, the one header that names quill:

- `LogLevel`: `Trace, Debug, Info, Warning, Error, Critical, None`.
- `Logging`: constructed once by the application or the test environment. Starts the
  quill backend thread, creates the console sink, sets the default level. Its config
  carries the backend thread's CPU affinity, so the backend never lands on a shard
  core, and the level. The destructor flushes and stops the backend.
- `Logger`: a small handle, obtained from `logger(std::string_view name)`, for
  instance `"aloe.runtime"`.
- `Logger::log<Level, "format">(args...)` plus per-level members,
  `log.info<"format">(args...)` and friends, `set_level` and `flush`. The format string
  is a `utils::FixedString` template parameter, so each call site owns one
  `static constexpr` metadata object as quill's macros would, and the public header
  defines no macro. Source location is not recorded; the logger name says where a
  line came from. Calls are cold-path only by convention; nothing in the runtime
  logs inside a tick.
- `LoggingConfig`: the level, the backend thread's CPU, and an optional file to write
  instead of the console, which is how the logging test reads what was logged.

`utils` gains `fixed_string.hpp` with `FixedString<N>`, a structural type usable as a
template parameter. Core links `Aloe::Common::Utils`.

`vcpkg.json` adds `quill`. `common/core/CMakeLists.txt` adds a `Core.Log` interface
library linking `quill::quill`, folded into the combined core library.

### Ethdev addition

`aloe::ethdev::register_thread()` in `eal/eal.hpp`: wraps `rte_thread_register` and
throws `EthdevError` on failure. Passed as the runtime's thread hook, it gives each
shard thread an lcore id so mempool per-lcore caches work. Threads are not
unregistered at exit; shards live as long as the process, and DPDK keeps the slot.
This is the only new DPDK call.

## The shard context

`ShardContext` is everything of a shard that does not touch the device. It is owned
by one thread for its whole life, and every member function except `Inbox::push` is
called on that thread. A thread-local pointer, `ShardContext::current()`, names the
context the calling thread is running; the loop sets it for the duration of a tick,
and the scheduler reads it to tell same-shard from cross-shard.

### Work and the run queue

```cpp
struct Work {
    using Function = void (*)(Work&) noexcept;
    std::atomic<Work*> next{nullptr};
    Function run = nullptr;
};
```

Operation states derive from `Work` and set `run`. The same node type serves both
queues: the inbox uses acquire and release on `next`, the run queue uses relaxed
operations, which are plain loads and stores on every supported target, so the
same-shard path pays no fence. `RunQueue` is a singly linked FIFO with head and tail:
`push(Work&)` and `take()`, which detaches the whole chain and leaves the queue empty.

### Inbox

An intrusive multi-producer single-consumer queue (Vyukov's), one atomic exchange
per `push` from any thread, one acquire load per `pop` on the owning thread, no
allocation, wait-free for producers. A node pushed from another thread stays alive
until the shard runs it; the pusher's operation state is the node, so that holds by
construction. `pop` can return nothing while a producer is between its exchange and
its link store; the node is not lost, the shard sees it on the next step. `empty()`
is true only when no node is queued or in flight, which is what the drain check
needs.

### Timer wheel

A hierarchical wheel, four levels of 256 slots, resolution set at construction, one
millisecond by default. Four levels at one millisecond cover about fifty days before a
timer has to wait at the horizon, so nothing TCP arms is ever far out. Memory is 1024 slot heads, 16 KB.

```cpp
struct Timer {
    Timer* next = nullptr;
    Timer* prev = nullptr;
    std::chrono::steady_clock::time_point deadline{};
    void (*fire)(Timer&) noexcept = nullptr;
};
```

- `arm(Timer&, time_point)`: re-arming an armed timer cancels it first. The deadline
  is rounded up to the resolution, so a timer never fires early.
- `cancel(Timer&)`: constant time through the doubly linked slot list; a no-op on an
  unarmed timer.
- `advance(time_point now)`: fires every timer whose rounded deadline is at or before
  `now`, in slot order, timers in one slot in arm order, cascading higher levels down
  as the lower ones wrap. A timer fires on the first `advance` whose stamp reaches it,
  so lateness is bounded by one resolution plus the gap between ticks.
- `pending()` for tests and counters; `Timer::armed()` on the node.
- A deadline more than 2^32 ticks out is placed at the horizon and re-placed at each
  cascade until its real tick is within reach, so it fires at its deadline, late by
  nothing more than any other timer.

The node owner keeps the node alive while armed, or cancels it first; the destructor
of an armed `Timer` asserts in debug builds.

### Task scope

`TaskScope` is shaped like the standard's `counting_scope`, `spawn` and `join`,
without atomics because one thread uses it.

- `spawn(Sender&&, Env)`: shard thread only. Allocates the operation state, the one
  allocation per task until the arena arrives in phase 1, connects it to a receiver
  whose environment answers `get_stop_token` with the scope's token and forwards
  every other query to `Env`, and starts it. The scope knows nothing of schedulers;
  `Scheduler::spawn(sender)` is what code calls, and it passes an environment that
  answers `get_scheduler` and `get_start_scheduler` with the shard's `Scheduler`.
  The allocator of phase 1 arrives the same way, as a query of that environment. On completion the
  state is freed and counted: value, stopped, or error. An error completion is logged
  at `Error` with the exception text, counted as failed, and does not take the shard
  down.
- `request_stop()`: trips the scope's `inplace_stop_source`. Every spawn afterwards
  starts with stop already requested.
- `size()`, `empty()`, `stop_token()`.
- `join()`: a sender that completes when the scope becomes empty. The shard's drain
  reads `empty()` directly; `join` is for tests and for code that wants to wait inside
  a task.

### run_once

```cpp
bool run_once(std::chrono::steady_clock::time_point now) noexcept;
```

In order: record `now` as the tick stamp; move everything the inbox holds onto the
run queue, in arrival order; `advance` the wheel to `now`; take the run queue's chain
and run it. Work pushed while the chain runs waits for the next step, so one step is
bounded and the device poll between steps is never starved. Returns whether anything
ran, for the idle policy.

Stop is a `Work` that arrives through the inbox and, on the shard, sets a plain flag
and calls `scope.request_stop()`. The loop reads the flag, never an atomic.
`drained()` is true when stop was requested, the scope is empty and both queues are
empty. Armed timers do not hold a shard open: a timer with no task behind it belongs
to the stack and dies with it.

### Threading contract

Written once here, repeated in `runtime.md`, asserted in debug builds:

1. Every member function of a context, its scheduler's senders, its scope and its
   wheel runs on the owning thread, except `Inbox::push` and therefore `schedule()`
   from another thread and `Runtime::stop` and `Runtime::spawn`.
2. A stop token handed to an operation on a shard is requested on that shard. The
   scope's stop source satisfies this because `Runtime::stop` arrives through the
   inbox. The timer sender's stop callback asserts `ShardContext::current()` in
   debug builds.
3. Operations complete on the shard that owns them, so the receiver runs there too.

## Scheduler and senders

`Scheduler` holds one `ShardContext*`. It is copyable and equality-comparable, and
not default-constructible: a task started without a shard scheduler fails to compile
instead of running on nothing. It models `ex::scheduler` and adds the timed shape,
`now()`, `schedule_after(duration)` and `schedule_at(time_point)`, with the member
names stdexec's `exec::timed_scheduler` expects, though the runtime does not name
that concept. `now()` returns the tick stamp.

- **`schedule()`.** The operation state is a `Work`. `start` compares
  `ShardContext::current()` with the scheduler's context: equal, push onto the run
  queue; different, push into the inbox. When the node runs it checks the receiver's
  stop token and completes `set_value()` or `set_stopped()`, on the shard. No
  allocation, no stop callback.
- **`schedule_after(d)` and `schedule_at(t)`.** The operation state is a `Timer` plus
  an intrusive stop callback on the receiver's token. Started on the shard it arms
  directly, `after` relative to the tick stamp. Started elsewhere it hops through the
  inbox as a `Work` and arms on arrival. The stop callback cancels the timer and
  completes stopped. Firing completes `set_value()`.
- All three advertise that they complete on their scheduler, through the names core
  exports, so a task awaiting them wraps nothing around them. They also answer
  `get_completion_scheduler<set_value_t>` with the `Scheduler`.

- **`spawn(sender)`.** Shard thread only: spawns into the context's scope with this
  scheduler in the environment. The one way to start a shard task.

`Scheduler` reports a forward progress guarantee of `parallel`, as `run_loop` does.

## The shard task

```cpp
// Errors default to std::exception_ptr.
using ShardEnvironment = core::TaskEnvironment<Scheduler, ex::inplace_stop_source>;
template <typename T>
using task = core::task<T, ShardEnvironment>;
```

The scheduler type is the concrete `Scheduler`, so the task holds one pointer, and
`affine` does nothing for the shard's senders. The stop source is
`inplace_stop_source`; errors are `std::exception_ptr`, the default. The allocator
slot stays default in this spec; phase 1 routes it to the connection arena. A child
task awaited by a parent inherits scheduler and stop token through the environment,
as P3552 specifies. A task parked on a shard timer when the scope stops sees the
timer complete stopped and unwinds; its exception, if any, reaches the scope.

`core::task<T>` with the default environment remains for tests and code outside a
shard, as today.

## The shard

### ShardQueue

`ShardQueue<Device>` is the stack's only view of the device: `allocate()`,
`transmit(Packet&&)`, `receive(span)` for the shard, `index()`, and the device's
facts, `mac()`, `mtu()`, `capabilities()`, `steering()`, `queue_count()`. `discard()`
drops what the device will not take and counts it refused; the shard calls it once,
after the last flush at drain. It owns a bounded transmit ring of
packets, `ShardConfig::transmit_ring` deep. `transmit` appends; when the ring is full
it flushes to the device first and retries once; if the device still refuses it
returns `false` and the caller keeps the packet. That `false` is the backpressure
signal TCP treats as loss. `flush()` hands the ring's front to `Device::transmit` and
drops the accepted prefix. Nothing blocks, throws or allocates.

### Stack contract

```cpp
template <typename S, typename Device>
concept IsStack = requires(S& stack, std::span<typename Device::Packet> burst) {
    { stack.on_receive(burst) } -> std::same_as<void>;
};
```

A stack is constructed in place by the shard as `Stack{context, queue, args...}`,
with `ShardContext&` and `ShardQueue<Device>&` first and the user's arguments after.
`on_receive` is called on the shard thread, inside a tick, with a non-empty burst; the
stack moves out the packets it keeps and the shard frees what it leaves. Everything
else, timers, tasks, scheduling, the stack does through the context. The stack is
destroyed before the queue and the context, so its armed timers and spawned tasks
must be gone by then, which the drain guarantees.

The only stack in this spec is the test's Ethernet echo: for a frame addressed to the
port's own MAC, swap the addresses, write the shard index into the last two bytes of
the payload, transmit; leave everything else for the shard to free. The address
filter is what lets the echo terminate on a loopback device such as `net_ring`, where
its own reply comes back addressed to the peer. Phase 1's Ethernet and IPv4 layer
fills the same slot.

### Tick

```cpp
bool step(std::chrono::steady_clock::time_point now) noexcept;
void run();  // step(now()) until context().drained(), with the idle policy
```

`step`: set the thread-local current context for the duration; receive one burst of
up to `ShardConfig::receive_burst` packets and, if any, call `on_receive` and free the
leftovers; `context.run_once(now)`; `queue.flush()`. Packet work goes before timers
and tasks so a reply leaves in the tick its request arrived. Returns whether anything
happened. `run()` sets the current context once and loops; tests call `step` with
stamps of their choosing and never need a thread.

### Idle policy

`ShardConfig::idle` is `Spin`, the production default, or `Yield`: after
`yield_after` consecutive empty steps the loop calls `std::this_thread::yield()`
once per step until something happens. Tests and CI use `Yield`. Sleeping with a
wakeup is out of scope.

### Configuration

```cpp
enum class IdlePolicy : std::uint8_t { Spin, Yield };

struct ShardConfig {
    std::chrono::nanoseconds timer_resolution = std::chrono::milliseconds{1};
    std::size_t receive_burst                 = 64;    // ethdev's max_burst
    std::size_t transmit_ring                 = 512;
    IdlePolicy idle                           = IdlePolicy::Spin;
    std::uint32_t yield_after                 = 1000;
};
```

`Shard(const ShardConfig&, Device&, std::uint16_t queue, Args&&... stack_args)`.

## The runtime

```cpp
struct ShardThread {
    std::optional<unsigned> cpu;  // pin here; none means unpinned
    std::string name;             // default "aloe-shard-N"
};

struct RuntimeConfig {
    ShardConfig shard;
    std::vector<ShardThread> threads;        // one per queue, or empty for unpinned defaults
    std::function<void()> thread_hook;       // runs first on every shard thread
};
```

`Runtime(const RuntimeConfig&, Device&, Args&&... stack_args)` builds
`device.queue_count()` shards, shard `i` on queue `i`, copying the stack arguments
into every stack, so the arguments must be copyable. A stack that needs per-shard
state reads its index from the queue.

### Launch

`start()` creates one `std::jthread` per shard. On its thread, in order: set the
name, pin to the CPU if given, run the hook, then `shard.run()`. `start()` returns
once every thread has passed its hook. A failed pin or a throwing hook is caught on
the thread, reported back, and `start()` stops the threads already running and
throws `RuntimeError` with the first failure's text. `start()` twice is an error.

### Stop and drain

`stop()` pushes a stop `Work` into every shard's inbox. It is safe from any thread
and idempotent. After it, a shard keeps ticking: frames still arrive and reach the
stack, timers still fire, so a connection can finish a FIN exchange in phase 1. The
loop exits when `drained()` holds, flushes the transmit ring once more, drops and
counts as refused whatever the device still will not take, and the thread ends.
`join()` waits for every thread. The destructor calls `stop()` and `join()`. A task
that never ends holds its shard open by design; `TaskScope::size()` makes a stuck
drain visible, and the drained log line is what a reader looks for.

### From outside

`scheduler(i)` returns shard `i`'s `Scheduler`, usable from any thread: `schedule()`
goes through the inbox. `spawn(i, Sender&&)` posts a `Work` carrying the sender to
shard `i`, where it is spawned into the scope; two allocations on a cold path.
`shard(i)` and `counters(i)` are for the shard's own thread or for after `join()`.

## Counters

```cpp
struct ShardCounters {
    std::uint64_t ticks              = 0;
    std::uint64_t idle_ticks         = 0;
    std::uint64_t frames_received    = 0;
    std::uint64_t frames_transmitted = 0;
    std::uint64_t transmit_refused   = 0;  ///< `transmit` returned false.
    std::uint64_t inbox_received     = 0;
    std::uint64_t work_run           = 0;
    std::uint64_t timers_fired       = 0;
    std::uint64_t tasks_spawned      = 0;
    std::uint64_t tasks_completed    = 0;
    std::uint64_t tasks_stopped      = 0;
    std::uint64_t tasks_failed       = 0;
};
```

Monotonic, plain integers, read on the shard thread or after `join`, like the
device's `QueueCounters`. Live snapshots from another thread are out of scope.

## Logging in the runtime

Through `core::logger("aloe.runtime")`, cold paths only: a shard thread starting,
with index, CPU and name; stop requested; drained, with the counters; a failed task,
with its exception text; a hook or pin failure. Nothing inside `step`. Tests register
a shared gtest environment, `aloe::testing::LoggingEnvironment` in `tests/shared/`,
that constructs `core::Logging` at `Warning` so the suite stays quiet.

## Error handling

- Setup throws `RuntimeError` (a `std::runtime_error`): a device with zero queues,
  `threads` whose size matches neither zero nor the queue count, a CPU the system
  rejects, a thread that fails to start, a hook that throws, `start()` twice.
- The hot path never throws: `allocate` returns an optional, `transmit` a bool,
  `step` and `run_once` are `noexcept`, `Work::run` and `Timer::fire` are `noexcept`.
- A task's exception reaches the scope and is logged and counted there.
- Using a context, scope or wheel from the wrong thread, destroying an armed timer,
  and spawning outside the shard thread are programming errors caught by debug
  assertions.

## Testing

The runtime is the hardest part and gets the heaviest tests. Time is always passed
explicitly where a test checks timing; no test sleeps to wait for a timer.

### Unit, `tests/unit_tests/component/runtime/`, target `...Runtime`

No device, one thread unless stated.

| File                     | Pins                                                                                                                                                                                                                                                                                                                                                                                                                                                           |
|--------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `test_run_queue.cpp`     | FIFO order; work pushed while a chain runs waits for the next step; `take` empties.                                                                                                                                                                                                                                                                                                                                                                            |
| `test_inbox.cpp`         | Push and pop on one thread; order; `empty` through a push-pop cycle.                                                                                                                                                                                                                                                                                                                                                                                           |
| `test_timer_wheel.cpp`   | Never early; at most one resolution late; cancel; re-arm moves the deadline; several in one slot fire in arm order; deadlines hours out cross levels; a deadline in the past fires on the next advance; a seeded random run of arms and cancels against a sorted reference.                                                                                                                                                                                    |
| `test_shard_context.cpp` | `run_once` order: inbox, then timers, then ready work; work pushed during the run waits; stop and `drained`; counters move.                                                                                                                                                                                                                                                                                                                                    |
| `test_scheduler.cpp`     | Same-shard `schedule` lands in the run queue and never touches the inbox; cross-thread `schedule` arrives through the inbox and completes on the context's thread; stop requested before the run completes stopped; `schedule_after` fires on the first step whose stamp reaches it; a stop request cancels an armed timer; `now()` is the stamp. **The zero-extra-pushes test**: a task awaits `schedule_after`, the wheel fires once, `work_run` stays zero. |
| `test_task_scope.cpp`    | Spawn, complete, `empty`; `request_stop` reaches a task parked on a timer; spawn after stop completes stopped; a throwing task is counted failed and the shard continues; `join` completes when the last task ends.                                                                                                                                                                                                                                            |
| `test_shard_task.cpp`    | A child task inherits scheduler and stop token; exceptions propagate to the parent; a task spawned into the scope runs on the shard with no reschedule.                                                                                                                                                                                                                                                                                                        |

### Shard and runtime on the fabric, same target

| File               | Pins                                                                                                                                                                                                                                        |
|--------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `test_shard.cpp`   | One shard driven by `step` from the test thread: the echo reply leaves in the same tick; leftovers are freed; the transmit ring flushes mid-tick when full and refuses when the device is full; stop then drain; `Yield` counts idle ticks. |
| `test_runtime.cpp` | N shards on threads over an N-queue fabric port; `spawn(i)` runs on shard `i`; `stop` and `join`; hook called once per thread; `counters(i)` after join; an impossible CPU throws from `start`.                                             |

### Threads, target `...Runtime.Threads`, the one the `tsan` preset exists for

| File                     | Pins                                                                                                                                                                                                                                            |
|--------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `test_inbox_threads.cpp` | P producer threads push N nodes each while the consumer pops; every node exactly once; per-producer order holds.                                                                                                                                |
| `test_cross_shard.cpp`   | A task on shard A schedules onto B and back, thousands of rounds; the count matches and every hop ran on the right thread.                                                                                                                      |
| `test_steering.cpp`      | An N-queue fabric port with RSS behind the runtime and client ports sending flows whose tuples `queue_for` maps to each shard. The echo stamps its shard index; every reply came from the predicted shard and every frame arrived exactly once. |

### Ethdev, `tests/integration_tests/runtime/`

The runtime over `net_ring` with four queues and `register_thread` as the hook: each
shard transmits frames on its queue, the ring loops them back onto the same queue,
the echo answers, and no frame crosses queues. Proves the loop with real mbufs, the
hook, and queue isolation. The null driver is not used here: it produces frames on
its own, which proves nothing about the loop.

### Manual, `tests/manual_tests/runtime/`

The echo over `net_tap0`, with the test injecting frames from a packet socket on the
tap and expecting them back with the addresses swapped. Root, as every tap test.

### Example

`examples/ethernet_echo`: EAL arguments, a port name, a queue count; runs the echo on
that many shards until interrupted and prints the counters. This is the real-card
run of the "done when", and the module's living example.

## Documentation

- New `docs/architecture/runtime.md`: what it is, the key types, usage with the echo,
  the threading contract, design notes (why a direct call, why a device-free context,
  why one wheel per shard, why the task environment matters, why quill behind an
  alias).
- `docs/architecture/overview.md`: "Shards" and "Senders and receivers" move from
  design to status with what exists; "Layers" gains the stack contract.
- `docs/architecture/core.md`: the logging alias and the task environment.
- `README.md`: a module row. `AGENTS.md`: a layout row for `runtime`, the rule that
  only `core/log/log.hpp` names quill, and whatever traps the implementation finds.
- `docs/roadmap.md`: phase 0 marked done when this lands.

## Verified facts

From the headers in `build/release/vcpkg_installed` and the vcpkg port tree on
2026-10-02:

- stdexec, port version-date 2026-05-25, includes `__detail/__task.hpp`,
  `__detail/__counting_scopes.hpp` and `__detail/__affine.hpp` from
  `<stdexec/execution.hpp>`. `stdexec::task<T, TaskEnv>` reads `allocator_type`,
  `start_scheduler_type`, `stop_source_type`, `error_types` and
  `env_type<ParentEnv>` from the environment, each defaulting when absent; the
  default scheduler is `task_scheduler`, type-erased behind a vtable with in-situ
  storage. `await_transform` wraps every awaited sender in `affine`, which leaves a
  sender alone when its completion behaviour is inline or already affine.
- `exec::task` and `exec::basic_task` still exist and store an `__any_scheduler`;
  they are no longer used.
- `exec::get_completion_behavior` lives in `<exec/completion_behavior.hpp>`; the
  `stdexec::` spelling is deprecated.
- quill 13.0.0 is on vcpkg, header-only, no dependencies, and installs in seconds.
  `quill::Logger::log_statement(MacroMetadata const*, Args&&...)` is the function
  under the macros; `MacroMetadata` is a `constexpr` aggregate of source location,
  function, format, tags, level and event. `Backend::start`, `Backend::stop`,
  `Frontend::create_or_get_sink` and `Frontend::create_or_get_logger` are the
  entry points. spdlog 1.17.0 is also on vcpkg with `fmt` as a default feature.
- The repo's `tsan` preset runs the whole test suite; the fabric already has one
  thread-stress target as the pattern to follow.
- `tests/shared` already provides `EalEnvironment`, `probe_vdev`, frame builders and
  the conformance suite; the ring test shows frames transmitted on queue `i` of a
  ring port come back on queue `i`.

## Claims the prototype must prove

Each becomes a test or a finding that amends this spec.

1. `stdexec::task` with `ShardEnvironment` compiles with our `Scheduler`, picks it up
   from the scope's environment, and performs no reschedule around the shard's
   senders. Measured by the zero-extra-pushes test.
2. `core::task<T>` with the default environment still runs under `sync_wait`, so the
   existing tests in `tests/unit_tests/common/core/test_execution.cpp` keep passing
   unchanged.
3. The completion-behaviour advertisement can be spelled through a core alias
   without naming `exec::` in the runtime.
4. A macro-free quill wrapper with the format string as a template parameter
   compiles under clang 22 and GCC 16 and formats correctly.
5. `rte_thread_register` on a `std::jthread` succeeds after an EAL started with
   `-l 0`, and mbuf allocation on that thread works.
6. The echo over `net_ring` with four queues sees every frame on its own queue.
7. The thread tests pass under `tsan` and the whole suite under `asan`.

## Findings from the spot checks

On 2026-10-02, after the design session, two throwaway probes checked the uncertain
claims against the installed headers with the project's warning set on clang 22 and
GCC 16. The implementation plan carries the details; what changed the spec:

1. **The task never wraps shard senders in `affine`, by one attribute.** stdexec's
   P3552 task inspects the attributes of its *scheduler's own `schedule()` sender*: if
   `exec::get_completion_behavior_t<set_value_t>` answers
   `exec::completion_behavior::asynchronous_affine`, every awaited sender is passed to
   `as_awaitable` directly. So the mechanism is an attribute on the scheduler's
   senders, aliased through core, and no customisation of `affine` is needed. Proven
   by a parent and child task awaiting one `schedule()` and two timers: one run-queue
   push, two timer arms, nothing else.
2. **The environment that starts a task must answer `get_start_scheduler`.** That is
   stdexec's P3552 spelling of the query, next to `start_scheduler_type` in the
   environment type; both spellings are confined to core and the scheduler's
   `SchedulerEnv`. Answering `get_scheduler` too keeps `read_env(get_scheduler)` working.
3. **`stdexec::get_completion_behavior` is deprecated** and fails under `-Werror`;
   core aliases the `exec::` spelling from `<exec/completion_behavior.hpp>`.
4. **The default-environment `core::task` still runs under `sync_wait`**, so the
   existing execution tests keep passing after the re-alias.
5. **The macro-free quill wrapper works** on both compilers with the format string as
   a `FixedString` template parameter and one `static constexpr quill::MacroMetadata`
   per instantiation, calling `quill::Logger::log_statement<false>`. The wrapper's
   logging functions are members of `Logger`, `log.info<"...">(args...)`, rather than
   the free functions the first draft described.
6. **The scope takes an environment, the scheduler supplies it.** `TaskScope::spawn`
   knows nothing of schedulers; `Scheduler::spawn` passes the environment. This
   removes a header cycle between the scope and the scheduler and is where phase 1's
   allocator query will go.
7. **The echo filters by destination address**, so the loopback tests terminate.

Claims 5 to 7 of "Claims the prototype must prove", `rte_thread_register` on a shard
thread, the ring echo and the sanitizer runs, are left to the plan's Tasks 13, 12 and 16.

## Risks

| Risk                                                                                                                | Mitigation                                                                                                                                |
|---------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------|
| P3552's task is new in stdexec and moving; a later port bump renames an environment member or the affine mechanism. | Everything that names it is in `execution.hpp`; the plan pins what it relied on under "Verified facts".                                   |
| `affine` still inserts a reschedule for our senders.                                                                | The zero-extra-pushes test fails loudly; fallback is customising `affine` through the scheduler's domain, which the standard also allows. |
| The wheel's `advance` loops over empty slots after a long stall.                                                    | Accepted for phase 0; a bitmap of occupied slots is a later optimisation and the API does not change.                                     |
| Unbounded inbox: a flood from other shards grows the run queue.                                                     | Producers are our own shards and the main thread; no external input reaches the inbox. Noted for the TCP spec.                            |
| Busy-polling shards in CI starve the runner.                                                                        | Tests use `IdlePolicy::Yield` and few shards.                                                                                             |
| quill's backend thread ends up on a shard's core.                                                                   | `Logging` takes the backend's CPU affinity; the example pins it.                                                                          |

## Out of scope

Connection leaf senders (connect, accept, wait readable, send, close), the
per-connection arena through the allocator query, flow rules and software steering
between shards, the two-lane shard, sleeping shards with wakeups, live counter
snapshots from other threads, launching on EAL lcores, scripted impairment in the
fabric, a logging surface beyond level, console sink and backend affinity.

## Open questions

Left to the plan or to later work; none blocks this spec.

- Whether `Scheduler` should also satisfy `exec::timed_scheduler` formally. The member
  names match, so it costs nothing to check in a test if a core alias for the concept
  is wanted.
- Whether `TaskScope::spawn` should take an allocator now, as the standard's
  `counting_scope` does through the environment, so phase 1's arena needs no API
  change. Leaning yes if it is cheap in the prototype.
- The default `transmit_ring` depth, 512, is a guess; the TCP spec may revisit it.

## Work order

For the plan to detail: core first (task alias, environment, quill wrapper, fixed
string), then the primitives bottom-up (work and run queue, inbox, wheel, scope,
context), then scheduler and senders, the task, the shard queue and shard with the
echo, the runtime, the thread tests, the ethdev hook and ring test, the tap test and
example, documentation last. Each step lands with its tests and with `debug`,
`gcc-debug`, `asan` and `tsan` green.
