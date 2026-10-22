# Baseline benchmark: tick-to-trade and the gateway round through the bricks, on DPDK's ring driver

Status: design agreed in the brainstorm of 2026-10-19 and 2026-10-20, written up for a builder check and
then for review before the plan. Step 1 of roadmap phase 2; tracks GitHub issue #16. Builds on the TSC
clock (PR #15, merged as 89785ad). The issue's premises were checked against the code and the DPDK 26.03
sources before this document; where a premise failed, this document says so and the issue is corrected
when the work lands.

## Goal

The baseline every later step of full TCP is measured against, taken before any of them adds work to the
hot path. Phase 2 adds retransmission, SACK, RACK and a lifecycle to `tcp::Stack`, and each step reruns
this benchmark on the same machine with the same command and records its numbers next to the baseline, so
a regression is seen as it happens rather than discovered at deployment.

Two rounds are measured, one per deployment the stack serves:

- **Tick-to-trade.** A feed handler's critical path. A market-data segment arrives from the venue, the
  application reads it from `unread()`, writes an order with `prepare` and `commit`, and the clock stops
  when the order's packet is handed to the device.
- **The gateway round.** An order gateway's critical path, the "publisher direction" of the issue made
  concrete. The application has an order in hand, on the shard's own thread; the clock starts, it copies
  the order into `prepare`, commits, flushes, and the clock stops at the device. No producer core: an
  order the thread can compose in place is composed in place, and handing it across cores would be work
  the benchmark invented.

Each round is measured at three levels, so the stack's cost and the runtime's overhead are read per
percentile, side by side, never subtracted:

- **The floor.** Nothing between the device and the application: take the raw frame, read the payload,
  write the reply frame into a fresh packet, transmit.
- **Level 0.** The hand-written loop over the bricks: `ShardQueue`, `Ipv4`, `tcp::Stack`, `TimerWheel`,
  the stream verbs, flush. The headline.
- **Level 1.** The same round as runtime tasks: a `Stream::readable` sender, the task resumes, writes,
  and the shard's flush transmits.

Every number is a per-message distribution: p50, p90, p99, p99.9 and max, with the count, never a mean.

Beyond the baseline, the benchmark is the instrument for a design question the author wants settled with
numbers before it is designed: whether a shard should span two cores, one keeping receive, application and
send for the critical connection and the other taking the stack's management. The minimal TCP design
already reserved the ownership boundaries for it ("placing maintenance on the less sensitive core ... a
constraint on the ownership boundaries, not a two-core implementation"). The loaded and isolated rounds
below measure what co-located work costs the tail, what two shards recover, and what a two-core shard
could win on top, without building it first.

## Goals, non-goals, anti-goals

Goals:

- A benchmark target that builds in `release` and reports both rounds, at every level, for each payload
  size, as distributions, in a printed table and a result file.
- The first results recorded in the docs as the phase 2 baseline, with the machine, the cores, hugepages
  and the ring driver's software checksums stated beside them.
- A rerun that is one command, comparable across phase 2 steps, with a way to put two result files side
  by side.
- The numbers the two-core shard design needs: the cross-core hop on the machine, the cost of co-located
  work on the tail, and what isolation on a second shard recovers.

Non-goals:

- Throughput. Rounds are closed-loop, one message in flight; nothing here measures messages per second.
- A two-core shard. This benchmark informs that design; it does not prototype it.
- Running in CI. The `ci` preset does not build benchmarks, and a shared runner has no quiet core.
- A real network card. The ring driver runs the code a card runs, minus the card.
- A microbenchmark suite over single functions (checksum, flow table, timer wheel). Google Benchmark
  stays available for those later; this benchmark does not use it.

Anti-goals, which would make this a failure even if it ran:

- Numbers nobody can reproduce: an unstated machine, core, hugepage state or command.
- A mean, or a single number, standing in for a distribution.
- A benchmark that measures garbage silently. Every run checks the stack's drop and checksum counters and
  the peer's view of every order, and fails loudly on a mismatch.
- Work added to the stack's hot path for the benchmark's sake. The stamps live in the benchmark's own
  wrapper device and loop, never in a brick.

## What the issue assumed, checked

- The fabric's per-frame cost holds and is worse than stated: one heap allocation, two copies and four
  mutex acquisitions per frame (`fabric.cpp`: two in `transmit`, one in `deliver`, one in `receive`). The
  fabric is not used.
- The ring driver holds: `rte_ring_create` with `RING_F_SP_ENQ | RING_F_SC_DEQ`, so receive and transmit
  are a pointer dequeue and enqueue with plain counters; offload capabilities are scatter and multi-seg
  only, so `Ipv4::send` fills both checksums in software and `tcp::Stack` verifies in software.
- One thing the issue left unsaid: a `net_ring` vdev loops a queue's transmit back onto its own receive.
  The ring tests work because the test drains the stack's own frames before injecting the peer's. This
  benchmark does not use the vdev; it builds two ports over cross-connected rings instead (below).
- "Without root or hugepages" is reversed for the benchmark. Aloe deploys on hugepages, so the benchmark
  runs on hugepages, and reserves them for its own run when it has the privilege to (below). The tests
  stay unprivileged and `--no-huge`; the benchmark accepts `--no-huge` as a pass-through for a quick
  look, and the result file says which it was.
- The TSC holds: `core::Clock` is `TscClock`, `TscClock::get_ticks()` is lfence plus rdtsc, and
  `TscClock::scale().nanoseconds(ticks)` converts. Stamps are raw ticks; conversion happens after the run.
- Google Benchmark 1.9.5 is resolved by vcpkg behind `DO_BENCHMARKS`, and it keeps no per-iteration
  samples: its statistics are over repetitions, and its loop decides iteration counts, which fights a
  scripted sequence. The percentiles need their own recording, so this benchmark is a standalone binary
  and does not link it.
- The `release-perf` preset (`KEEP_FRAME_POINTERS=ON`) exists for perf call graphs; perf is installed on
  the author's workstation and usable without root (`perf_event_paranoid=1`).
- The author's workstation: AMD Ryzen 5 7600X, one CCX with a shared L3, SMT on, boost on, performance
  governor, kernel clocksource `tsc`, no isolated cores, no hugepages reserved today. A cross-core hop
  there is the cheap kind, near 20 ns one way; a multi-CCD part or an Intel mesh pays two to three
  times more. The result file carries the machine so a baseline from one box is never read against
  another.

## Constraints

- The rules in `AGENTS.md` apply unchanged: no DPDK outside `Aloe::Dpdk`, no stdexec or quill below the
  runtime, `core::Clock` for stamps, `std::expected` on hot paths, one class one place, the namespace per
  module. The benchmark lives in `aloe::benchmarks`.
- Nothing in `common/` or `fixtures/` changes for the benchmark except where a defect is found; such a
  fix is its own commit with its own test.
- The benchmark's own code may name DPDK directly where it builds the ring pair and registers threads,
  as `tests/shared` does; everything else goes through `ethdev::Port`.
- `debug`, `gcc-debug`, `asan` and `tsan` build the target (except `tsan`, which turns benchmarks off) and
  the format check passes. The binary is run by hand, in `release`.

## The harness

### Topology: two ports over cross-connected rings

`rte_eth_from_rings` builds an Ethernet port from arrays of receive and transmit rings. The benchmark
creates two single-producer single-consumer rings per queue, `stack_to_peer` and `peer_to_stack`, and
two ports:

- The **stack port**: receive on `peer_to_stack`, transmit on `stack_to_peer`.
- The **peer port**: receive on `stack_to_peer`, transmit on `peer_to_stack`.

Both are wrapped by `ethdev::Port` by name, so the stack side is the same `Port` a card gets, with real
mbufs, headroom, software checksums and `max_burst` of 64. The peer side uses `ethdev::Port` too, for
`allocate`, `transmit` and `receive`. Ring depth 1024, as the vdev uses.

Default topology: the stack on one core, the peer on a second. Both threads are pinned with
`pthread_setaffinity_np` and registered with DPDK through `ethdev::register_thread()`, so mempool caches
work per thread. A `--same-core` switch runs peer and
stack strictly sequentially on one thread, as the ring test does; the difference between the two floors
is the raw cross-core hop on the machine. The TSC is read on both cores; Linux only selects the `tsc`
clocksource after its cross-CPU synchronisation check passes, so the subtraction is sound, and the
same-core topology is the check.

**Mbufs are recycled, never freed across cores.** DPDK's per-thread mempool cache does not amortise a
one-way flow: a thread that only gets from a pool refills it with a bulk dequeue of 513 objects every
512 gets, and a thread that only puts flushes 768 objects at once every 768 puts (`rte_mempool.h`,
`rte_mempool_do_generic_get` and `_put`). Had the peer filled ticks from its own pool and freed the
stack's orders, the stack core would have paid a ring operation of about half a microsecond inside the
window on a third of a percent of rounds, which is p99.9 exactly, and the artefact would vanish under
`--same-core`, so the floors would differ by more than the hop at the tails. Instead the peer writes
every frame it sends into the mbuf the stack's last frame arrived in: the order comes back as the next
tick, or as the bare ACK in the gateway round, and the handshake's frames seed the first. The stack
core then gets and puts into its own pool in balance, as a card's receive path does, and the peer core
touches no pool on the measured path. The rule applies to every mbuf the stack allocates in any round or
load: the peer returns it as an inbound frame or, where that is impossible, the docs say which rows
carry a pool operation. None is known today: the closed-loop tick round sends no pure ACK (see the note on the 1460 row under
Variables), so the stack core's gets and puts balance exactly.

### The stamp wrapper

`StampedDevice<Port>` models `IsDevice`, forwards every call to the port, and records
`TscClock::get_ticks()` in `transmit` when it accepts at least one packet. It is the one stop point for
the floor, level 0 and level 1, so the three levels end at the same instruction. It also serves as the
breakdown mode's last stamp. The wrapper costs one register read per transmit at every level alike.

### The scripted peer

The peer is `frames::TcpPeer` driven by a thread, as `test_tcp_ring.cpp` drives one:

- **Setup**, before any clock runs: the stack learns the peer's MAC with `Ipv4::learn` (the ARP exchange
  is not on the measured path; an `--arp-on-wire` switch does it on the wire for completeness), and the
  handshake runs scripted: SYN, SYN-ACK, ACK, with the stack as the passive side for tick-to-trade and
  the active side for the gateway, matching the deployments. A clean run configures the ARP cache with
  `reachable` and `expire` longer than the run, since the learned entry would otherwise go stale after
  60 s and put a refresh request on the hot path; the peer answers ARP requests regardless, so a long
  run never stalls. Ports built by `rte_eth_from_rings` carry an all-zero MAC; the ring tests already
  run with it, and the peer addresses frames to `port.mac()` whatever it is.
- **Tick-to-trade, per round:** build the tick frame with `TcpPeer::data` (correct sequence numbers, the
  ACK for the previous order piggybacked so the window never closes), write it into the mbuf the last
  order arrived in, take the start stamp, transmit onto `peer_to_stack`. Poll `stack_to_peer` for the
  order, parse it, check its checksums and sequence numbers, `see` it, keep its mbuf as the next
  carrier. Optional pause (`--gap`), then the next round. One message in flight at all times. The peer
  skips pure ACKs when it matches order N to tick N, as a defence; the closed-loop tick round produces none.
- **Gateway, per round:** the application on the stack's thread takes the start stamp and writes the
  order (below). The peer polls for it, checks it, and answers with a bare ACK segment in the order's
  own mbuf. The stack must process that ACK for the window to open, and in the clean round it does so
  outside the window: the application starts the next round only once it observes
  `acknowledged() == committed()` (level 0: the loop checks after `tcp.process`; level 1: the task
  awaits `stream.acked(committed)`), so the round is a closed loop on one thread, deterministic, and the
  number is the send path alone. `--load-responses` breaks the lockstep on purpose (below).
- The tick payload carries the round number in its first 8 bytes, and the application copies it into
  the order, so the peer verifies that order N answers tick N.
- Every frame is built before its clock runs, on the peer's core, outside the timed window. The fill of
  the mbuf is outside the window too; the stack then pulls the frame across cores, which is what a card's
  DMA write costs it.

### The order, gateway round

The order is composed on the stack's thread, in place; nothing crosses a core to deliver it. The orders
are pre-serialized, 128 bytes each, in an array the application walks; "serialize" in the round is the
copy into `prepare`, because encoding is the application's cost, not the stack's.

- **Level 0:** at the point of the loop where the application runs, once the previous order is
  acknowledged, the application takes the start stamp, copies the next order into `prepare`, commits,
  and the loop's `tcp.flush` and `queue.flush` carry it to the wrapper's `transmit`.
- **Level 1:** a task loops: start stamp, `prepare`, `commit`, `co_await stream.acked(committed)`. The
  commit queues the segment in TCP; the task suspends; the shard's flush transmits it. That suspension
  and flush are the runtime's overhead on the send path, which is what level 1 is for.
- Stamps: start (just before `prepare`) and transmit (the wrapper). The report gives one number, order
  to device, the issue's "prepare to the packet handed to the device".

### The application at each level

The application is the same work at every level so the comparison holds:

- **Tick-to-trade:** fold over every byte of the tick in `unread()` (a feed handler decodes the whole
  message), `consume` it, `prepare(128)`, write the order with the round number, `commit`.
- **Gateway:** start stamp, copy the order into `prepare(128)`, `commit`; next round once the order is
  acknowledged.
- **The floor** does the same reads and writes on raw frames: fold over the payload, `allocate`, write the
  Ethernet, IPv4 and TCP headers and the order from a pre-built template, `transmit`. No checksums are
  computed at the floor; the peer does not check them there. In the gateway round the floor reads and
  discards the peer's bare ACK frame, so the inbound work is the same as at the other levels.

Level 0's loop is the `tcp_echo` example's loop: `Clock::now()`, `queue.receive`, `ip.process`,
`tcp.process`, `wheel.advance`, drain events, application, `tcp.flush`, `queue.flush`. Level 1 is
`Runtime<StampedDevice<Port>, TcpStack<StampedDevice<Port>>>` with one shard pinned to the stack core,
`IdlePolicy::Spin`, and a task that loops `co_await stream.readable(1)`, application, repeat.

### Recording

Per round, raw tick stamps go into preallocated arrays, one per stamp kind, each written by exactly one
thread, indexed by round number; no atomics on the measured path. After the run, deltas are converted to
nanoseconds with `TscClock::scale()`, the warm-up rounds are discarded, the rest sorted, and the
percentiles read. Defaults: 100 000 rounds after 10 000 of warm-up, per configuration; the result file
records both.

### Breakdown mode, level 0 only

`--breakdown` stamps the phases of a level 0 round: after `receive`, after `ip.process`, after
`tcp.process`, after the application's `commit`, after `tcp.flush`, at transmit. Each phase gets its own
distribution. It costs a register read per phase, so it is off for the headline and on for attribution,
which is what a regression investigation needs first.

### The hop

`--hop` runs a bare cache-line ping-pong between the stack core and the peer core for a few seconds and
reports the one-way transfer as a distribution. Thirty lines, and it gives the design discussion the
machine's actual number next to the two floors.

### Sanity

A run fails, with a message, when: the stack's `dropped_*` or `dropped_bad_checksum` counters are not
zero at the end; the peer saw an order out of sequence, with a bad checksum, or with the wrong round
number; a round took longer than a deadline (default one second, which means something hung); the EAL
refused to start. A failed run writes no result file.

## Variables and loads

Variables in every run: tick payloads 64, 256, 1024, 1448 and 1460 bytes; both rounds; all three levels;
`--same-core` on request for the floor and level 0 (level 1 is a `Runtime`, which owns its thread, so
it cannot share one with the peer; the hop is read from the floors, which is what the switch is for).
Orders are 128 bytes, responses 64.

Two rows at the top on purpose. 1460 is a full segment. 1448 is what a full segment becomes once phase 2's
timestamps option takes 12 bytes, so it is the row later steps compare against. The brainstorm expected the
1460 row to carry RFC 5681's second-segment ACK inside the window; Task 6 of the plan found it does not:
`tcp::Stack` resets its full-segment count whenever it sends an ACK, and every order carries one, so a
closed-loop tick round never triggers the rule. The row is as clean as the others.

Loads, the second pull request, each behind its own switch so a tail can be attributed to one, all
applied to the critical shard unless `--isolate` moves them:

- `--load-responses`: in the gateway round the peer's ACK rides on a 64-byte response the application
  must read and consume, instead of a bare ACK, and the application no longer waits for it: it issues
  the next order after `--gap` whatever has arrived, and consumes responses after the critical action.
  A response then lands inside a window on some rounds and outside on others; the distribution is a
  mixture by design, because in deployment orders and venue responses coincide at random. Read against
  the clean round, whose ACK is processed outside the window, the loaded p99 shows the receive path of
  the critical connection on the writer's core when it does land inside, the one load `--isolate` cannot
  move.
- `--load-idle N`: N established, idle connections in the shard's table, opened by the peer from distinct
  ports during setup. Measures what the table costs the lookup and what the timer wheel costs with N
  entries once phase 2 arms per-connection timers.
- `--load-burst K`: K inbound 64-byte data segments to idle connections arrive ahead of the tick in the
  same burst, so the tick sits last; the application consumes them after the critical action. Measures
  "read potentially delaying an action".
- `--load-publish B`: the application streams B bytes per round out on a second connection, committed
  after the critical action; the peer drains and ACKs it. Measures "send something" on the hot core.
- `--load-arp`: the next hop's ARP entry is configured with a short `reachable`, so a refresh request is
  built and transmitted on the hot path every few rounds, and the peer answers it. The cache already does
  what Linux does, using the stale entry while the refresh is out, so this measures the request's cost,
  not a stall.
- `--isolate`: the stack port gets two queues over two ring pairs, the loads move to a second shard on a
  second core with its own peer, and the critical connection has its shard to itself. At level 0 that
  is two hand-written loops; at level 1 a two-shard `Runtime`. Rings have no RSS, so a frame lands on
  the queue of the ring it was enqueued on, which is the placement the benchmark wants. With RSS off,
  `tcp::Stack::choose_port` places an active open on queue 0 only, so every connection on the load
  shard is opened passively, by the load peer, during setup; the critical connection is on queue 0
  either way.

Reading the loads: loaded against clean is what co-location costs the tail; isolated against clean is
what remains when only the critical connection's own management stays on the hot core; that remainder,
minus the hop, is the most a two-core shard can win over two shards. Rerun per phase 2 step, the same
switches show when the remainder grows enough to pay for the split.

## Output

- **The table**, printed: one block per round and level, rows per payload size, columns p50, p90, p99,
  p99.9, max, count, in nanoseconds. Loads and topology in the block header.
- **The result file**, JSON written with `std::format`, one per run: the machine (CPU model, cores used,
  SMT state, governor, hugepages or `--no-huge`, kernel, clocksource), the git revision and preset, the
  command line, the TSC frequency, and the distributions, each as its percentiles plus the raw sorted
  sample count. Written next to the binary unless `--out` says where. The schema is flat so a script can
  read it without a JSON library in C++.
- **Comparison**: `scripts/compare-benchmark.py`, Python 3 with the standard library only, prints two
  result files side by side with the delta per percentile and flags a delta beyond a threshold
  (`--threshold`, default 5 %). The first Python in the repository; the author can veto it, in which
  case the comparison is by eye over two tables.

## Build and command line

- Target `Aloe.Benchmarks.TickToTrade` under `benchmarks/tick_to_trade/`, sources in that directory,
  linked against `Aloe::Common::Tcp`, `Net`, `Loop`, `Ethdev`, `Runtime` and `Aloe::Fixtures::Frames`.
  `benchmarks/CMakeLists.txt` adds the subdirectory; `DO_BENCHMARKS` gates it as today. Google
  Benchmark is not linked; the vcpkg feature and `find_package` stay for later targets.
- Command line, in the example's style: options first, then `--` and the EAL arguments. Options:
  `--round tick|gateway|both`, `--level floor|0|1|all`, `--payload <list>`, `--rounds N`, `--warmup N`,
  `--gap <ns>`, `--same-core`, `--stack-core C`, `--peer-core C`, `--breakdown`, `--hop`,
  `--out <file>`, `--arp-on-wire`, and the loads above. EAL defaults when none are given:
  `--in-memory --no-pci --no-telemetry -l <stack core>`, on hugepages.
- Hugepages are turned on by the benchmark itself, for its own run, whenever it can. Reserving them means
  writing the count to `/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages`, which only root may do,
  so: before the EAL starts, the benchmark reads the number of free 2 MB pages; if enough are free it
  proceeds; if not and it has the privilege, it raises the reservation by what it lacks, reads the count
  back, because the kernel reserves fewer than asked when memory is fragmented, and refuses with the
  shortfall and the remedy (`compact_memory`, or fewer pages) when it comes up short; if not and it has
  no privilege, it refuses with the exact command to run, either the benchmark under `sudo` or the
  reservation by hand. At exit the benchmark lowers the reservation back to what it found, which frees
  only pages nobody uses; `--keep-hugepages` leaves them for the next run, since a reservation after
  hours of uptime can fail where one at boot does not, and a crash leaves them reserved, which is
  harmless. It never falls back to `--no-huge` on its own; `--no-huge` reaches the EAL only when passed
  explicitly after `--`, and the result file records which it was. `docs/guides/getting-started.md`
  gains a short section saying all of this, how many pages the benchmark needs, and that the tests need
  none. Whether a binary runs unprivileged on pages someone else reserved, with `--in-memory`, is
  verified in the plan; if it does not, the docs say `sudo` is the way and why.

## Documentation

- `docs/benchmarks/tick-to-trade.md`, a new directory for benchmark pages: what is measured and why,
  how to run it (hugepages, cores, the command), how to read the table, the known artefacts (software
  checksums, no card, mbuf ownership crossing cores, SMT siblings), and the recorded results with their
  machine. Results files committed beside it under `docs/benchmarks/results/`.
- `docs/roadmap.md` step 1: the ring driver instead of the fabric, the two rounds, the three levels, the
  loads and the isolated round, and the sentence that every later step reruns it.
- `docs/architecture/overview.md`: one paragraph that the baseline exists and where.
- `AGENTS.md`: `docs/benchmarks/` in the documentation pattern, and the new traps found on the way.
- Issue #16: a comment correcting the hugepages premise and the loopback nuance when the first PR opens.

## Delivery

Two pull requests under issue #16, one design:

1. **The baseline.** The harness, both rounds, the three levels, both topologies, the payload sizes,
   breakdown, hop, the table and result file, the comparison script, the docs page with the first
   recorded results, the roadmap and getting-started edits. Phase 2 step 2 can start against it.
2. **The loads and isolation.** The switches above, `--isolate`, the result file and docs extended, and
   the reading for the two-core shard written into the docs page. The two-core shard design follows as
   its own spec, with these numbers in hand.

Done when, for each: the target builds in `release` and the four presets; the run on the author's
machine passes the sanity checks and its results are committed; the docs are current; the format check
passes.

## Decisions made in the brainstorm

- Peer on a second core is the default topology, same-core is a switch (author: "second core benchmark,
  settled").
- Levels are named floor, level 0 (the loop over the bricks) and level 1 (the runtime); the comparison
  between them is per percentile, side by side (author).
- Hugepages, not `--no-huge` (author: "we are using hugepages"). They are not turned on by default on
  the author's machine, and the author first ruled they stay a manual step, then changed their mind:
  the benchmark turns them on for its own process when it can (author: "if possible we turn on huge
  pages for this process"). The "when it can" is root, because `nr_hugepages` is root-writable; the
  unprivileged path checks and instructs instead (Claude's reading of the constraint).
- The gateway round is the publisher direction: a writer that gets an order, serializes it and pushes
  the frame (author). The order is composed on the shard's own thread, with no producer core and no
  queue between: "an order which we obviously can make right in that thread in place" (author, after
  the builder check; this replaced a producer core handing orders through `loop::Inbox`).
- The two-core shard is measured, not built, in this step (author's uncertainty is the reason for the
  loads and the isolated round).
- Standalone binary, no Google Benchmark (Claude's call, stated in the brainstorm, not objected to).
- Stamp wrapper device as the common stop point (Claude's call).
- JSON result files committed under `docs/benchmarks/results/`, a Python comparison script (Claude's
  call, flagged for veto).
- Order 128 bytes, response 64 bytes, the round number in the first 8 bytes of every tick and order
  (Claude's call).
- Defaults of 100 000 rounds after 10 000 warm-up, `--gap` default 0 (Claude's call; the first run may
  move them).
- Bare ACK per order in the clean gateway round; the response with payload is a load (Claude's call,
  so the delta isolates the application-visible receive path).

Settled after the builder check, Claude's calls, listed for the author to overrule:

- Mbufs recycled by the peer rather than freed across cores, so the mempool cache's bulk operations
  never land in the window; a one-way mbuf flow would have put a half-microsecond ring operation at
  p99.9 on every level and hidden later regressions there.
- 1448 added beside 1460 as the phase 2 comparison row. The builder check's prediction that the 1460 row is
  bimodal from a second-segment ACK was wrong: Task 6 found the stack resets its full-segment count whenever
  it sends an ACK, and every order carries one, so the row is clean.
- The clean gateway round is a closed loop on one thread, the bare ACK processed outside the window
  before the next order starts; `--load-responses` drops the wait so responses land inside windows at
  random, a mixture on purpose.
- `--same-core` applies to the floor and level 0 only.

## Left to the builder

- The exact ring and port names, the mempool sizes and the EAL memory amount.
- The JSON layout, as long as it is flat and carries everything listed under Output.
- The floor's frame template and how it keeps sequence numbers plausible for the peer's checks.
- The ping-pong's protocol for `--hop`.
- Whether the breakdown stamps go through a small `Stamps` struct or plain arrays.
- Where in `benchmarks/tick_to_trade/` the pieces split into files; one class one place applies.
- The comparison script's table layout.
- Port reuse across configurations: `RTE_MAX_ETHPORTS` is 32 and a closed port cannot be reopened, so
  one port pair serves every payload size and round, with the rings drained between configurations so
  the next stack does not count stale frames as `dropped_no_connection`.
- What the peer does with surplus mbufs (the 1460 row's pure ACKs): free them on the peer core, outside
  the window, with the stack pool sized for what the peer core's cache can hold.
- How the application at level 0 notices the previous order's acknowledgement without a check on the
  measured path: a comparison of `acknowledged()` and `committed()` after `tcp.process` is the obvious
  one.
