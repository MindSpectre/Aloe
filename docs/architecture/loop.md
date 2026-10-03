# Loop Module

The loop module (`component/loop/`) is the bricks a shard loop is built from: one device queue with a
bounded transmit ring, the timer wheel, the work node with its two queues, and the counters. It is the first
of Aloe's two products, the one a program writes its own loop over, and the [runtime](runtime.md) is written
over it too. Everything is reached through the umbrella `#include <aloe/loop>` (`export/aloe/loop`), and
targets link `Aloe::Component::Loop`. It depends on [`device`](device.md) for the device concept and on
[`utils`](utils.md). It does not depend on [`core`](core.md): no header here names the asynchronous model,
and a protocol module that links this target and not `core` cannot include stdexec by accident.

## Key types

- **`aloe::loop::ShardQueue<Device>`** -- one queue of a device as a loop sees it. `receive(span)` fills a
  burst from the front and returns how many; `allocate()` gives a packet or nothing; `transmit(packet)`
  appends to a bounded ring; `flush()` hands the ring's front to the device and returns how many it took;
  and the device's facts: `mac()`, `mtu()`, `capabilities()`, `steering()`, `queue_count()`. When the ring
  is full, `transmit` flushes once and, if the device still refuses, returns false and the caller keeps the
  packet: the backpressure signal TCP treats as loss. `discard()` drops what the device will not take, for
  a drain. Nothing here blocks, throws or allocates after construction.
- **`aloe::loop::Timer`, `TimerWheel`** -- an intrusive timer node, a deadline and a function, and the
  hierarchical wheel it lives in: four levels of 256 slots at a resolution fixed on construction, arm and
  cancel in constant time, swept by `advance(now)`. Deadlines round up to the resolution, so a timer never
  fires early and is at most one resolution plus one advance late. Deadlines past the horizon, about fifty
  days at one millisecond, wait at the horizon and still fire on time. One thread uses a wheel.
- **`aloe::loop::Work`** -- the intrusive node every unit of deferred work is: a next pointer and a
  function. Anything that wants to run later derives from it and sets `run`; the runtime's operation
  states do. **`RunQueue`** is the same-thread FIFO of them: `push`, `take` the whole chain, `run_chain`.
  No synchronisation. **`Inbox`** is the queue other threads push into, Vyukov's intrusive MPSC: one
  atomic exchange and one release store per push, one acquire load per pop, no allocation. The node is the
  pusher's and stays alive until the consumer runs it.
- **`aloe::loop::ShardCounters`** -- ticks, frames, work, timers and tasks, all monotonic, read on the
  owning thread or after it has stopped. One struct for both products: a loop fills the frame, work and
  timer fields, the runtime adds the task fields.

## Usage

A loop over one queue of a device, written by hand. The timer is a node the program owns, the wheel is
advanced by the program, and nothing calls back into it:

```cpp
#include <aloe/fabric>
#include <aloe/loop>

using Clock = aloe::loop::TimerWheel::Clock;

struct Heartbeat : aloe::loop::Timer {
    Heartbeat() noexcept : Timer{&Heartbeat::fired} {}
    static void fired(aloe::loop::Timer& timer) noexcept { ++static_cast<Heartbeat&>(timer).beats; }
    int beats = 0;
};

aloe::fabric::Fabric fabric;
aloe::fabric::Port& port = fabric.add_port({.queues = 1});
aloe::loop::ShardCounters counters;
aloe::loop::ShardQueue<aloe::fabric::Port> queue{port, 0, 512, counters};
aloe::loop::TimerWheel wheel{std::chrono::milliseconds{1}, Clock::now()};
aloe::loop::Inbox inbox;  // other threads push Work nodes here: control, never data
std::vector<aloe::fabric::Packet> burst(64);

Heartbeat heartbeat;
wheel.arm(heartbeat, Clock::now() + std::chrono::seconds{1});

while (running) {
    const auto now             = Clock::now();
    const std::size_t received = queue.receive(burst);
    for (aloe::fabric::Packet& packet : std::span{burst}.first(received)) {
        const std::span<std::byte> data = packet.data();
        std::swap_ranges(data.begin(), data.begin() + 6, data.begin() + 6);  // the echo: destination <-> source
        if (!queue.transmit(std::move(packet))) {
            packet = aloe::fabric::Packet{};  // ring and device both full: drop, the slot must be empty for the next receive
        }
    }
    std::ignore = queue.flush();  // to the device now, before anything else runs
    for (aloe::loop::Work* work = inbox.pop(); work != nullptr; work = inbox.pop()) {
        work->run(*work);
    }
    std::ignore = wheel.advance(now);  // Heartbeat::fired when due
}
```

A TCP stack, from phase 1, slots into this loop between `receive` and `flush`: it takes the queue and the
wheel at construction, consumes the burst, and reports what changed as an event list the loop drains. The
[overview](overview.md) shows that loop.

## Design notes

**Bricks, not a framework.** Every type here is complete on its own and is driven by plain calls from
whoever owns the thread. The module starts no thread, reads no clock and owns no loop; the stamp is a
parameter. That is what lets a program with its own reader, writer and balancer cores adopt one piece at a
time, and what lets the runtime be written over the same pieces with nothing hidden from a hand-written loop.

**One node type, two memory orders.** `Work::next` is atomic so one node type serves both queues. The inbox
uses acquire and release on it; the run queue uses relaxed operations, which are plain loads and stores on
every supported target, so the same-thread path pays no fence. A type with an atomic member is immovable, so
containers of operation states hold them by `std::unique_ptr` or in a `std::deque`.

**Intrusive everywhere.** Timers, work and inbox nodes are all intrusive: the owner's object is the node, so
arming, queueing and completing allocate nothing. The price is a lifetime rule the owner keeps: a node stays
alive while it is armed or queued, and the `Timer` destructor asserts it in debug builds.

**The transmit ring is a batch, not a buffer.** `transmit` appends and `flush` posts, so a loop that produces
several frames in one tick posts them as one transmit burst, and a loop that wants a frame on the wire now
calls `flush` right after `transmit`. A partial accept ends a flush; ethdev takes at most 64 per call, so a
ring holding more drains over several calls. The ring depth is a run-time value, so the index wraps with a
modulo; a power-of-two mask is on the list for the TCP phase, together with the depth.
