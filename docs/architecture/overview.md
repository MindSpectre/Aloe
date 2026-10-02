# Aloe Architecture

Aloe is a userspace TCP/IP stack on DPDK. It aims to give application code a stream API in the spirit of
Boost.Beast, with the whole path from Ethernet frame to WebSocket message running in userspace, and with
senders and receivers as its asynchronous model. Three goals rank every trade-off, in this order: **low
latency**, **high throughput**, and **long-lived connections**.

This page describes the design the stack is built towards. Today the repository holds the build skeleton and
the [`core`](core.md) module; the rest arrives phase by phase, as the [roadmap](../roadmap.md) lays out. Each
section says what exists and what is still design.

## Shards

A shard is the unit of everything. Each shard runs on one isolated core and owns one receive queue and one
transmit queue of the network card, its packet buffer pool, its ARP cache, its connection table, its timer
wheel, its run queue and its run loop. Nothing on the data path is shared between shards, so the data path
takes no locks and touches no atomics.

- **The stack owns the run loop.** A shard polls its queues, fires due timers and runs ready work, all on its
  own core. Application code runs on that core too, as work the shard schedules.
- **A connection belongs to one shard for its whole life.** Every packet of that connection is received,
  processed and answered on the same core.
- **Inbound packets must reach the owning shard.** Accepted connections are placed by the card's
  receive-side scaling hash over the 4-tuple. For outbound connections, the local port is chosen so the hash
  lands on the wanted shard. Flow rules are the fallback, and software steering between shards is the last
  resort.

A two-core shard, one core per direction, is a possible later option. TCP is written as a receive half and a
transmit half, each the only writer of its own state and linked by an explicit event interface, so the
option stays open without a rewrite.

Status: design. Arrives in phase 0.

## Senders and receivers

The public asynchronous model is senders and receivers: stdexec today, `std::execution` once the standard
library ships it. Code names these facilities only through the [`core`](core.md) module, so the switch is a
one-header change.

The protocol layers know nothing about the asynchronous model. Between them and the senders there is one
internal seam: an operation parked on a connection is completed on the core that owns it. On top of that
seam the runtime provides:

- **A concrete scheduler per shard.** A value type, never type-erased. Scheduling on the same shard pushes
  onto an intrusive run queue with no atomics. Scheduling onto another shard goes through that shard's
  multi-producer inbox.
- **Leaf senders whose operation state is the wait node.** Connect, accept, wait until readable, send, close
  and timers. The operation state lives in the caller's frame and links into the connection, so completing
  it allocates nothing.
- **Readiness on receive.** The receive sender completes when enough bytes have arrived. The application
  then reads a zero-copy view of the input and consumes what it used.
- **Two completions on send.** One when the bytes are accepted into the send queue, and one when the peer
  has acknowledged them, which is when a zero-copy buffer may be reused.
- **Level-triggered cancellation.** Each connection owns a stop source. A stop request wakes a parked
  operation and marks the connection closed, so any operation started afterwards completes stopped at once.
- **Deadlines as stamps.** A connection carries a deadline, the shard's timer tick sweeps it, and expiry
  becomes a stop request. There is one timer per shard, never one per operation.
- **A per-shard counting scope.** One task per connection runs in it. Requesting stop and waiting for it to
  empty is graceful shutdown.
- **A task type bound to the shard scheduler,** with the connection's memory arena reachable from inside it.

Status: the `aloe::core::ex` alias and `aloe::core::task` exist in [`core`](core.md). The rest is design and arrives in
phase 0 and phase 1.

## Layers

Protocols compose at compile time, each layer templated over the one below it: TCP over IPv4 over a device,
then TLS over TCP, then WebSocket over TLS over TCP. Every layer sees the one below through the same stream
concept, expressed as senders. That concept is defined in phase 1 and reused by every later layer, which is
why phase 1 gets the most design care although its code is the simplest.

The device layer has two backends: DPDK's ethdev for real network cards and for tap devices, and an
in-memory fabric for deterministic tests. Scripted loss, reordering and delay join the fabric in phase 2.

Network cards vary, so offloads such as checksums and segmentation are queried from the card at run time,
and every offload has a software fallback. Steering is a pure function of what the card reports, so the
runtime can predict which queue any flow lands on.

Status: the device layer exists, see [device](device.md). IPv4 and TCP arrive in phases 1 and 2, TLS in
phase 3, HTTP/1.1 and WebSocket in phase 4.

## Build decisions

These exist today and shape every later module.

- **C++26 on libstdc++**, with clang 22 and GCC 16. No C++ modules: with the current compilers, module units
  that include stdexec do not build, and DPDK's inline C functions cannot be used from exported templates.
- **DPDK linked statically through one target, `Aloe::Dpdk`.** Its archives are linked whole, because DPDK
  drivers register themselves from static constructors and an ordinary static link drops them. The price is
  size: a debug executable that links DPDK is about 200 MB.
- **DPDK built for the generic x86-64 baseline,** so a cached build starts on any CPU model. See
  [getting started](../guides/getting-started.md#how-dpdk-is-built-and-linked).
- **Tests need no root, no hugepages and no network card.** DPDK starts with a null virtual device.
