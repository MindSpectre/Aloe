# Tick-to-Trade Benchmark

The tick-to-trade benchmark (`benchmarks/tick_to_trade/`) is the baseline of phase 2 of the
[roadmap](../roadmap.md): the latency of the two critical paths the stack serves, through the bricks and through
the runtime, taken before any step of full TCP adds work to the hot path. It is a standalone binary,
`tick_to_trade`, built from the target `Aloe.Benchmarks.TickToTrade` over a static harness library in
`aloe::benchmarks`. It does not link Google Benchmark, because it records every message and reads its own
percentiles. It runs by hand, in `release`, on hugepages, with the stack and a scripted venue on two cores over
DPDK's ring driver. Its tests, under `tests/unit_tests/benchmarks/`, `tests/integration_tests/benchmarks/` and
`tests/functional_tests/benchmarks/`, need neither root nor hugepages, like every other test: the integration
tests start the EAL with `--no-huge`, and none of them pins a thread.

## What it measures

Two rounds, one per deployment:

- **Tick-to-trade**, a feed handler's critical path. A market-data segment, the tick, arrives from the venue. The
  application reads it from `unread()`, folds over every byte of it as a decoder would, consumes it, and writes a
  128-byte order with `prepare` and `commit`. The clock starts when the venue transmits the tick, once its frame
  is in the mbuf, and stops when the order's packet is handed to the device.
- **The gateway round**, an order gateway's critical path. The application has an order in hand on the stack's
  own thread. The clock starts, the order is copied into `prepare(128)`, committed and flushed, and the clock
  stops at the device. The order is composed in place, with no producer core, since an order the thread can
  compose itself would only cross a core for the benchmark's sake. The venue answers each order with a bare ACK,
  and the next order starts only once the stack has processed it, outside the window: the round is a closed loop
  on one thread and measures the send path alone.

Each round runs at three levels:

- **The floor.** Nothing between the device and the application: take the raw frame, fold over its payload,
  write the order into a fresh packet from a pre-built frame, transmit. The floor computes no checksum and the
  venue checks none there.
- **Level 0.** The loop a program writes over the bricks, the loop of `examples/tcp_echo`: `Clock::now()`,
  `queue.receive`, `ip.process`, `tcp.process`, `wheel.advance`, the event drain where the application runs,
  `tcp.flush`, `queue.flush`. This is the headline.
- **Level 1.** The same work as a runtime task: a `runtime::Runtime` over `runtime::TcpStack` with one shard,
  pinned to the stack's core and spinning when idle. The tick task loops on `co_await stream.readable(payload)`,
  the gateway task on `co_await stream.acked(committed)`, and the shard's flush transmits.

The application's work is the same at every level (`application_work.hpp`): an FNV-1a fold over every byte of
the tick, then an order that opens with the round number, in the same 8 little-endian bytes every tick opens
with, so the venue checks that order N answers tick N.

Every number is a distribution over the messages of one configuration: count, min, p50, p90, p99, p99.9 and max,
never a mean. A configuration is a round, a level, a topology and a tick payload; the default run measures
100 000 rounds of each after 10 000 of warm-up. The floor against level 0 is what the stack costs, and level 0
against level 1 what the runtime adds, each read per percentile, side by side.

The point is the comparison over time. Every step of phase 2 reruns the same commands on the same machine and
records its numbers beside the baseline's, as "Tracking regressions" below says, so a regression shows when it
lands rather than at deployment.

### The harness

**Two ports over cross-connected rings.** `RingPair` creates two single-producer single-consumer rings per queue,
1024 deep, `to_stack` and `to_peer`, and builds two ports over them with `rte_eth_from_rings`: the stack's port
receives on `to_stack` and transmits on `to_peer`, the venue's port the reverse. Both are wrapped in
`ethdev::Port` by name, so the stack side is the `Port` a card gets: real mbufs with headroom, software
checksums, bursts of 64. The `net_ring` virtual device is not used, because its queue loops its own transmit
back onto its receive.

**The venue.** `VenuePeer` is a `frames::TcpPeer` scripted on a thread of its own. It runs the handshake both
ways, as the deployments do: the stack listens in the tick round and connects in the gateway round. At levels 0
and 1 it checks every order's IPv4 and TCP checksums, its sequence number and its round number. Every frame it
sends is built outside the timed window, on its own core. It offers what a venue running Linux offers in its SYN
or SYN-ACK: RFC 7323 timestamps, which it echoes on every frame with a value that rises by one per frame,
SACK-permitted, and a window scale of 7. The stack's connection therefore uses SACK and scales its window by 3, and
the venue's script reads the stack's window in bytes, the field shifted by that scale.

**Mbufs are recycled, never freed across cores.** DPDK's per-thread mempool cache does not amortise a one-way
flow: with the 512-entry cache these pools get, a thread that only allocates refills it with a bulk dequeue of
513 mbufs every 512, and a thread that only frees flushes 768 at once every 768 (`rte_mempool_do_generic_get`
and `rte_mempool_do_generic_put`). Had the venue filled ticks from its own pool and freed the stack's orders, the
stack's core would have paid a ring operation inside the window on a third of a percent of rounds, which is
p99.9. So the venue writes every frame it sends into the mbuf the stack's last frame arrived in: the order comes
back as the next tick, or as the bare ACK, and the handshake's frames seed the first. The stack's core takes
from and returns to its own pool in balance, as a card's receive path does, and the venue's core touches no pool
on the measured path.

**The stamps.** `StampedDevice<Port>` forwards every call to the port and takes the stop stamp,
`TscClock::get_ticks()`, in the `transmit` the application armed for its critical frame; an unarmed transmit, a
pure ACK say, stamps nothing. It is the one stop point of all three levels, so they end at the same
instruction. The start stamp is the venue's in the tick round, taken once the tick is in the mbuf and just
before its transmit, and the application's in the gateway round, just before `prepare`. Stamps are raw ticks in
arrays allocated before the run, one slot per round and one writing thread per array. They are converted to
nanoseconds, stripped of the warm-up and sorted only after the run.

**Sanity.** A run fails with a message, and writes no result file, when:

- any drop, refusal or allocation-failure counter of the stack, TCP's, IP's or the shard's, or TCP's counts of
  retransmissions (timeout, fast, deferred and copied), zero-window probes and PAWS drops, is not zero at the end of a configuration; only the waits for ARP and IP's refusal of
  unsolicited ARP replies are allowed;
- any of TCP's receive-side counts, segments held out of order and released, ACKs sent with SACK blocks or a
  duplicate report, and ACKs the delay timer sent, is not zero: a clean ring never reorders, and the delayed ACK is
  off by default;
- any of TCP's loss-recovery counts is not zero: duplicate reports received, segments RACK marked lost and resends
  marked lost again, expiries of the reordering timer, ACKs that showed reordering, reneging, tail probes and their
  three verdicts (needless, a repaired loss, closed early by a second probe), and undos of either sign. Nothing is lost
  or reordered on a clean ring, and with one order in flight a probe waits until the retransmission timeout (see "The
  tail probe's allowance" below), so a probe is a round that stalled that long: a finding, not noise;
- the venue sees an order for another round or, at levels 0 and 1, one with a bad checksum or out of sequence;
- a round outlasts the deadline, one second or twice `--gap`, whichever is longer;
- a stamp pair is missing or inverted, checked per round and per breakdown leg, and the message names the round,
  the leg, the configuration and the two cores;
- the EAL refuses to start.

## How to run it

Build the `release` preset. The benchmark builds in every preset whose `DO_BENCHMARKS` is on, which leaves out
`ci` and `tsan`, but its numbers mean something only in `release`:

```bash
cmake --preset release
cmake --build --preset release --target Aloe.Benchmarks.TickToTrade
./build/release/benchmarks/tick_to_trade/tick_to_trade --help
```

The command line takes options first, then `--` and the EAL's arguments, which reach the EAL verbatim. Without
`--` the EAL gets `--in-memory --no-pci --no-telemetry -l <stack core>`, on hugepages. Arguments after `--`
replace that list whole, so a list of your own repeats what it still wants from it. An option that takes a value
refuses one that starts with `--`, so `--out --hop` stops with a message instead of writing a file named `--hop`.

### Hugepages

By default a run needs 64 free hugepages of 2 MB, 128 MB in all; `--hugepages N` changes the count. Before the
EAL starts, the binary reads `free_hugepages` under `/sys/kernel/mm/hugepages/hugepages-2048kB/`:

- With enough free pages it runs.
- Short of pages and running as root, it raises `nr_hugepages` by the shortfall and reads the count back,
  because the kernel reserves fewer pages than asked when memory is fragmented; if it came up short it puts the
  count back and refuses with the shortfall and the remedy (`/proc/sys/vm/compact_memory`, or fewer pages). At
  exit it writes back the count it found, which frees only pages nobody uses. `--keep-hugepages` leaves them
  reserved for the next run instead, since a reservation after hours of uptime can fail where one at boot does
  not; a crash leaves them reserved too, which is harmless.
- Short of pages and without root, it refuses before the EAL starts and prints both ways to get them:

```
hugepages: 0 free 2 MB hugepages, 64 needed, and only root may reserve them. Either run the benchmark under sudo, so it reserves 64 more for its own run and releases them after, or reserve them by hand first: echo 64 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
```

The two ways, in full:

```bash
# the benchmark reserves the pages for its own run and releases them at exit
sudo ./build/release/benchmarks/tick_to_trade/tick_to_trade

# or reserve them by hand once, then run without root
echo 64 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
./build/release/benchmarks/tick_to_trade/tick_to_trade
```

The unprivileged refusal above is verified. The two runs that reach the pages are still to be confirmed on the
author's machine. Under `sudo`, the output should open with `reserved 64 hugepages for this run`, and
`nr_hugepages` should read its old value again after the run. Without root, on pages reserved by hand, the run
should work: with `--in-memory` the EAL takes its pages through `memfd_create` rather than files under the
hugetlbfs mount, which needs no privilege. If it does not, `sudo` is the way. A reservation by hand does not
survive a reboot unless `vm.nr_hugepages` is set in a file under `/etc/sysctl.d/`.

The binary never falls back to `--no-huge` on its own. For a quick look without hugepages, pass it after `--`
with the rest of the default list and `-m 128`:

```bash
./build/release/benchmarks/tick_to_trade/tick_to_trade -- --in-memory --no-pci --no-telemetry --no-huge -m 128 -l 2
```

The run's two pools of 8192 mbufs do not fit the 64 MB the EAL takes without hugepages by default, hence
`-m 128`. The pools are that large because each level 1 configuration registers its shard thread as a DPDK
lcore that is never unregistered, and that lcore's mempool cache keeps up to 768 mbufs out of reach for the rest
of the run. A `--no-huge` run says `no hugepages` in the table's first line and `"no_huge": true` in its result
file. Its pages are 4 KB, so its numbers are no baseline.

### Cores

`--stack-core` (default 2) is the stack's core and, through the default `-l`, the EAL's main lcore: the floor and
level 0 run on the main thread, which the run pins there before the first configuration, and level 1 pins its
shard there. `--peer-core` (default 3) is the venue's, and the run refuses the stack's core for it. Choose two
physical cores whose SMT siblings are idle. `lscpu -e` lists every CPU with its `CORE`, and two CPUs with the same
`CORE` are siblings; on the author's Ryzen 5 7600X, CPU 2 pairs with 8 and CPU 3 with 9. An EAL list of your own after `--` keeps `-l` equal to
`--stack-core`. The run pins the main thread to `--stack-core` itself, so `-l` only decides where the EAL's own
threads start: the EAL puts its control threads on the allowed CPUs that `-l` does not list, which keeps them off
the stack's core only when `-l` names it.

`none` suits tests and nothing else. `--peer-core none` leaves the venue's thread and the hop's unpinned: a thread
spawned after the EAL starts inherits the main thread's affinity, so they widen their mask to every CPU, or they
would share the stack's core. `--stack-core none` does not unpin the stack: the default EAL list then says `-l 0`,
so the main thread, which runs the floor and level 0, is pinned to CPU 0, and level 1's shard thread inherits that
mask.

### The matrix

- `--round tick|gateway|both` and `--level floor|0|1|all` choose the rounds and the levels, by default every
  round and every level.
- `--payload` lists the tick payloads, from 8 to 1448 bytes, `64,256,1024,1448` by default. The gateway
  round's payload is the 128-byte order.
- `--rounds N` and `--warmup N` set the measured rounds and the discarded rounds before them, 100 000 and
  10 000 by default.
- `--gap NS` pauses that many nanoseconds between rounds, 0 by default. The deadlines grow with it. In the tick
  round the gap is bounded by the retransmission timeout, about 200 ms: the order waits through it for the next tick,
  the venue's only ACK of it, and a longer gap draws a tail probe of the order at the RTO's moment, which the venue
  refuses as out of sequence. The gateway round's gap has no such bound, since the venue acknowledges each order at
  once and the gap falls after that.
- `--topology cross|same|both`. `cross`, the default, puts the venue on its own core. `same` runs the venue
  inline on the stack's thread, strictly in turn with the stack, for the floor and level 0; level 1 is a
  `Runtime`, which owns its thread, so it has no same-core configuration, and `--level 1 --topology same` is
  refused for leaving nothing to run. `both` runs every cross-core configuration and every same-core one.
- `--breakdown` stamps the phases of level 0, as "Reading the table" below describes.
- `--hop` measures the bare cache-line hop between the two cores after the matrix.
- `--arp-on-wire` makes the stack resolve the venue by ARP instead of knowing it from a static entry: it switches the
  IP brick's learning on, with lifetimes of 24 and 48 hours so that no refresh falls inside a run. It is refused with
  `--topology same` or `both`: on one thread, the inline venue answers the stack's
  ARP request while it waits for the next handshake segment, but the stack cannot step to process the reply and
  send that segment while the venue waits.
- `--out FILE` names the result file.
- `--samples DIR` also writes every row's raw samples into `DIR`, one file per row named
  `<round>-<level>-<topology>-<payload>-<metric>.txt` (`hop-cross-core-0-hop.txt` for the hop), a comment line and
  then one value per measured round in nanoseconds, in round order. A headline run writes about 30 files of 100 000
  lines: material for a distribution or a time-series plot, not for committing.

### The baseline commands

```bash
sudo ./build/release/benchmarks/tick_to_trade/tick_to_trade --topology both --hop --out /tmp/baseline.json
sudo ./build/release/benchmarks/tick_to_trade/tick_to_trade --level 0 --breakdown --out /tmp/baseline-breakdown.json
```

The first is the headline. With the default cores, payloads and counts it is 25 configurations, 20 of
tick-to-trade (the floor and level 0 in both topologies and level 1 cross-core, four payloads each) and 5 of the
gateway round, then the hop. It runs without `--breakdown`, because the phase stamps cost level 0 time of their
own. The second carries the legs: level 0, cross-core, the four tick payloads and the gateway round, five
configurations with `--breakdown`. Each takes a few seconds.

### The result file

The result goes to `--out`, or to `tick-to-trade-<YYYYMMDD-HHMMSS>.json` beside the binary, in
`build/release/benchmarks/tick_to_trade/` for the `release` preset; the working directory takes it only when the
binary's own path cannot be read from `/proc/self/exe`. The binary checks that it can write the path before the
run and writes the file only after the whole run passed, so a failed run leaves no file behind and an earlier file
at the same path as it was. A write that fails at the end, on a full disk say, prints `cannot write` and removes
the partial file. Under `sudo` the file belongs
to root; `chown` it before committing it. Linux's `fs.protected_regular`, which systemd turns on by default,
keeps even root from reopening a file in `/tmp` that another user owns, so a second `sudo` run onto a file
already handed over stops with `cannot write`; give every run a new name.

The file is flat JSON, `"schema": 1`, one row per line:

- `machine`: the CPU model, the CPU count, the SMT state, the governor, the clocksource, the kernel and the free
  hugepages;
- `run`: the command line, the git revision and build type the binary was configured with, the UTC timestamp,
  the TSC frequency, the rounds, the warm-up, the gap, the two cores, and whether it ran without hugepages;
- `rows`: one per distribution, keyed by `round`, `level`, `topology`, `payload` and `metric`, with `count`,
  `min`, `p50`, `p90`, `p99`, `p999` and `max` in nanoseconds.

## Reading the table

The table opens with two lines, the machine and the run, then one block per round, level and topology, headed
like `== tick level0 cross-core ==`, with a line per payload and metric and the columns count, min, p50, p90,
p99, p99.9 and max, in nanoseconds. A `round` line is the distribution of stop minus start.

Percentiles are nearest-rank: percentile p is the sample of rank p times the count, rounded up, among the sorted
samples. Nothing is interpolated, so every number in the table is a latency some message saw. With 100 000
rounds, p99.9 is the 99 900th sample and a hundred rounds lie above it. Max is one round, and on a workstation
it is usually an interrupt.

The levels are read side by side, per percentile: the floor against level 0 is the stack's cost, and level 0
against level 1 the runtime's. They are never subtracted. The p99 rounds of two levels are different rounds, and
a difference of percentiles is not the percentile of a difference.

`--breakdown` adds a stamp at the end of each phase of a level 0 round and a line per leg between consecutive
stamps:

- `receive`: from the start to `queue.receive` returning the tick, the trip across the ring.
- `ip`: `ip.process`, the IPv4 header and its checksum.
- `tcp`: `tcp.process`, the connection lookup, the sequence checks and the TCP checksum over the whole payload.
  Across cores that checksum is the first read of the payload, so this leg also carries the payload's cache lines
  coming over from the venue's core.
- `application`: `wheel.advance`, the event drain and the application: the fold, `consume`, `prepare`, writing
  the order, and `commit`, which builds the segment's headers and checksums and queues it.
- `tcp-flush`: `tcp.flush`, which in a closed loop has no waiting ACK to send.
- `queue-flush`: `queue.flush` handing the queue's ring to the device, up to the stop stamp.

The gateway round has the last three legs only, because its window opens at the application. A stamp is an
`lfence` and an `rdtsc`, and it is not free: in a `--breakdown` run, level 0's `round` line carries five more
stamps in the tick round and two in the gateway round, which on the author's machine add about 40 ns and 20 ns
at p50. So level 0 against level 1 is read from the headline file, recorded without `--breakdown`, and the legs
from a second file, recorded with `--level 0 --breakdown`, whose `round` lines are not compared with the
headline's.

`--hop` adds a block `== hop - cross-core ==` with one line, metric `hop`: a cache-line ping-pong between the
stack's core and the venue's, `--warmup` plus `--rounds` round trips with the warm-up discarded, each halved. It
is the machine's one-way transfer between the two cores. Under `--topology both`, the cross-core floor against the
same-core floor is the same hop seen through the device. A frame that crosses cores is several cache lines, the
ring's index and slot, the mbuf's header and every line of the payload, so that difference is several hops and
grows with the payload.

## Known artefacts

- **Software checksums.** The ring driver offers no checksum offload, so `Ipv4::send` fills the IPv4 and TCP
  checksums in software and `tcp::Stack` verifies them in software: levels 0 and 1 carry a pass over every
  payload that a card would take off them. The floor computes none.
- **No card and no DMA.** The ring driver runs the code a card runs, minus the card. A frame reaches the stack's
  core through the caches from the venue's core, standing in for the card's DMA write; the PCIe trip, the card's
  own latency and its interrupt moderation are absent.
- **The ring port's zero MAC.** A port from `rte_eth_from_rings` has the MAC `00:00:00:00:00:00`. The venue
  addresses its frames to whatever `port.mac()` returns, and `Ipv4::process` accepts them.
- **The 1460 and 1448 rows.** 1460 bytes is a full segment at the MSS of 1460, and 1448 is what a full segment
  becomes once the timestamps option takes 12 bytes of it. The venue now offers timestamps, so a tick of 1460
  bytes would pass the Ethernet MTU and the sweep ends at 1448, the full-segment row; the 1460 rows of the
  recorded run below were taken before. The 1460 row carried no second-segment ACK: `tcp::Stack` resets its count of full segments whenever it sends
  an ACK, and every order carries one, so in a closed loop RFC 5681's ACK of every second full-sized segment
  never fires. The venue still skips pure ACKs when it matches an order to its tick, as a defence.
- **Level 1 is cross-core only.** `--topology same` covers the floor and level 0, because a `Runtime` owns its
  thread and cannot share it with the venue.
- **The tail probe's allowance.** Every stack the benchmark builds, at both levels and in both topologies, sets
  `TcpConfig::tail_probe_ack_delay` to 500 ms, the most it allows, where the default is 40 ms (`stack_tcp_config` in
  `level_bricks.hpp`). The venue acknowledges an order only with its next tick, where a real venue acknowledges at
  once or within its delayed ACK. With the default a lone order would be probed two round trips and 40 ms after it
  left, so any `--gap` above about 40 ms in the tick round would draw a probe the strict venue refuses. With 500 ms the
  probe of a lone order falls at the retransmission timeout's moment, and the tick round's `--gap` is bounded by the
  RTO, about 200 ms, as it was before the probe existed. The measured path is the same code either way: the probe's
  deadline is computed and the wheel armed once per order, and the diagnostics of the rerun of 2026-11-21 below
  measured the branch's head with either value within one 10 ns step: 500 ns at p50 both ways, 740 against 750 ns at
  p99.
- **Level 1's listener and its two yields.** Level 1 opens its listener through `Streams::ensure_listener`
  before the runtime starts, because the shard's first step receives the venue's SYN before the accepting task
  has run, and a SYN that finds no listener is reset. A public `Streams::listen` would be the proper door and is
  a later change. The tick task yields once after its last commit, so the last window excludes the stream's
  release, which sends a RST, and the wake-up of the waiting main thread. The gateway task yields once after
  connecting, so the handshake's ACK leaves alone, as the venue expects, before order 0.
- **The TSC across cores.** In the cross-core tick round the start is stamped on the venue's core and the stop on
  the stack's, so the subtraction relies on the TSC being synchronised across cores. Linux selects the `tsc`
  clocksource only after its cross-CPU check passes, and `core::TscClock` refuses any other clocksource. A run
  that sees an inverted cross-core pair fails and names the two cores; the same-core topology is the check.
- **The TSC's resolution.** On the author's Ryzen 5 7600X the TSC advances in steps of 47 ticks, 10 ns at its
  4.7 GHz, so every number there is a multiple of 10 ns except the hop, which halves its round trips. A leg of
  10 or 20 ns is at the clock's resolution, and a line can move by 10 ns from one run to the next with nothing
  changed.
- **SMT siblings and boost.** A workstation runs with SMT on, boost on and no isolated cores. A busy sibling
  shares its core's caches and execution units, and boost changes the core's clock with load and temperature
  while the TSC keeps its rate. Close browsers and IDE indexers before a recorded run, and choose cores whose
  siblings are idle.
- **The revision.** The `revision` in the table and the result file is the git revision at the last configure,
  not at the build. Reconfigure, `cmake --preset release`, before a recorded run.
- **The pool size.** Each level 1 configuration leaves a registered lcore whose mempool cache keeps about 512 to
  768 mbufs out of reach for the rest of the run, so the 8192-mbuf pools fit roughly a dozen level 1
  configurations, twice the default matrix's six; a longer `--payload` sweep would need a larger pool. A pool that
  runs dry fails the run through `tcp.allocation_failures`.
- **Two modes in the round time.** From 256 B up, the round time of level 0 and level 1 has two modes, about 140 ns
  apart at 256 and 1024 B and 210 ns at 1448 B, in both topologies, with a larger share cross-core; the floor has a
  smaller second mode. The slower mode arrives in bursts of hundreds to over a thousand consecutive rounds, and its
  share differs from run to run: over three runs a minute apart, level 1 at 1448 B spent a quarter, then six tenths,
  then nearly all of its rounds in the slower mode. A median that falls between the modes moves by a whole mode from
  one run to the next, so at 1024 B and above level 0 against level 1 is read at the minimum and at the tail, or over
  several runs, never from one run's p50. Cross-core the slower mode sits in the `receive` and `tcp` legs, the first
  touch of the frame's cache lines, while the application leg runs the same in both modes. The cause is not
  identified; the raw samples of a run, written with `--samples`, show it as a histogram and in round order.
- **The venue's pool sees a one-way flow.** Once the stack lends its orders until they are acknowledged, the venue's
  `ethdev::Port::receive` copies each order into the venue's pool and drops the shared handle, so the venue's core
  only allocates from its pool and the stack's core only frees to it. After its cache has filled, the stack's core
  returns 256 mbufs in bulk every 256 rounds, which lands inside the window when the tick's `consume` precedes the
  order's transmit. It is accepted: the stack's own pool stays balanced. The step 3 rerun is owed, so no number
  is recorded here yet.

## Results

The machine: an AMD Ryzen 5 7600X, 6 cores and 12 threads in one CCX, performance governor, boost on,
clocksource `tsc`, no isolated cores. The stack on core 2 and the venue on core 3, their SMT siblings 8 and 9
idle; 64 hugepages of 2 MB; the `release` preset.

The commands, from the repository root:

```bash
cmake --preset release && cmake --build --preset release --target Aloe.Benchmarks.TickToTrade
name="$(date -u +%Y-%m-%d)-$(git rev-parse --short HEAD)-$(hostname)"
sudo ./build/release/benchmarks/tick_to_trade/tick_to_trade --topology both --hop --out /tmp/baseline.json
sudo ./build/release/benchmarks/tick_to_trade/tick_to_trade --level 0 --breakdown --out /tmp/baseline-breakdown.json
sudo chown "$USER" /tmp/baseline.json /tmp/baseline-breakdown.json
mkdir -p docs/benchmarks/results
cp /tmp/baseline.json "docs/benchmarks/results/$name.json"
cp /tmp/baseline-breakdown.json "docs/benchmarks/results/$name-breakdown.json"
```

Then a second headline run, compared with the first as a check of the regression tooling:

```bash
sudo ./build/release/benchmarks/tick_to_trade/tick_to_trade --topology both --hop --out /tmp/again.json
./scripts/compare-benchmark.py "docs/benchmarks/results/$name.json" /tmp/again.json
```

The rerun's flags at p50 say whether the machine was quiet; the lines it flags there are noted beside the table.

A headline file is named `<date>-<revision>-<hostname>.json`, and the breakdown file of the same run
`<date>-<revision>-<hostname>-breakdown.json`: the UTC date of the run as `YYYY-MM-DD`, the short git revision the
binary was configured from, and the machine's host name. The files sort by date, and the host name keeps a
baseline from one machine from being read against another's. Further runs of the same day take a suffix after the
host name: `-run2`, `-run3` for repetitions of the headline, `-gap5us` for the matrix with `--gap 5000`, `-earlier`
for a run made before the recorded one.

### Recorded run: 2026-10-21, revision 993cdf3, host polaris

Run unprivileged on 64 pages reserved beforehand with
`echo 64 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages`, with an IDE and a browser open, so not
on a quiet machine. The binary carried the `--samples` option on top of the revision, which touches no measured
path. The files under `results/`: `2026-10-21-993cdf3-polaris.json` is the headline, `-run2` and `-run3` two
repetitions within the same minute, `-breakdown` the level 0 legs in both topologies, `-gap5us` the headline matrix
with `--gap 5000`, and `-earlier` a default-matrix run three hours before, without the hop.

The headline, in nanoseconds:

| round   | level   | topology   | payload |  min |  p50 |  p90 |  p99 | p99.9 |   max |
|---------|---------|------------|--------:|-----:|-----:|-----:|-----:|------:|------:|
| tick    | floor   | cross-core |      64 |  179 |  229 |  249 |  289 |   449 |  8449 |
| tick    | floor   | cross-core |     256 |  329 |  399 |  449 |  489 |   799 |  8789 |
| tick    | floor   | cross-core |    1024 |  889 |  969 | 1019 | 1179 |  3049 | 22819 |
| tick    | floor   | cross-core |    1448 | 1199 | 1249 | 1319 | 1519 |  3479 | 18009 |
| tick    | floor   | cross-core |    1460 | 1209 | 1259 | 1329 | 1569 |  3469 | 15159 |
| tick    | floor   | same-core  |      64 |   89 |   99 |  119 |  149 |   209 |  4719 |
| tick    | floor   | same-core  |     256 |  229 |  239 |  279 |  339 |   479 |  6179 |
| tick    | floor   | same-core  |    1024 |  799 |  809 |  889 |  959 |  1919 |  5909 |
| tick    | floor   | same-core  |    1448 | 1109 | 1209 | 1249 | 1319 |  3729 | 20579 |
| tick    | floor   | same-core  |    1460 | 1119 | 1219 | 1259 | 1329 |  4409 | 21269 |
| tick    | level 0 | cross-core |      64 |  309 |  359 |  439 |  529 |   849 |  6169 |
| tick    | level 0 | cross-core |     256 |  479 |  579 |  719 |  809 |  1729 | 14399 |
| tick    | level 0 | cross-core |    1024 | 1139 | 1399 | 1559 | 1729 |  4329 | 17199 |
| tick    | level 0 | cross-core |    1448 | 1499 | 1669 | 1869 | 2079 |  4249 | 27559 |
| tick    | level 0 | cross-core |    1460 | 1529 | 1759 | 1979 | 2219 |  4719 | 16539 |
| tick    | level 0 | same-core  |      64 |  209 |  229 |  239 |  349 |   469 |  4249 |
| tick    | level 0 | same-core  |     256 |  369 |  389 |  399 |  599 |   799 |  4959 |
| tick    | level 0 | same-core  |    1024 | 1009 | 1039 | 1229 | 1349 |  3309 |  8369 |
| tick    | level 0 | same-core  |    1448 | 1359 | 1389 | 1609 | 1709 |  3979 | 13139 |
| tick    | level 0 | same-core  |    1460 | 1369 | 1389 | 1429 | 1689 |  3789 | 17529 |
| tick    | level 1 | cross-core |      64 |  349 |  379 |  409 |  569 |   829 | 10279 |
| tick    | level 1 | cross-core |     256 |  499 |  599 |  759 |  859 |  1719 |  8409 |
| tick    | level 1 | cross-core |    1024 | 1159 | 1439 | 1659 | 1799 |  4159 | 24759 |
| tick    | level 1 | cross-core |    1448 | 1519 | 1679 | 1879 | 2099 |  4049 | 22609 |
| tick    | level 1 | cross-core |    1460 | 1519 | 1689 | 1989 | 2119 |  4239 | 27779 |
| gateway | floor   | cross-core |     128 |   49 |   59 |   69 |   89 |   129 |  5019 |
| gateway | floor   | same-core  |     128 |   19 |   29 |   39 |   49 |    79 |  5299 |
| gateway | level 0 | cross-core |     128 |   99 |  109 |  109 |  169 |   199 |  7989 |
| gateway | level 0 | same-core  |     128 |   69 |   79 |   89 |  149 |   189 | 15789 |
| gateway | level 1 | cross-core |     128 |  109 |  119 |  119 |  189 |   239 |  3579 |
| hop     | -       | cross-core |       0 |   24 |   34 |   49 |   54 |    59 |  1879 |

The breakdown, cross-core, p50 per leg in nanoseconds; the `round` column is the breakdown run's own round, which
carries the stamps' cost:

| round   | payload | receive | ip | tcp | application | tcp-flush | queue-flush | round |
|---------|--------:|--------:|---:|----:|------------:|----------:|------------:|------:|
| tick    |      64 |      99 | 49 |  59 |         119 |         9 |          39 |   389 |
| tick    |     256 |     119 | 49 | 139 |         259 |         9 |          39 |   609 |
| tick    |    1024 |     109 | 49 | 269 |         819 |         9 |          39 |  1319 |
| tick    |    1448 |     129 | 49 | 349 |        1129 |         9 |          39 |  1719 |
| tick    |    1460 |     129 | 49 | 349 |        1139 |         9 |          39 |  1729 |
| gateway |     128 |       - |  - |   - |          69 |         9 |          39 |   129 |

The hop between cores 2 and 3: 34 ns at p50, 54 ns at p99.

What they say. At 64 B, cross-core, the stack costs 130 ns at p50 and 400 ns at p99.9 over the floor, and the
runtime 20 ns at p50 and nothing measurable at p99.9 over the bricks; in the gateway round the stack costs 50 ns at
p50 over the floor and the runtime 10 ns over the bricks. The hop is 34 ns one way, and the floor cross-core
against same-core at 64 B is 130 ns: a frame is several lines. From 1024 B up the application's fold over every
byte sets the scale, about 0.75 ns a byte, and the `tcp` leg's checksum is the second cost; the receive, IP and
flush legs do not grow with the payload.

The repetitions, p50 of the cross-core tick round in nanoseconds:

| level   | payload | run 1 | run 2 | run 3 | earlier |
|---------|--------:|------:|------:|------:|--------:|
| floor   |      64 |   229 |   229 |   229 |     199 |
| floor   |     256 |   399 |   379 |   379 |     349 |
| floor   |    1024 |   969 |   989 |   989 |     939 |
| floor   |    1448 |  1249 |  1249 |  1309 |    1229 |
| floor   |    1460 |  1259 |  1259 |  1259 |    1249 |
| level 0 |      64 |   359 |   349 |   339 |     359 |
| level 0 |     256 |   579 |   549 |   559 |     559 |
| level 0 |    1024 |  1399 |  1289 |  1389 |    1399 |
| level 0 |    1448 |  1669 |  1669 |  1669 |    1859 |
| level 0 |    1460 |  1759 |  1679 |  1679 |    1879 |
| level 1 |      64 |   379 |   389 |   389 |     379 |
| level 1 |     256 |   599 |   589 |   589 |     579 |
| level 1 |    1024 |  1439 |  1439 |  1449 |    1279 |
| level 1 |    1448 |  1679 |  1859 |  1899 |    1679 |
| level 1 |    1460 |  1689 |  1889 |  1909 |    1689 |

Level 0's median at 1448 B held at 1669 ns over three runs while level 1's moved from 1679 to 1899 ns, and the
earlier run had level 0 at 1859 ns against level 1 at 1679 ns: the two modes described under "Known artefacts",
not a change in the code, and the reason the level comparison from 1024 B up is read at the minimum and at the
tail. `compare-benchmark.py` flags six p50 lines between run 1 and run 2, every one of them a mode shift or the
same-core floor at 1448 and 1460 B. The gap run leaves every p50 at one of its two modes and raises the p99.9 of
most lines, the cross-core floor at 64 B from 449 to 1799 ns: five idle microseconds between rounds cost the tail.

### Rerun of step 5: 2026-11-21, loss recovery with SACK against `main`, host polaris

**The gate is not met.** The bar is no change beyond run-to-run noise, 10 to 20 ns, at p50 and p99 of the 64-byte
tick at level 0, cross-core. Step 5 moves that tick from 460 to 500 ns at p50, +40 ns, and from 670 to 740 ns at p99,
+70 ns (medians of three runs a side). Of the 40 ns at p50, 30 are in the `tcp` leg, which acknowledges the previous
order, and 10 in the application leg, which sends the next. Every level 0 and level 1 tick row moves by 40 to 60 ns at
p50, and the gateway round at levels 0 and 1 by 10 to 20 ns. No fix was attempted in this step. What was measured of
the cause, and what was not, is set out below.

**The runs.** Step 5 is the SACK scoreboard, RACK-TLP, one recovery with PRR, and the undo. It was measured against
`main` at the branch's base on the same machine and cores as the recorded run, kernel 7.2.15:

- Both sides were built with the `release` preset and `-DDO_BENCHMARKS=ON`: `main` at 1aeddb9 in a worktree, and the
  branch's head, 13cefd6, in place.
- The head carried this step's benchmark changes (the sanity counters and the probe's allowance above), which touch
  no stack code.
- The runs were unprivileged, on 64 free hugepages reserved beforehand, and not on a quiet machine: a browser, two
  IDEs and a chat client were open, and the CPUs were about 95 % idle.
- There were three rounds of runs. Each round ran a headline run of `main`, then one of the branch,
  `--topology both --hop`, then a breakdown run of each in the same order, `--level 0 --breakdown --topology both`.
  Each run took about three seconds.

Every row below is the median of its three runs. To compare the sides, a short script (not in the repository) wrote,
for each side, a result file whose every row holds the field-by-field median of that row over the three runs, with
the first run's `machine` and `run` sections kept. `compare-benchmark.py` then compared `main`'s file with the
branch's.

The files under `results/`:

- `2026-11-21-1aeddb9-polaris.json` and `2026-11-21-13cefd6-polaris.json` are the first headline run of each side,
  and `-run2` and `-run3` the next two;
- `-breakdown`, `-run2-breakdown` and `-run3-breakdown` are the breakdown runs that followed each.

The 64-byte tick at level 0, cross-core, in nanoseconds:

| side            | run 1 p50 | run 2 p50 | run 3 p50 | median p50 | run 1 p99 | run 2 p99 | run 3 p99 | median p99 |
|-----------------|----------:|----------:|----------:|-----------:|----------:|----------:|----------:|-----------:|
| `main`, 1aeddb9 |       460 |       470 |       460 |        460 |       670 |       680 |       670 |        670 |
| step 5, 13cefd6 |       500 |       500 |       500 |        500 |       740 |       780 |       730 |        740 |

Its legs, from the breakdown runs, medians of three:

| side            | receive | ip | tcp | application | tcp-flush | queue-flush | round | tcp p99 | application p99 |
|-----------------|--------:|---:|----:|------------:|----------:|------------:|------:|--------:|----------------:|
| `main`, 1aeddb9 |     120 | 50 | 110 |         170 |        10 |          40 |   500 |     170 |             270 |
| step 5, 13cefd6 |     120 | 50 | 140 |         180 |        10 |          40 |   540 |     250 |             310 |

In the breakdown runs the `tcp` leg moves by 30 to 40 ns at p50 at every payload and in both topologies. The receive,
ip, tcp-flush and queue-flush legs do not move at p50.

Every row of the headline, medians of three, in nanoseconds:

| round   | level   | topology   | payload | `main` p50 | step 5 p50 |  +/- | `main` p99 | step 5 p99 |  +/- |
|---------|---------|------------|--------:|-----------:|-----------:|-----:|-----------:|-----------:|-----:|
| tick    | floor   | cross-core |      64 |        230 |        240 |  +10 |        290 |        280 |  -10 |
| tick    | floor   | cross-core |     256 |        380 |        380 |    0 |        450 |        460 |  +10 |
| tick    | floor   | cross-core |    1024 |        980 |        970 |  -10 |       1180 |       1170 |  -10 |
| tick    | floor   | cross-core |    1448 |       1260 |       1270 |  +10 |       1550 |       1520 |  -30 |
| tick    | floor   | same-core  |      64 |        100 |        100 |    0 |        140 |        140 |    0 |
| tick    | floor   | same-core  |     256 |        240 |        250 |  +10 |        280 |        290 |  +10 |
| tick    | floor   | same-core  |    1024 |        810 |        820 |  +10 |        890 |        920 |  +30 |
| tick    | floor   | same-core  |    1448 |       1120 |       1120 |    0 |       1210 |       1210 |    0 |
| tick    | level 0 | cross-core |      64 |        460 |        500 |  +40 |        670 |        740 |  +70 |
| tick    | level 0 | cross-core |     256 |        630 |        680 |  +50 |        870 |        980 | +110 |
| tick    | level 0 | cross-core |    1024 |       1300 |       1340 |  +40 |       1640 |       1700 |  +60 |
| tick    | level 0 | cross-core |    1448 |       1660 |       1720 |  +60 |       2000 |       2120 | +120 |
| tick    | level 0 | same-core  |      64 |        320 |        380 |  +60 |        550 |        640 |  +90 |
| tick    | level 0 | same-core  |     256 |        480 |        530 |  +50 |        730 |        840 | +110 |
| tick    | level 0 | same-core  |    1024 |       1120 |       1170 |  +50 |       1460 |       1500 |  +40 |
| tick    | level 0 | same-core  |    1448 |       1480 |       1530 |  +50 |       1840 |       1920 |  +80 |
| tick    | level 1 | cross-core |      64 |        510 |        550 |  +40 |        750 |        880 | +130 |
| tick    | level 1 | cross-core |     256 |        670 |        730 |  +60 |        970 |       1040 |  +70 |
| tick    | level 1 | cross-core |    1024 |       1330 |       1380 |  +50 |       1700 |       1820 | +120 |
| tick    | level 1 | cross-core |    1448 |       1690 |       1750 |  +60 |       2100 |       2220 | +120 |
| gateway | floor   | cross-core |     128 |         70 |         60 |  -10 |         80 |         80 |    0 |
| gateway | floor   | same-core  |     128 |         30 |         30 |    0 |         50 |         50 |    0 |
| gateway | level 0 | cross-core |     128 |        150 |        160 |  +10 |        230 |        250 |  +20 |
| gateway | level 0 | same-core  |     128 |        110 |        120 |  +10 |        200 |        210 |  +10 |
| gateway | level 1 | cross-core |     128 |        160 |        180 |  +20 |        280 |        320 |  +40 |
| hop     | -       | cross-core |       0 |         34 |         34 |    0 |         60 |         54 |   -6 |

**The noise these files show.** `compare-benchmark.py` over a pair of runs of one binary flags 7 to 14 of the
headline's percentiles and 0 to 8 of the breakdown's (each of the three pairs of each side). Over the two sides'
medians it flags 26 and 46.

- The floor runs no stack code. It came out within one 10 ns step at p50 on every row, but its tails moved:
  same-core at 256 and 1024 B, p90 went from 240 to 270 and from 810 to 860 ns, and cross-core at 1448 B, p99 went
  from 1550 to 1520 ns. A move of 30 to 50 ns in one row's tail is within this machine's noise.
- The branch's own p99 of the 64-byte tick ran from 730 to 780 ns over its three runs, and `main`'s from 670 to
  680 ns.
- The p50 of that tick moved by at most one 10 ns step within either side: 460 to 470 ns and 500 ns.

**Where it comes from: what was measured.** The following runs were made the same day, on the 64-byte tick at
level 0, cross-core. They are diagnostics, and their files are not kept.

The command for each binary was the default cores and hugepages, plus:

- `--round tick --level 0 --payload 64`;
- then the same with `--breakdown`.

The binaries ran in turn, three rounds, in four sets. Each set included `main` and the head, which measured 450 to
460 ns and 500 ns at p50 in every set.

- **By commit.** The bisection built seven of the branch's commits, its feature commits, with the fix commits
  between them not built on their own. Medians of three, in nanoseconds:

  | build                                         | p50 | p99 | `tcp` leg p50 |
  |-----------------------------------------------|----:|----:|--------------:|
  | 1aeddb9, `main`                               | 460 | 650 |           110 |
  | fbbc164, the scoreboard                       | 460 | 650 |           120 |
  | e5da258, one recovery with PRR                | 460 | 680 |           120 |
  | 84ac785, growth only while the window limits  | 460 | 690 |           120 |
  | b763912, RACK                                 | 480 | 730 |           130 |
  | f1be798, the tail probe                       | 490 | 760 |           140 |
  | 1a47e39, the undo                             | 490 | 710 |           140 |
  | 13cefd6, the head                             | 500 | 740 |           140 |
  | 13cefd6 with this step's benchmark changes    | 500 | 750 |           140 |

  The p50 rises in 10 to 20 ns steps from RACK on, and the `tcp` leg from the scoreboard on. One binary's p99 moved by
  up to 60 ns between its runs, so the p99 column does not place the steps. On the measured path the last two rows
  differ only in the probe's allowance, 40 ms against 500 ms. They are within one 10 ns step of each other at p99
  and equal at p50.
- **The brief's four candidates.** Each build below is 13cefd6 with one edit, made for the measurement only and never
  committed. Medians of p50, in nanoseconds, against the head's 500:
  - *The arm of the probe's deadline at every data segment.* `Transmitter::arm_after_send` armed the RTO into an empty
    queue, as `main` does, instead of the probe: 490.
  - *RACK's update and the probe's branch on the ACK.* Only RACK's part was taken out, by three edits:
    - in `AckProcessor::acknowledge` the local `sack` was made false at run time (`block.sack.in_use() &&
      context_.now < core::TimePoint{}`), which skips, for each entry the ACK frees, RACK's advance and the test for
      reordering;
    - `rack.on_ack` was removed from `on_ack`, and detection never ran (`detecting = false`);
    - `rack.sample` was removed from `on_acknowledged`.

    The connection still used SACK everywhere else. This build measured 480 in each of two sets, 20 ns below the
    head: half of the p50 regression, and the whole of the bar. The probe's branch on the ACK (`on_probe_ack` behind
    `verdict_open()`, and `probe_schedulable` in `set_timer`) was **not isolated**.
  - *The two subtractions in `writable()`.* **Not isolated.**
  - *`WindowUse::on_send`.* The record in `commit`, with the comparison of `recovery.room(...)` with the MSS that feeds
    it, was removed: 500.

  Two edits outside the brief's list were measured too:
  - the undo's checks, with `try_undo` replaced by an empty `Undone`: 490;
  - `[[gnu::always_inline]]` on `AckProcessor::acknowledge`, `acknowledge_data` and `on_acknowledged`, on
    `Transmitter::acknowledge` and `RetransmissionQueue::acknowledge`, and on `NodePool::release_front`: 490 in each
    of two sets.

  **No build with several edits together was measured.** Two builds were meant to take all four out together, one of
  them also forcing the helpers inline. They carried none of the edits: the loop that built them handed the variant
  names to the patching script as one word, which matched no variant. Both are the head built again: their
  `on_acknowledged` and `arm_after_send` are the head's, and the two binaries are identical. They measured 500 ns
  three times; that says nothing about the edits together.

  **The attribution is unresolved.** The edits were measured one at a time. Each gave back one or two 10 ns steps,
  which is the clock's resolution, and whether those gains add up is unknown.
- **What grew.** The same-core run has the venue inline and nothing spinning: `--round tick --level 0 --payload 64
  --topology same --rounds 2000000 --warmup 10000`, under `perf record -e <event> -c <period>`. Counting the main
  thread's samples times the period, over 2 010 000 rounds, gives these per-round changes:
  - cycles: +187, from 1334 to 1521 (period 20011);
  - instructions: +151, from 1334 to 1485 (period 20011);
  - L1 data misses: +4.2, from 7.0 to 11.2 (`L1-dcache-load-misses`, period 1009);
  - loads blocked by an older store: +7.3, from 65.7 to 73.0 (`ls_bad_status2.stli_other`, period 1009);
  - branch misses: +0.3, from 1.0 to 1.3.

  The main thread also runs the venue, the setup and the sort of the samples after the run, which are the same code
  on both sides. `ControlBlock` grew from 704 to 944 bytes and a queue entry from 88 to 96 (`sizeof`, read from the
  binaries' debug information).

  A `cycles` profile of the same run, by symbol, puts most of the growth in four places: `AckProcessor::on_ack`,
  `AckProcessor::acknowledge`, the queue's `acknowledge`, and `NodePool::release_front`, which resets a freed entry by
  assigning a temporary. Smaller shares fall on `TimerWheel::arm` and `Transmitter::arm_after_send`. Inlining
  differs between the two builds, so a symbol's share is not like for like. About nine tenths of
  `AckProcessor::acknowledge`'s samples fall on the reload of the `Freed` result, just after the call to the queue's
  `acknowledge`, which is not inlined and has just written that result to the stack.

**Which builds the diagnostics ran.** Every diagnostic build of the head was made in the scratch worktree while it
was checked out at 13cefd6: its reflog shows that checkout from 07:10:46 to 07:22:17 local time, the span of those
builds. The result files of the head's bisection runs, and of the probe and window builds, record revision 1a47e39.
The revision is read at configure time (see "The revision" under "Known artefacts"), and no CMake file changed
between 1a47e39 and 13cefd6, so checking out 13cefd6 did not reconfigure. Their code is 13cefd6's: their
`on_acknowledged` carries f3ccdb4's floor of one tick on an echo's sample, as the head built in place does, and the
1a47e39 binary's does not. Those builds, the bisection's included, kept clang-tidy on. The RACK, undo and inline
builds and the two that carried no edit had it switched off in the worktree's top `CMakeLists.txt`, so that the edits
compiled. That reconfigured the worktree, so their files record 13cefd6. `-Werror` stayed on, and clang-tidy does not
change the code generated.

**What is left.** The regression stands at 40 ns at p50 and 70 ns at p99 and is recorded here; no fix was attempted
in this step. The diagnostics point at the general work every in-order ACK and send now carries, not at one step that
could be taken out. They do not divide it among the candidates. Two kinds of change could win it back, and both would
be measured against these files:

- **Local changes**, which leave the order of an acknowledgement's steps as it is:
  - pass the queue's `Freed`, and the `Cumulative` and `AckInfo` built from it, without a round trip through memory,
    built in place or with the helpers inlined; the inline build gave back one 10 ns step;
  - reset a freed entry field by field rather than by assigning a temporary;
  - keep the fields an ACK and a send touch together in `ControlBlock`.
- **A short path** for an in-order ACK on an open connection that finds nothing reported, lost or probed, which skips
  the steps with nothing to do. This changes the order in which an acknowledgement's steps run, which the design
  sets.

### Rerun after the tick path's cleanup: 2026-11-21, step 5 against `main`, host polaris

**The gate is still not met.** The bar is no change beyond run-to-run noise, 10 to 20 ns, at p50 and p99 of the
64-byte tick at level 0, cross-core. After the cleanup below, against `main` at the branch's base run alternately with
it, that tick is 490 ns at p50 against 450 ns, +40 ns, and 730 ns at p99 against 670 ns, +60 ns (medians of three runs
a side): 20 to 30 ns beyond the bar at p50 and 40 to 50 ns at p99. The `tcp` leg is 140 ns at p50 against 110 ns,
+30 ns, as in the first rerun. Against that rerun the branch's medians are one 10 ns step lower, 500 to 490 ns at p50
and 740 to 730 ns at p99, and `main`'s, the same code, came out 460 to 450 ns at p50 and 670 ns at p99 both times: the
gap is the same 40 ns at p50 and 10 ns narrower at p99. The three changes gave back no more than the clock's
resolution on the gated row.

**What changed.** A review of the whole branch named three pieces of work on the path without loss that the path does
not need, and the fix wave took them out in one commit, bbcd1dc, without changing the order of an acknowledgement's
steps:

- RACK's detection computed the reordering window before its walk on every ACK that advanced RACK, and wrote its two
  counters with zero; on an in-order ACK the walk stops at once, at an empty queue or at the head. The window is now
  computed only when an entry needs it, and the counters are written only when something was marked.
- An ACK that moved SND.UNA and left data in flight armed the wheel twice, the RTO as the cumulative part restarted it
  and the probe in the ACK's last step. The cumulative part now records the RTO's moment only, and the last step arms
  the wheel once. On this benchmark's loop the queue is empty after each ACK, so this one is off the measured path; it
  is on every burst's.
- `commit` read what is in flight and the congestion window three times, and `prepare` twice when the window held a
  send back. Each now reads them once and reuses them.

No test or script number moved, and a trace of every ACK's timer, every commit's records and every detection, expiry
and prepare over the TCP unit tests, the scripts and the scenario runner's tests was the same line for line before and
after. The measured head also carries the fix wave's correction of a tail probe that never left (513b4f1), which adds
the test of one flag to every ACK of new data.

**The runs.** The machine, cores and kernel of the first rerun:

- `main` at 1aeddb9 in its worktree and the head, bbcd1dc, in place, each reconfigured (`cmake --preset release
  -DDO_BENCHMARKS=ON`) so that its files record its revision, and built with the `release` preset.
- Unprivileged, on 64 free hugepages reserved beforehand; a browser, two IDEs and a chat client open, the CPUs about
  95 % idle, as before.
- Three rounds, each a headline run of `main`, then one of the head, `--topology both --hop`, then a breakdown run of
  each in the same order, `--level 0 --breakdown --topology both`. The twelve runs took under a minute.
- Medians as in the first rerun: for each side a file of the field-by-field median of every row over its three runs,
  compared with `compare-benchmark.py`.

The files under `results/`:

- `2026-11-21-bbcd1dc-polaris.json`, `-run2` and `-run3` are the head's headline runs, and `-breakdown`,
  `-run2-breakdown` and `-run3-breakdown` the breakdown runs that followed each;
- `2026-11-21-1aeddb9-polaris-run4.json`, `-run5` and `-run6`, with `-run4-breakdown`, `-run5-breakdown` and
  `-run6-breakdown`, are `main`'s runs of this rerun, each made just before the head's of its round. The comparison
  reads these, not the first rerun's `main` files.

The 64-byte tick at level 0, cross-core, in nanoseconds:

| side              | run 1 p50 | run 2 p50 | run 3 p50 | median p50 | run 1 p99 | run 2 p99 | run 3 p99 | median p99 |
|-------------------|----------:|----------:|----------:|-----------:|----------:|----------:|----------:|-----------:|
| `main`, 1aeddb9   |       450 |       450 |       460 |        450 |       670 |       640 |       680 |        670 |
| cleanup, bbcd1dc  |       490 |       490 |       490 |        490 |       730 |       720 |       740 |        730 |

Its legs, from the breakdown runs, medians of three:

| side              | receive | ip | tcp | application | tcp-flush | queue-flush | round | tcp p99 | application p99 |
|-------------------|--------:|---:|----:|------------:|----------:|------------:|------:|--------:|----------------:|
| `main`, 1aeddb9   |     120 | 50 | 110 |         170 |        10 |          40 |   510 |     160 |             270 |
| cleanup, bbcd1dc  |     110 | 50 | 140 |         180 |        10 |          40 |   530 |     230 |             280 |

The same tick and its `tcp` leg in both reruns, medians of three, in nanoseconds:

| side                         | p50, first | p50, now | p99, first | p99, now | `tcp` p50, first | `tcp` p50, now | `tcp` p99, first | `tcp` p99, now |
|------------------------------|-----------:|---------:|-----------:|---------:|-----------------:|---------------:|-----------------:|---------------:|
| `main`, 1aeddb9              |        460 |      450 |        670 |      670 |              110 |            110 |              170 |            160 |
| step 5, 13cefd6 then bbcd1dc |        500 |      490 |        740 |      730 |              140 |            140 |              250 |            230 |

Over the rest of the headline, every level 0 and level 1 tick row is 30 to 70 ns slower at p50 than `main`'s, where
the first rerun had 40 to 60, and the gateway round at levels 0 and 1 is 10 to 20 ns slower, as before. Between the two
reruns `main`'s medians moved by at most 10 ns at p50 on every level 0 and level 1 row, tick and gateway; the head's
tick rows at levels 0 and 1 are 10 to 30 ns lower at p50 on nine of the twelve, the same on two, and 10 ns higher on
one. Against `main` the gap narrowed by 10 or 20 ns on six of the eight cross-core tick rows and held on the other two;
on the four same-core rows it narrowed by 10 ns on one and widened by 10 or 20 ns on three. The floor, which runs no
stack code, differed between the two binaries by up to 40 ns at p50 on one row (cross-core at 1024 B, 990 against
950 ns). Nothing here is beyond this machine's run-to-run noise on a single row.

**What is left.** The regression stands at 40 ns at p50 and 60 ns at p99, recorded here. The work the review could name
as plainly unnecessary is gone, and the gated row moved by one 10 ns step. Where the remaining 40 ns goes is still not
known: the first rerun's diagnostics, on 13cefd6, stand (per round 151 more instructions, 4.2 more L1 data misses and
7.3 more loads blocked by an older store; `ControlBlock` from 704 to 944 bytes; taking RACK's per-ACK update out gave
back 20 ns at p50), and no build with several edits together has been measured, so how the cost divides among the
scoreboard, RACK, the probe and the undo is unresolved. The proposal stands: an optimization pass of its own, measured
against these files, with the two kinds of change set out under the first rerun, the local ones (the layout of
`ControlBlock`, `Freed` and what is built from it passed without a round trip through memory, a freed entry reset field
by field) and a short path for an in-order ACK, which changes the order of an acknowledgement's steps. The author
decides on both.

### Rerun of step 6's first part: 2026-11-28, the hardening against `main`, host polaris

**The bar is met at p50 and missed at p99 in the headline runs.** The bar is no change beyond run-to-run noise, 10 to
20 ns, at p50 and p99 of the 64-byte tick at level 0, cross-core. Against `main` at the branch point, 7dae733, run
alternately with it, that tick is 490 ns at p50 on both sides, and 770 ns at p99 against 720 ns, +50 ns (medians of
three runs a side). The breakdown runs that followed each headline run do not show it: their round is 520 against
540 ns at p50 and 830 ns on both sides at p99, and the `tcp` leg 140 ns at p50 on both sides and 240 against 250 ns at
p99; no leg moved by more than one 10 ns step. Neither do runs of the gated configuration alone, below. No fix was
attempted.

**What the step changes on the measured path.** The first checks add to an in-order segment the comparison that tells
a segment with no room from an in-order one, which the edge test already makes, and to an in-order ACK one addition,
the sum of SND.UNA's advances; the bound on an old ACK, the limiter and the challenge are read only for a segment that
fails a check, which the venue never sends. A send adds nothing, and an open hashes twelve bytes once. The venue's ARP
entry differs between the sides as each side runs: the branch knows the venue from a static entry with learning off,
`main` seeds a learned entry before the run with lifetimes of 24 and 48 hours, which never refreshes within a run
either, so the kind of entry is not what the comparison measures. The sanity check's lists gained the step's drop and
refusal counters, and every run left them at zero.

**The runs.** The machine and cores of the recorded run, kernel 7.3.4:

- Both sides were built with the `release` preset and `-DDO_BENCHMARKS=ON`, each configured afresh so that its files
  record its revision: `main` at 7dae733 in a worktree, and the branch's head, 72df4da, in place. The commits after
  72df4da change documentation only.
- The runs were unprivileged, on 64 free hugepages reserved beforehand, with a browser and the desktop open and the
  CPUs about 98 % idle.
- There were three rounds. Each ran a headline run of `main`, then one of the branch, `--topology both --hop`, then a
  breakdown run of each in the same order, `--level 0 --breakdown --topology both`. The twelve runs took 30 seconds.
- Medians as in step 5's reruns: for each side a file of the field-by-field median of every row over its three runs,
  compared with `compare-benchmark.py`.

The files under `results/`:

- `2026-11-28-7dae733-polaris.json`, `-run2` and `-run3` are `main`'s headline runs, and `-breakdown`,
  `-run2-breakdown` and `-run3-breakdown` the breakdown runs that followed each;
- `2026-11-28-72df4da-polaris.json`, `-run2` and `-run3`, with `-breakdown`, `-run2-breakdown` and `-run3-breakdown`,
  are the branch's, each made just after `main`'s of its round.

The 64-byte tick at level 0, cross-core, in nanoseconds:

| side                | run 1 p50 | run 2 p50 | run 3 p50 | median p50 | run 1 p99 | run 2 p99 | run 3 p99 | median p99 |
|---------------------|----------:|----------:|----------:|-----------:|----------:|----------:|----------:|-----------:|
| `main`, 7dae733     |       480 |       490 |       490 |        490 |       720 |       680 |       730 |        720 |
| hardening, 72df4da  |       490 |       480 |       490 |        490 |       770 |       700 |       830 |        770 |

Its legs, from the breakdown runs, medians of three:

| side                | receive | ip | tcp | application | tcp-flush | queue-flush | round | tcp p99 | application p99 |
|---------------------|--------:|---:|----:|------------:|----------:|------------:|------:|--------:|----------------:|
| `main`, 7dae733     |     120 | 50 | 140 |         180 |        10 |          40 |   540 |     250 |             320 |
| hardening, 72df4da  |     110 | 40 | 140 |         180 |        10 |          40 |   520 |     240 |             310 |

Every row of the headline, medians of three, in nanoseconds:

| round   | level   | topology   | payload | `main` p50 | branch p50 |  +/- | `main` p99 | branch p99 |  +/- |
|---------|---------|------------|--------:|-----------:|-----------:|-----:|-----------:|-----------:|-----:|
| tick    | floor   | cross-core |      64 |        230 |        230 |    0 |        270 |        290 |  +20 |
| tick    | floor   | cross-core |     256 |        390 |        380 |  -10 |        450 |        450 |    0 |
| tick    | floor   | cross-core |    1024 |        960 |        950 |  -10 |       1200 |       1160 |  -40 |
| tick    | floor   | cross-core |    1448 |       1260 |       1260 |    0 |       1590 |       1550 |  -40 |
| tick    | floor   | same-core  |      64 |        100 |        100 |    0 |        140 |        130 |  -10 |
| tick    | floor   | same-core  |     256 |        250 |        240 |  -10 |        290 |        280 |  -10 |
| tick    | floor   | same-core  |    1024 |        810 |        800 |  -10 |        880 |        870 |  -10 |
| tick    | floor   | same-core  |    1448 |       1120 |       1110 |  -10 |       1200 |       1220 |  +20 |
| tick    | level 0 | cross-core |      64 |        490 |        490 |    0 |        720 |        770 |  +50 |
| tick    | level 0 | cross-core |     256 |        660 |        650 |  -10 |        880 |        970 |  +90 |
| tick    | level 0 | cross-core |    1024 |       1360 |       1340 |  -20 |       1720 |       1810 |  +90 |
| tick    | level 0 | cross-core |    1448 |       1720 |       1720 |    0 |       2100 |       2220 | +120 |
| tick    | level 0 | same-core  |      64 |        380 |        380 |    0 |        650 |        660 |  +10 |
| tick    | level 0 | same-core  |     256 |        530 |        540 |  +10 |        800 |        860 |  +60 |
| tick    | level 0 | same-core  |    1024 |       1170 |       1180 |  +10 |       1500 |       1570 |  +70 |
| tick    | level 0 | same-core  |    1448 |       1520 |       1530 |  +10 |       1870 |       1990 | +120 |
| tick    | level 1 | cross-core |      64 |        530 |        540 |  +10 |        830 |        900 |  +70 |
| tick    | level 1 | cross-core |     256 |        710 |        750 |  +40 |        990 |       1200 | +210 |
| tick    | level 1 | cross-core |    1024 |       1370 |       1420 |  +50 |       1770 |       1990 | +220 |
| tick    | level 1 | cross-core |    1448 |       1760 |       1760 |    0 |       2310 |       2350 |  +40 |
| gateway | floor   | cross-core |     128 |         60 |         60 |    0 |         80 |         80 |    0 |
| gateway | floor   | same-core  |     128 |         30 |         30 |    0 |         50 |         50 |    0 |
| gateway | level 0 | cross-core |     128 |        150 |        160 |  +10 |        250 |        270 |  +20 |
| gateway | level 0 | same-core  |     128 |        120 |        130 |  +10 |        210 |        220 |  +10 |
| gateway | level 1 | cross-core |     128 |        170 |        170 |    0 |        290 |        290 |    0 |
| hop     | -       | cross-core |       0 |         34 |         34 |    0 |         54 |         54 |    0 |

**The noise these files show.** `compare-benchmark.py` over a pair of runs of one binary flags 8 to 16 of `main`'s
headline percentiles and 22 to 31 of the branch's, and 24 to 38 of either side's breakdown percentiles (each of the
three pairs of each side). Over the two sides' medians it flags 19 of the headline's and 19 of the breakdown's.

- The p50 of the gated tick moved by one 10 ns step within either side, 480 to 490 ns. Its p99 ran from 680 to 730 ns
  over `main`'s three runs and from 700 to 830 ns over the branch's.
- In the branch's first and third headline runs the tick rows at levels 0 and 1 have a raised p90: 590 and 670 ns at
  64 B, level 0, cross-core, where every run of `main` has 510 to 530 ns, and 950 and 930 ns at 256 B, level 1, where
  `main` has 740 to 760 ns. That is the slower of the two modes described under "Known artefacts" arriving in longer
  bursts. The branch's second run has no such rise, and its gated tick is 480 ns at p50 and 700 ns at p99, inside
  `main`'s range.
- At 256 and 1024 B level 1's p50 moved by 40 and 50 ns, rows where the two modes move a median by a whole mode.

**What else was measured.** Two more sets, the same day; their files are not kept.

- The same three rounds with the branch first in each: the gated tick at 500 against 490 ns at p50 and 720 against
  670 ns at p99, medians of three, with a raised p90 in one of the branch's three runs, 730 ns, and in none of
  `main`'s.
- `--round tick --level 0 --payload 64`, five runs a side alternating, then the same with `--breakdown`: 480 against
  490 ns at p50 and 750 ns on both sides at p99; with `--breakdown`, the round at 520 against 530 ns at p50 and 760
  against 770 ns at p99, and the `tcp` leg at 130 against 140 ns at p50 and 220 ns on both sides at p99.

**What is left.** At p50 the bar is met in every set. At p99 the headline runs give +50 ns in both orders, while the
breakdown runs and the runs of the gated configuration alone, which go through the same code, give none. Whether the
branch makes the slower mode's bursts more frequent inside the 25-configuration matrix, or these headline runs caught
more of them by chance, is not established, and the cause of the bursts is still not identified. The number stands as
recorded. The second rerun, below, did not reproduce it.

### Second rerun of step 6's first part: 2026-11-28, after the sum's type changed, host polaris

**The bar is met at p50 and at p99.** Against `main` at the branch point, 7dae733, run alternately with it as in the
first rerun, the 64-byte tick at level 0, cross-core, is 490 ns at p50 on both sides, and 720 ns at p99 against
740 ns, 20 ns lower (medians of three runs a side). The breakdown runs agree: their round is 540 against 550 ns at p50
and 760 against 780 ns at p99, and the `tcp` leg 140 against 150 ns at p50 and 220 against 230 ns at p99. The first
rerun's +50 ns at p99 did not recur.

**What changed between the two reruns.** A review of the whole branch found nothing on the tick's path at the
instruction level and named the control block's layout as the plausible cause: the sum of SND.UNA's advances, a
`std::uint64_t`, had taken `SendSequence` from 32 to 40 bytes and its alignment from 4 to 8. The sum is now
`bytes_acked`, a `std::uint32_t` that stops at the top of 32 bits, which leaves `SendSequence` at 36 bytes and an
alignment of 4. The control block did not change with it: the member after `SendSequence` is 8-byte aligned, so it and
every member behind it sit where they sat in the first rerun. Against `main` that is 8 bytes further along as far as
the SACK blocks, and 24 bytes for what stands behind the answer limiter that follows them, the two timers among it;
over the benchmark's DPDK port the block is 968 bytes in both reruns and 944 on `main`. On the tick's path the change
puts a saturating 32-bit addition where a 64-bit one was; the minimum in the bound on an old ACK is read only for an
ACK below SND.UNA. The other commits between 72df4da and the measured head, 8c5ee5b, change what the IP brick's seed
answers, the TCP stack's constructor, tests and comments, none of them on the tick's path.

**What this does and does not establish.** The first rerun's rise at p99 is not reproduced: here the gated tick's p99
runs from 710 to 760 ns over the branch's three runs and over `main`'s alike, and no run of either side has the raised
p90 that two of the branch's first three had; the gated row's p90 is 520 ns in all six. Since every member behind
`SendSequence` sits where it did in the first rerun, this set does not test the layout suspicion, and it does not show
that the change of type removed anything. It shows the branch at `main`'s p99 in a set where neither side caught the
slower mode's bursts on the gated row; it does not say why the first rerun's headline runs caught more of them on the
branch's side, and the cause of the bursts is still not identified.

**The runs.** The machine, cores and kernel of the first rerun:

- `main`'s binary is the first rerun's, built at 7dae733 and not rebuilt. The branch was configured afresh at its head,
  8c5ee5b (`cmake --preset release -DDO_BENCHMARKS=ON`), so that its files record its revision, and built with the
  `release` preset.
- The runs were unprivileged, on 64 free hugepages reserved beforehand, with a browser and the desktop open and the
  CPUs about 98 % idle.
- There were three rounds, in the first rerun's order: a headline run of `main`, then one of the branch,
  `--topology both --hop`, then a breakdown run of each in the same order, `--level 0 --breakdown --topology both`. The
  twelve runs took 30 seconds.
- Medians as in the first rerun: for each side a file of the field-by-field median of every row over its three runs,
  compared with `compare-benchmark.py`.

The files under `results/`:

- `2026-11-28-7dae733-polaris-run4.json`, `-run5` and `-run6`, with `-run4-breakdown`, `-run5-breakdown` and
  `-run6-breakdown`, are `main`'s runs of this rerun, each made just before the branch's of its round. The comparison
  reads these, not the first rerun's `main` files.
- `2026-11-28-8c5ee5b-polaris.json`, `-run2` and `-run3`, with `-breakdown`, `-run2-breakdown` and `-run3-breakdown`,
  are the branch's.

The 64-byte tick at level 0, cross-core, in nanoseconds:

| side                | run 1 p50 | run 2 p50 | run 3 p50 | median p50 | run 1 p99 | run 2 p99 | run 3 p99 | median p99 |
|---------------------|----------:|----------:|----------:|-----------:|----------:|----------:|----------:|-----------:|
| `main`, 7dae733     |       490 |       490 |       490 |        490 |       710 |       740 |       760 |        740 |
| hardening, 8c5ee5b  |       490 |       490 |       490 |        490 |       710 |       720 |       760 |        720 |

Its legs, from the breakdown runs, medians of three:

| side                | receive | ip | tcp | application | tcp-flush | queue-flush | round | tcp p99 | application p99 |
|---------------------|--------:|---:|----:|------------:|----------:|------------:|------:|--------:|----------------:|
| `main`, 7dae733     |     120 | 50 | 150 |         180 |        10 |          40 |   550 |     230 |             280 |
| hardening, 8c5ee5b  |     110 | 50 | 140 |         170 |        10 |          40 |   540 |     220 |             260 |

The same tick and its `tcp` leg in both reruns, medians of three, in nanoseconds:

| side                            | p50, first | p50, now | p99, first | p99, now | `tcp` p50, first | `tcp` p50, now | `tcp` p99, first | `tcp` p99, now |
|---------------------------------|-----------:|---------:|-----------:|---------:|-----------------:|---------------:|-----------------:|---------------:|
| `main`, 7dae733                 |        490 |      490 |        720 |      740 |              140 |            150 |              250 |            230 |
| hardening, 72df4da then 8c5ee5b |        490 |      490 |        770 |      720 |              140 |            140 |              240 |            220 |

Every row of the headline, medians of three, in nanoseconds:

| round   | level   | topology   | payload | `main` p50 | branch p50 |  +/- | `main` p99 | branch p99 |  +/- |
|---------|---------|------------|--------:|-----------:|-----------:|-----:|-----------:|-----------:|-----:|
| tick    | floor   | cross-core |      64 |        240 |        230 |  -10 |        280 |        280 |    0 |
| tick    | floor   | cross-core |     256 |        390 |        380 |  -10 |        450 |        440 |  -10 |
| tick    | floor   | cross-core |    1024 |        950 |        970 |  +20 |       1150 |       1180 |  +30 |
| tick    | floor   | cross-core |    1448 |       1250 |       1260 |  +10 |       1410 |       1500 |  +90 |
| tick    | floor   | same-core  |      64 |        100 |        110 |  +10 |        140 |        140 |    0 |
| tick    | floor   | same-core  |     256 |        240 |        250 |  +10 |        280 |        290 |  +10 |
| tick    | floor   | same-core  |    1024 |        810 |        810 |    0 |        870 |        870 |    0 |
| tick    | floor   | same-core  |    1448 |       1120 |       1120 |    0 |       1180 |       1200 |  +20 |
| tick    | level 0 | cross-core |      64 |        490 |        490 |    0 |        740 |        720 |  -20 |
| tick    | level 0 | cross-core |     256 |        660 |        660 |    0 |        920 |        930 |  +10 |
| tick    | level 0 | cross-core |    1024 |       1320 |       1320 |    0 |       1670 |       1670 |    0 |
| tick    | level 0 | cross-core |    1448 |       1680 |       1690 |  +10 |       2060 |       2080 |  +20 |
| tick    | level 0 | same-core  |      64 |        390 |        390 |    0 |        590 |        620 |  +30 |
| tick    | level 0 | same-core  |     256 |        540 |        550 |  +10 |        770 |        850 |  +80 |
| tick    | level 0 | same-core  |    1024 |       1180 |       1180 |    0 |       1490 |       1480 |  -10 |
| tick    | level 0 | same-core  |    1448 |       1530 |       1530 |    0 |       1900 |       1870 |  -30 |
| tick    | level 1 | cross-core |      64 |        540 |        550 |  +10 |        880 |        860 |  -20 |
| tick    | level 1 | cross-core |     256 |        720 |        720 |    0 |       1030 |       1050 |  +20 |
| tick    | level 1 | cross-core |    1024 |       1360 |       1370 |  +10 |       1730 |       1810 |  +80 |
| tick    | level 1 | cross-core |    1448 |       1720 |       1730 |  +10 |       2160 |       2210 |  +50 |
| gateway | floor   | cross-core |     128 |         60 |         60 |    0 |         80 |         80 |    0 |
| gateway | floor   | same-core  |     128 |         30 |         30 |    0 |         50 |         50 |    0 |
| gateway | level 0 | cross-core |     128 |        160 |        160 |    0 |        240 |        230 |  -10 |
| gateway | level 0 | same-core  |     128 |        120 |        130 |  +10 |        190 |        220 |  +30 |
| gateway | level 1 | cross-core |     128 |        180 |        180 |    0 |        260 |        270 |  +10 |
| hop     | -       | cross-core |       0 |         34 |         34 |    0 |         54 |         60 |   +6 |

**The noise these files show.** `compare-benchmark.py` over a pair of runs of one binary flags 3 to 9 of `main`'s
headline percentiles and 3 to 10 of the branch's, and 5 to 8 of `main`'s breakdown percentiles and 5 to 15 of the
branch's (each of the three pairs of each side), fewer than in the first rerun. Over the two sides' medians it flags 4
of the headline's and 1 of the breakdown's. No p50 of a level 0 or level 1 row moved by more than one 10 ns step. The
largest moves are at p99, +80 to +90 ns on three rows, one of them the floor's at 1448 B cross-core, which runs no stack
code.

## Tracking regressions

Every step of phase 2 reruns both baseline commands on the same machine and the same cores, and commits both
result files beside the baseline's under `docs/benchmarks/results/`. A step is gated on its headline file, compared
with the baseline's headline file; its breakdown file is compared with the baseline's breakdown file to see which
leg moved:

```bash
./scripts/compare-benchmark.py docs/benchmarks/results/<baseline>.json docs/benchmarks/results/<step>.json
./scripts/compare-benchmark.py docs/benchmarks/results/<baseline>-breakdown.json \
    docs/benchmarks/results/<step>-breakdown.json
```

The script, Python 3 with the standard library only, matches rows by round, level, topology, payload and
metric, and prints both files' percentiles with the change of each. By default it gates p50, p90 and p99: a change
in either direction beyond `--threshold` percent, 5 by default, and beyond `--min-ns` nanoseconds, 20 by default,
is marked with `!` and counted. The floor in nanoseconds is there because the TSC steps in 10 ns on the author's
machine: one step alone moves a 30 to 60 ns row, a short breakdown leg say, by 17 to 33 % between identical runs,
so a row may move by up to two steps unmarked. A zero baseline is flagged when the candidate is above the floor. p99.9
and max are printed without a mark, because they rest on a hundred rounds and on one; `--tails` gates them too. The
script exits 1 when a gated percentile moved or a baseline row is missing from the candidate, so a step can gate
on it. It prints both machines and builds and gates on neither, but a warning line says when the two files differ
in CPU, in build or in a row's count: a baseline is only read against a run of the same machine.

A same-machine rerun that flags lines at p50 means the machine was not quiet: note which lines rather than raising
the threshold. p99.9 is read by eye in the history table below, or gated with `--tails` when a step means to move
the tail. A change the step's added work does not explain gets a `perf record -g` session on the `release-perf`
preset, which keeps frame pointers, narrowed to the configuration that moved and run on hugepages like any other:

```bash
cmake --preset release-perf && cmake --build --preset release-perf --target Aloe.Benchmarks.TickToTrade
perf record -g -o /tmp/tick-to-trade.perf.data ./build/release-perf/benchmarks/tick_to_trade/tick_to_trade \
    --round tick --level 0 --payload 64 --out /tmp/tick-to-trade-perf.json
perf report -i /tmp/tick-to-trade.perf.data
```

Each step adds a line here, read from its headline file: the date, the revision, the step, and p50, p99 and p99.9
of level 0, cross-core, for the tick round at 64 bytes, then p50 and p99.9 for the gateway round. p99 is there
because the gate reads it.

| Date       | Revision | Step                                                          | Tick, 64 B, p50 | Tick, 64 B, p99 | Tick, 64 B, p99.9 | Gateway, p50 | Gateway, p99.9 |
|------------|----------|---------------------------------------------------------------|-----------------|-----------------|-------------------|--------------|----------------|
| pending    | pending  | 1. Baseline benchmark                                         | pending         | pending         | pending           | pending      | pending        |
| 2026-11-21 | 1aeddb9  | `main` at step 5's base, medians of 3                         | 460             | 670             | 1310              | 150          | 300            |
| 2026-11-21 | 13cefd6  | 5. Loss recovery with SACK, medians of 3: the gate is not met | 500 (+40)       | 740 (+70)       | 1300              | 160          | 320            |
| 2026-11-21 | 1aeddb9  | `main` at step 5's base, beside the cleanup, medians of 3     | 450             | 670             | 970               | 150          | 290            |
| 2026-11-21 | bbcd1dc  | 5. After the tick path's cleanup, medians of 3: not met       | 490 (+40)       | 730 (+60)       | 1250              | 160          | 320            |
| 2026-11-28 | 7dae733  | `main` at step 6's base, medians of 3                         | 490             | 720             | 1240              | 150          | 320            |
| 2026-11-28 | 72df4da  | 6. The hardening, medians of 3: p50 met, p99 not              | 490 (0)         | 770 (+50)       | 1310              | 160          | 410            |
| 2026-11-28 | 7dae733  | `main` at step 6's base, second rerun, medians of 3           | 490             | 740             | 1240              | 160          | 320            |
| 2026-11-28 | 8c5ee5b  | 6. The hardening, second rerun, medians of 3: met             | 490 (0)         | 720 (-20)       | 1240              | 160          | 310            |
 
Step 3 (reliable send) gates on the 64-byte tick at level 0, cross-core: p50 and p99 within 50 ns of a run of `main`
at the branch's merge base (`git worktree add ../aloe-base $(git merge-base main HEAD)`, built with the `release`
preset), taken on the same day and machine as the branch's run. The recorded baseline of 2026-10-21 is not the
comparison point, because it predates #24's copy of shared mbufs on receive. A delta over 50 ns at p50 or p99 is
investigated before the pull request, and the finding goes in this page either way. The step 3 rerun is owed: it
needs hugepages and root. Both result files go to `docs/benchmarks/results/<date>-<sha>-<host>.json`, one named
for the base revision and one for the branch's.

The receive side (out-of-order segments held as packets, SACK blocks, window scaling and the optional delayed ACK)
gates the same way, on a tighter bar: p50 and p99 of the 64-byte tick at level 0, cross-core, may move no more than
the run-to-run noise, 10 to 20 ns. Its rerun is owed too, since the machine it was built on had no hugepages and no
root to reserve them. It is made on one machine with the `release` preset and 64 hugepages: `main` at the branch's
base, 6a779fb, built in a worktree, and the branch's head, each with `-DDO_BENCHMARKS=ON`; three runs of each
alternating, base first, with the options of the recorded run above; compared by the medians of the three runs, p50
and p99, with the `tcp` leg of a `--breakdown` run beside them. A change beyond the bar is found and fixed before the
merge, or recorded on this page with its number and its reason; the six result files go under
`docs/benchmarks/results/`, named for their revisions. If the tick moves, the places to look first are what the receive
side added to it: the one question an in-order segment asks before it is held, whether anything is held above; the store
that forgets a pending duplicate SACK on every segment with payload; the load of whether SACK blocks are owed on every
send; the offer's compare against the silly-window threshold and the two shifts that write the scaled field; and the
doubling test in `consume`. The first fix to reach for is the one TLDK and Onload have: recognise an option block that
is exactly `nop, nop, TS` with one 32-bit compare and read its two values directly, in place of the walk over the
options every segment takes today.

Step 5 (loss recovery with SACK: the scoreboard, RACK-TLP, one recovery with PRR and the undo) gates the same way, on
the same bar, against `main` at its base, 1aeddb9. Its rerun was made on hugepages and is recorded under "Rerun of
step 5" above: the bar is not met, the 64-byte tick at level 0, cross-core, being 40 ns slower at p50 and 70 ns at p99.
What the diagnostics measured of its cause, what they did not isolate, and the changes that could win it back are
recorded there. The work a review then found plainly unnecessary on the tick path was taken out, and the rerun after
it, under "Rerun after the tick path's cleanup", still misses the bar: 40 ns slower at p50 and 60 ns at p99. Winning the
rest back is an optimization pass of its own.

Step 6's first part (the hardening: keyed initial sequence numbers, RFC 5961's checks and their limited answers, the
handshake's and the window's edge cases, and ARP from a static table by default) gates the same way, on the same bar,
against `main` at its base, 7dae733. Its first rerun is recorded under "Rerun of step 6's first part" above: the gated
tick's p50 is unchanged, 490 ns, and its p99 is 50 ns higher in the headline runs and level in the breakdown runs and in
runs of that configuration alone. The second, under "Second rerun of step 6's first part", met the bar at both: 490 ns
at p50 on both sides and 720 against 740 ns at p99. The step's later parts gate against `main` at the same base.

## What comes next

The second part adds loads on the critical shard, venue responses landing inside the window, idle connections, a burst
ahead of the tick, a publishing stream and, under `--arp-on-wire`, where the venue is learned rather than static, ARP
refreshes, and `--isolate`, which moves them to a second shard on a core of its own. Loaded against clean is what
co-location costs the tail and isolated against clean what a second shard recovers; the remainder, less the hop, is the
most a shard spanning two cores could win, and those numbers decide whether one is built.
