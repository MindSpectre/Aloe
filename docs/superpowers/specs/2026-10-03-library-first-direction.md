# Library first: bricks and a runtime over them

**Date:** 2026-10-03. **Status:** decided, applied to the documents and to the module layout on branch
`runtime/execution-runtime` after the shard runtime landed. Amends the direction in
`2026-10-02-shard-runtime-design.md`, not its mechanisms.

## Context

Aloe's target is tick-to-send: order gateways and feed handlers, where the author's production layout is
readers, writers and balancers on pinned cores, SPSC rings between them, nothing on the hot path that
schedules, and kTLS sockets today. The move to DPDK is to take the writer's send off the kernel's syscall,
TCP, kTLS, qdisc and driver path, and the reader's wake off epoll and the softirq path.

Phase 0 delivered the shard runtime with senders and receivers named as "the public asynchronous model",
the stream concept "expressed as senders", and phase 1 done when "an echo client and server written as
coroutine tasks" interoperate. Reviewed against the target, that ordering is wrong in three separable ways.

1. **Who owns the loop.** A runtime that spawns threads and calls the stack is a framework. A gateway that
   already has its cores cannot add a second event source, pick the order of work or use its own thread.
2. **How TCP hands data up.** A callback into the application from inside segment processing re-enters the
   stack, raises lifetime questions, and forces the whole program onto callbacks. Readiness, an intrusive
   event list the caller drains after `process`, costs one link per changed connection and leaves order,
   batching and priority with the caller.
3. **Which way the templates point.** A layer templated on the layer above it (a `Handler`) has to know
   who reads it and cannot change. Templated on the layer below it only, the hot path still inlines.

Seastar is the extreme of the framework shape: it owns the thread, the allocator and the future type, and
its data path is a future per read with a continuation allocated when the future is not ready. asio is the
other shape: an `io_context` you run where you like. Aloe's shard should be a constraint on who writes what,
with the loop the program's.

## Decision

**Two products, the second built only from the first.**

- **Bricks.** `loop` today; the protocol modules from phase 1. Each is a library that depends on the layer
  below it, calls nothing above it, and reports what happened as a list the caller drains. No brick names
  stdexec; the `loop` target does not link `core`, so a protocol module cannot include it by accident. The
  hand-written loop over the bricks is the primary form of the product: what the first example shows, what
  the benchmark measures, what phase 1 is done against.
- **Runtime.** `runtime`: the same loop written for you, with threads, pinning, counters, the drain
  protocol, a scheduler, timer senders, a counting scope and the P3552 task. Its step is the hand-written
  loop plus one pass that wakes operation states parked in wait slots the runtime keeps per connection, in
  a parallel array keyed by the connection's stable index. It pays per awaited event, never per packet.
- **The rule.** A capability lands in the bricks first, as a plain call or an event, and the runtime wraps
  it afterwards. The runtime never has a capability the bricks lack. Docs and examples lead with the bricks.

**The shard is a rule, not an owner.** One core is the only writer of one queue's state. Two cores may
split a shard by direction: the receive half is the only writer of ack, window and round-trip state, the
transmit half the only writer of the send buffer and owner of the retransmit timer, linked by a small
record. TCP is designed as the two halves in phase 1 and runs them in one loop first.

**Templates point downward.** A layer is parameterised by the device and its packet type, never by its
reader.

## Consequences

- `component/loop` (`aloe::loop`, `Aloe::Component::Loop`, `<aloe/loop>`): `Work`, `RunQueue`, `Inbox`,
  `Timer`, `TimerWheel`, `ShardCounters`, `ShardQueue<Device>`, moved from `runtime` unchanged except for
  the namespace. `runtime` keeps `ShardContext`, `TaskScope`, `Scheduler`, `task`, `Shard`, `Runtime` and
  links `loop`; `<aloe/runtime>` includes `<aloe/loop>`. Tests of the bricks move with them.
- Overview, roadmap, README, AGENTS.md and the two module pages rewritten for the two products.
- Phase 1 scope: UDP with multicast; TCP as two halves; placement by flow rules per connection; the stream
  shape as bricks (`process`, `events`, `unread`/`consume`, `send` that never coalesces two sends, `flush`);
  the connection leaf senders as a runtime module over the event list; a per-tick hook on the runtime's
  shard and a flush before the tasks run; a tick-to-send benchmark against a raw device poll. Done-when
  puts the hand-written echo first and the task echo second.
- Phase 2 gains pre-built segment headers per connection; phase 1's header layout must allow them.
- Phase 3 notes the record layer on the connection's core and NIC TLS offload as the later option.

## Rejected

- **Callback-first stream API** (`on_data` into a `Handler` template parameter). The floor was too high;
  see "Context".
- **Senders as the primary stream API.** Right for servers, wrong for the target; kept as the runtime tier.
- **A second umbrella under `runtime` instead of a module.** A module makes the "no stdexec" property a
  link-time fact rather than a convention.

## Open

- The module name `loop` is a ruling; renaming is mechanical.
- Whether `ShardCounters` splits along the product boundary.
- The clock: `steady_clock` today; a TSC clock behind a policy when the benchmark says so.
- The ring depth and a power-of-two mask, with the TCP spec.
