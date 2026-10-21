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
sends is built outside the timed window, on its own core.

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

- any drop, refusal or allocation-failure counter of the stack, TCP's, IP's or the shard's, or TCP's count of
  retransmissions, is not zero at the end of a configuration; only the waits for ARP and IP's refusal of
  unsolicited ARP replies are allowed;
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
- `--payload` lists the tick payloads, from 8 to 1460 bytes, `64,256,1024,1448,1460` by default. The gateway
  round's payload is the 128-byte order.
- `--rounds N` and `--warmup N` set the measured rounds and the discarded rounds before them, 100 000 and
  10 000 by default.
- `--gap NS` pauses that many nanoseconds between rounds, 0 by default. The deadlines grow with it.
- `--topology cross|same|both`. `cross`, the default, puts the venue on its own core. `same` runs the venue
  inline on the stack's thread, strictly in turn with the stack, for the floor and level 0; level 1 is a
  `Runtime`, which owns its thread, so it has no same-core configuration, and `--level 1 --topology same` is
  refused for leaving nothing to run. `both` runs every cross-core configuration and every same-core one.
- `--breakdown` stamps the phases of level 0, as "Reading the table" below describes.
- `--hop` measures the bare cache-line hop between the two cores after the matrix.
- `--arp-on-wire` makes the stack resolve the venue by ARP instead of learning its MAC with `Ipv4::learn` before
  the run. It is refused with `--topology same` or `both`: on one thread, the inline venue answers the stack's
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

The first is the headline. With the default cores, payloads and counts it is 30 configurations, 25 of
tick-to-trade (the floor and level 0 in both topologies and level 1 cross-core, five payloads each) and 5 of the
gateway round, then the hop. It runs without `--breakdown`, because the phase stamps cost level 0 time of their
own. The second carries the legs: level 0, cross-core, the five tick payloads and the gateway round, six
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
- **The 1460 and 1448 rows.** 1460 bytes is a full segment at the MSS of 1460. 1448 is what a full segment
  becomes once phase 2's timestamps option takes 12 bytes of it, so it is the row later steps compare against.
  The 1460 row carries no second-segment ACK: `tcp::Stack` resets its count of full segments whenever it sends
  an ACK, and every order carries one, so in a closed loop RFC 5681's ACK of every second full-sized segment
  never fires. The venue still skips pure ACKs when it matches an order to its tick, as a defence.
- **Level 1 is cross-core only.** `--topology same` covers the floor and level 0, because a `Runtime` owns its
  thread and cannot share it with the venue.
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

| round | level | topology | payload | min | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| tick | floor | cross-core | 64 | 179 | 229 | 249 | 289 | 449 | 8449 |
| tick | floor | cross-core | 256 | 329 | 399 | 449 | 489 | 799 | 8789 |
| tick | floor | cross-core | 1024 | 889 | 969 | 1019 | 1179 | 3049 | 22819 |
| tick | floor | cross-core | 1448 | 1199 | 1249 | 1319 | 1519 | 3479 | 18009 |
| tick | floor | cross-core | 1460 | 1209 | 1259 | 1329 | 1569 | 3469 | 15159 |
| tick | floor | same-core | 64 | 89 | 99 | 119 | 149 | 209 | 4719 |
| tick | floor | same-core | 256 | 229 | 239 | 279 | 339 | 479 | 6179 |
| tick | floor | same-core | 1024 | 799 | 809 | 889 | 959 | 1919 | 5909 |
| tick | floor | same-core | 1448 | 1109 | 1209 | 1249 | 1319 | 3729 | 20579 |
| tick | floor | same-core | 1460 | 1119 | 1219 | 1259 | 1329 | 4409 | 21269 |
| tick | level 0 | cross-core | 64 | 309 | 359 | 439 | 529 | 849 | 6169 |
| tick | level 0 | cross-core | 256 | 479 | 579 | 719 | 809 | 1729 | 14399 |
| tick | level 0 | cross-core | 1024 | 1139 | 1399 | 1559 | 1729 | 4329 | 17199 |
| tick | level 0 | cross-core | 1448 | 1499 | 1669 | 1869 | 2079 | 4249 | 27559 |
| tick | level 0 | cross-core | 1460 | 1529 | 1759 | 1979 | 2219 | 4719 | 16539 |
| tick | level 0 | same-core | 64 | 209 | 229 | 239 | 349 | 469 | 4249 |
| tick | level 0 | same-core | 256 | 369 | 389 | 399 | 599 | 799 | 4959 |
| tick | level 0 | same-core | 1024 | 1009 | 1039 | 1229 | 1349 | 3309 | 8369 |
| tick | level 0 | same-core | 1448 | 1359 | 1389 | 1609 | 1709 | 3979 | 13139 |
| tick | level 0 | same-core | 1460 | 1369 | 1389 | 1429 | 1689 | 3789 | 17529 |
| tick | level 1 | cross-core | 64 | 349 | 379 | 409 | 569 | 829 | 10279 |
| tick | level 1 | cross-core | 256 | 499 | 599 | 759 | 859 | 1719 | 8409 |
| tick | level 1 | cross-core | 1024 | 1159 | 1439 | 1659 | 1799 | 4159 | 24759 |
| tick | level 1 | cross-core | 1448 | 1519 | 1679 | 1879 | 2099 | 4049 | 22609 |
| tick | level 1 | cross-core | 1460 | 1519 | 1689 | 1989 | 2119 | 4239 | 27779 |
| gateway | floor | cross-core | 128 | 49 | 59 | 69 | 89 | 129 | 5019 |
| gateway | floor | same-core | 128 | 19 | 29 | 39 | 49 | 79 | 5299 |
| gateway | level 0 | cross-core | 128 | 99 | 109 | 109 | 169 | 199 | 7989 |
| gateway | level 0 | same-core | 128 | 69 | 79 | 89 | 149 | 189 | 15789 |
| gateway | level 1 | cross-core | 128 | 109 | 119 | 119 | 189 | 239 | 3579 |
| hop | - | cross-core | 0 | 24 | 34 | 49 | 54 | 59 | 1879 |

The breakdown, cross-core, p50 per leg in nanoseconds; the `round` column is the breakdown run's own round, which
carries the stamps' cost:

| round | payload | receive | ip | tcp | application | tcp-flush | queue-flush | round |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| tick | 64 | 99 | 49 | 59 | 119 | 9 | 39 | 389 |
| tick | 256 | 119 | 49 | 139 | 259 | 9 | 39 | 609 |
| tick | 1024 | 109 | 49 | 269 | 819 | 9 | 39 | 1319 |
| tick | 1448 | 129 | 49 | 349 | 1129 | 9 | 39 | 1719 |
| tick | 1460 | 129 | 49 | 349 | 1139 | 9 | 39 | 1729 |
| gateway | 128 | - | - | - | 69 | 9 | 39 | 129 |

The hop between cores 2 and 3: 34 ns at p50, 54 ns at p99.

What they say. At 64 B, cross-core, the stack costs 130 ns at p50 and 400 ns at p99.9 over the floor, and the
runtime 20 ns at p50 and nothing measurable at p99.9 over the bricks; in the gateway round the stack costs 50 ns at
p50 over the floor and the runtime 10 ns over the bricks. The hop is 34 ns one way, and the floor cross-core
against same-core at 64 B is 130 ns: a frame is several lines. From 1024 B up the application's fold over every
byte sets the scale, about 0.75 ns a byte, and the `tcp` leg's checksum is the second cost; the receive, IP and
flush legs do not grow with the payload.

The repetitions, p50 of the cross-core tick round in nanoseconds:

| level | payload | run 1 | run 2 | run 3 | earlier |
|---|---:|---:|---:|---:|---:|
| floor | 64 | 229 | 229 | 229 | 199 |
| floor | 256 | 399 | 379 | 379 | 349 |
| floor | 1024 | 969 | 989 | 989 | 939 |
| floor | 1448 | 1249 | 1249 | 1309 | 1229 |
| floor | 1460 | 1259 | 1259 | 1259 | 1249 |
| level 0 | 64 | 359 | 349 | 339 | 359 |
| level 0 | 256 | 579 | 549 | 559 | 559 |
| level 0 | 1024 | 1399 | 1289 | 1389 | 1399 |
| level 0 | 1448 | 1669 | 1669 | 1669 | 1859 |
| level 0 | 1460 | 1759 | 1679 | 1679 | 1879 |
| level 1 | 64 | 379 | 389 | 389 | 379 |
| level 1 | 256 | 599 | 589 | 589 | 579 |
| level 1 | 1024 | 1439 | 1439 | 1449 | 1279 |
| level 1 | 1448 | 1679 | 1859 | 1899 | 1679 |
| level 1 | 1460 | 1689 | 1889 | 1909 | 1689 |

Level 0's median at 1448 B held at 1669 ns over three runs while level 1's moved from 1679 to 1899 ns, and the
earlier run had level 0 at 1859 ns against level 1 at 1679 ns: the two modes described under "Known artefacts",
not a change in the code, and the reason the level comparison from 1024 B up is read at the minimum and at the
tail. `compare-benchmark.py` flags six p50 lines between run 1 and run 2, every one of them a mode shift or the
same-core floor at 1448 and 1460 B. The gap run leaves every p50 at one of its two modes and raises the p99.9 of
most lines, the cross-core floor at 64 B from 449 to 1799 ns: five idle microseconds between rounds cost the tail.

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

Each step adds a line here, read from its headline file: the date, the revision, the step, and p50 and p99.9 of
level 0, cross-core, for the tick round at 64 bytes and for the gateway round.

| Date    | Revision | Step                  | Tick, 64 B, p50 | Tick, 64 B, p99.9 | Gateway, p50 | Gateway, p99.9 |
|---------|----------|-----------------------|-----------------|-------------------|--------------|----------------|
| pending | pending  | 1. Baseline benchmark | pending         | pending           | pending      | pending        |

## What comes next

The second part adds loads on the critical shard, venue responses landing inside the window, idle connections,
a burst ahead of the tick, a publishing stream and ARP refreshes, and `--isolate`, which moves them to a second
shard on a core of its own. Loaded against clean is what co-location costs the tail and isolated against clean
what a second shard recovers; the remainder, less the hop, is the most a shard spanning two cores could win, and
those numbers decide whether one is built.
