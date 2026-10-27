# Packet loan, scripted impairment and a scenario-script runner

Status: design agreed in the brainstorm of 2026-10-22, written up for a builder check and then for review
before the plan. Step 2 of roadmap phase 2; tracks GitHub issue #17. Builds on the baseline benchmark
(PR #22, merged as e084351). Supersedes the draft `2026-10-17-retransmit-design-draft.md` for the loan's
device side; that draft's TCP side (the retransmission queue, the busy rule, the in-place patch, the bounded
copy) is issue #18 and stays decided as written there. The issue's premises were checked against the code and
the DPDK 26.03 sources before this document; where a premise failed, this document says so and the issue is
corrected when the work lands.

## Goal

Full TCP resends lost data without copying it. Today `commit` hands the packet to IP and the stack forgets
it; resending means keeping the sent packet until the peer acknowledges it, while the device may still be
reading it. This step builds what that stands on, and the tools every later step of phase 2 is tested with:

- **The packet loan.** A packet the stack must keep after sending is lent to the device and comes back when
  the device is done with it. The stack sees ownership move and return, never a reference count. Each
  backend implements the loan its own way; the device conformance suite covers both.
- **Scripted impairment in the fabric.** Loss, duplication, reordering and delay, deterministic and seeded,
  so two Aloe stacks can be made to lose segments from each other in a unit test.
- **A runner for scenario scripts** in packetdrill's syntax: inject segments at given times, assert what the
  stack sends and when, drive the application side with the script's calls. The scripts are Aloe's own;
  Linux's packetdrill corpus is the reference for what Linux does, read and never copied.

TCP's behaviour does not change in this step. `commit` still hands the packet to IP and forgets it; the
first lend from TCP is #18. The first set of scripts therefore asserts the TCP that exists today.

## Goals, non-goals, anti-goals

Goals:

- `device::IsPacket` and `device::IsDevice` extended with the loan and the reclaim, implemented by
  `ethdev::Packet`/`ethdev::Port` and `fabric::Packet`/`fabric::Port`, covered by the conformance suite on
  both backends: lent, readable while lent, back once the device is done, freed exactly once, a loan that
  outlives its lender, and the reclaim's result consistent with what the device reports.
- The ring driver's loopback made safe for the loan without discipline on the reader.
- DPDK's fast-free transmit offload kept off as a documented, build-checked invariant.
- The fabric's impairment: a seeded random model and a deterministic script, with counters, and a clock
  step that releases delayed frames and returns loans.
- The `scenario` fixture module: a parser for the script subset below, a runner over the TCP brick on the
  fabric, and failure reports a person can read without a debugger.
- A first set of scripts, handshake, data, close and reset, passing against the current TCP in both
  checksum modes of the fabric.
- The docs: the device page, the fixtures page, a page for the new module, the roadmap's step 2 wording.

Non-goals:

- Any change to what `tcp::Stack` sends or keeps. No retransmission queue, no send cap, no
  `Ipv4::send` that keeps the packet; those are #18, and this document says what they will need.
- Running scripts against the DPDK backend or a kernel peer. The runner is a fabric tool; tap and netem
  tests stay hand-written manual tests.
- A full packetdrill: no shell commands, no IPv6, no UDP or ICMP lines, no `TCP_INFO`, no epoll. The parser
  accepts the grammar subset below and the runner fails loudly on anything else.
- Impairment in the DPDK backend or in the runtime. The fabric is the one place a test shapes the wire.
- Changes to the benchmark. The benchmark's venue peer is affected by the loan only once TCP lends (#18);
  the consequence is recorded below.

Anti-goals, which would make this a failure even if it built:

- A reference count that shows through to TCP, in the concept or in the docs. The vocabulary is lend,
  lent and back.
- A copy on the data path of a real card. The one copy this step adds runs only when a received mbuf is
  shared, which no card ever delivers.
- Scripts that pass because they assert nothing: an outbound line matches flags, sequence, length and
  whatever else it states, and an unexpected segment from the stack fails the script.
- A GPL-licensed line in the repository. Scripts are written from the RFCs and from Aloe's own behaviour,
  with the corpus as a reading reference.
- A test that depends on wall-clock time. The fabric and the runner run on the tests' virtual stamps.

## What the issue assumed, checked

- **The reference count is the hook, and it works as assumed.** `rte_pktmbuf_prefree_seg` (DPDK 26.03,
  `lib/mbuf/rte_mbuf.h`) decrements a count above one and frees nothing; only a count of one frees the
  mbuf. A driver's free of a transmitted mbuf whose count was raised to two leaves it at one, in the
  stack's hands. There is no transmit-completion event anywhere in `rte_ethdev.h`.
- **`rte_eth_tx_done_cleanup` exists and returns `-ENOTSUP` from every CI driver.** None of `net_ring`,
  `net_null` or `net_tap` implements `tx_done_cleanup`. The drivers that do, in 26.03: Intel's e1000, igb,
  igc, i40e, iavf, ice and ixgbe; hns3, cnxk, ena, netvsc, ngbe, txgbe, r8169, hinic3 and vhost. Not mlx5.
  So the reclaim is a real tool on common Intel cards and nothing on Mellanox, and the device reports which.
- **"Drivers return packets lazily" is true of cards and false of every CI driver.** `eth_null_tx` frees
  the burst inside the transmit call (`rte_pktmbuf_free_bulk`); the tap driver frees each mbuf after writing
  it to the tap; `eth_ring_tx` never frees: it enqueues the mbuf pointer and the receiver frees it. On CI a
  loan is back at transmit (null, tap) or when the receiver lets go (ring). The "still lent long after the
  frame left" case is a hardware behaviour, reproduced on CI only by the fabric holding loans on purpose.
- **The ring driver hands the very mbuf to the receiver.** Confirmed in `eth_ring_tx`. A lent packet reaches
  the reader as the mbuf the stack holds, so any trim by the reader moves the stack's offsets. The fix is
  below, in ethdev's receive, not in the readers.
- **Fast-free is off and no CI driver offers it.** `port.cpp` masks the driver's transmit offloads with
  checksum bits only; `net_ring` advertises multi-segment only, `net_null` multi-segment and lock-free, tap its
  queue offloads. The invariant cannot be tested at run time on CI, so it becomes a `static_assert` beside
  the mask and a documented rule.
- **A loan can outlive its connection, and the packet level handles it.** With the count on ethdev and an
  orphan mark on the fabric, dropping the lender while lent frees the buffer when the device returns it; TCP
  in #18 just drops the packet.
- **The fabric already gives the receiver its own copy.** `Port::transmit` copies the frame into the
  destination queue today; holding the lent handle beside that copy is the only addition.
- **Not foreseen: the licence.** packetdrill and its corpus are GPL-2.0 (`google/packetdrill`, LICENSE and
  README: "The code is GPLv2"), and the kernel's in-tree scripts under
  `tools/testing/selftests/net/packetdrill/` carry `SPDX-License-Identifier: GPL-2.0`. Aloe is BSD-3-Clause.
  Decided: Aloe's own scripts, the corpus as a reference (author, 2026-10-22).
- **Not foreseen: the benchmark's venue peer rewrites the stack's mbuf.** `VenuePeer` writes the next tick
  into "the carrier, the mbuf the stack's last frame arrived in". Once TCP lends its order packets, the
  carrier is the stack's retransmission copy. The ethdev receive rule below makes the peer's copy private by
  itself; what that does to the mbuf flow is recorded under consequences for #18.
- **The script syntax, read from `syntax.md` of `google/packetdrill` and two scripts.** Event times are
  `+t` (relative to the previous event), `t` (absolute), `t1~t2` and `+a~+b` (a window), `*` (any time).
  Packet lines are `<` for inbound and `>` for outbound, then flags (`S`, `.`, `P.`, `F.`, `R`, `R.`; a
  trailing `.` is ACK, `.` alone is a pure ACK), `first:last(length)`, optional `ack n`, `win n`, `urg n`,
  and an option list in angle brackets (`mss n`, `nop`, `eol`, `sackOK`, `wscale n`, `sack a:b,c:d`,
  `TS val v ecr e`) or `<...>` for any options. Sequence numbers are relative: outbound to the stack's ISN,
  inbound to the peer's. Syscall lines are `name(args) = result` with `...` for an argument the script does
  not care about, and `// comments`. A backtick line runs a shell command; the runner has no shell.

## Constraints

- The rules in `AGENTS.md` apply unchanged: nothing below the runtime names stdexec or quill; DPDK only
  through `Aloe::Dpdk` and only inside `ethdev`; `core::TimePoint` for every stamp; `std::expected` on hot
  paths; one class, one place; public headers define no macros; a module's namespace is its name.
- The hot paths of `ethdev::Port` and `ethdev::Packet` gain at most a load and a branch per packet. The
  conformance suite's existing tests pass unchanged on both backends.
- The fabric stays a fixture: locks and heap allocations are fine there, as they are today, and nothing in
  `common/` depends on it.
- The new module is a fixture: it lives under `fixtures/scenario/`, links `common` and the other fixtures,
  and contains no gtest; Aloe's test binary wraps it.
- `debug`, `gcc-debug`, `asan` and `tsan` build and pass, and the format check passes. The tap manual test
  gains the loan case and is run once as root before the PR is marked ready.
- The benchmark builds and its tests pass (`-DDO_BENCHMARKS=ON`), since `StampedDevice` models the device
  concept and gains the new member.

## The packet loan

### The concept

`device::IsPacket` gains two members:

```cpp
{ packet.lend() }        -> std::same_as<P>;     // a second handle over the same buffer, for the device
{ const_packet.lent() }  -> std::same_as<bool>;  // true while the handle from lend() is alive somewhere
```

The vocabulary: the stack *lends* a packet by taking a handle from it and transmitting the handle. The
packet is *lent* while that handle exists: in the shard queue's ring, in the driver, or in the fabric's
in-flight list. It is *back* when the handle is gone, which `lent()` reports as false. The lender never
gives up its own handle, so there is no ticket to redeem: the packet in the stack's hands is the title,
and `lent()` says whether the device is still reading it. This is the loan of the issue, spelled on the
packet rather than on the device, because the shard queue's ring and the device's `transmit` then need no
change: a handle from `lend()` is a packet like any other to them.

The contract, kept by both backends and by every caller:

- `lend()` is called on a non-empty packet that is not lent, after the frame is complete: the headers are
  written and `set_tx` has been called. One handle at a time; a second `lend()` while lent is a contract
  violation, asserted. The handle carries the same data and the transmit metadata as of the call.
- While lent, the lender reads the packet and does not write it: `data()`, `size()`, `rx()` and `tx()` are
  fine; `prepend`, `append`, `trim_front`, `trim_back` and `set_tx` are violations, asserted in both
  backends. Reading a lent packet is what #18's copy fallback will do.
- What the device does to a lent buffer: it reads it, and when the transmit metadata asked for a checksum
  fill it may complete the checksum fields in place, as the tap driver and the fabric do and a card does
  not. The size, the offsets and every other byte stay as lent. A lender that sends the frame again rewrites
  its headers and so recomputes the checksums anyway, writing the pseudo-header sum into the field as the
  first send did; the conformance test of unchanged bytes transmits a frame that requests no fill.
- The handle from `lend()` is transmitted or dropped, and never written either. Dropping it without a
  transmit (a refused send, a `ShardQueue::discard`) returns the loan at once.
- Dropping the lender while lent is allowed: the buffer is freed when the device returns it, exactly once.
  This is how a connection releases with packets on loan.
- `lent()` is a poll. No backend has an event for the return, because DPDK has none; the stack asks when
  it needs to know, which is at a retransmit (#18).

`device::IsDevice` gains one member and `device::Capabilities` one field:

```cpp
{ device.reclaim(queue) } -> std::same_as<std::size_t>;  // asks the driver to free finished transmits
bool tx_reclaim = false;                                 // whether reclaim does anything on this device
```

`reclaim(queue)` runs on the queue's thread, never throws, and returns how many packets the driver let go
of. On a device whose `tx_reclaim` is false it returns zero and does nothing. The loop's `ShardQueue`
gets a `reclaim()` passthrough, so a stack reaches it without naming the device.

### Ethdev

`ethdev::Packet::lend()` raises the mbuf's reference count by one and returns a `Packet` over the same
mbuf. `lent()` reads the count and reports more than one. Dropping either handle is `rte_pktmbuf_free`,
which decrements a count above one and frees at one, so the buffer goes back to its pool exactly once no
matter which handle goes last. The driver's free after transmission is the same call. The count stays
inside `ethdev`: nothing above it sees a number.

`ethdev::Port::reclaim(queue)` is `rte_eth_tx_done_cleanup(port, queue, 0)`, with a negative result
reported as zero. `Capabilities::tx_reclaim` is probed once after `rte_eth_dev_start`, by calling the
reclaim on queue 0 and reading whether the answer is `-ENOTSUP`; the field is set before the constructor
returns.

**Receive copies a shared mbuf.** In `Port::receive`, an mbuf whose reference count is above one is copied
into a fresh mbuf of the queue's pool (`rte_pktmbuf_copy`), the shared one is dropped (its count falls to
one, the lender's), and the copy is what the caller gets. A real card never delivers a shared mbuf, so on
hardware this is one load and one untaken branch per packet; on the ring driver's loopback it is the rule
that makes a lent packet safe to receive: the reader trims and frees its own copy, and the lender's
offsets never move. When the pool has no mbuf for the copy, the frame is dropped and counted in
`QueueCounters::dropped`, which ethdev has left at zero until now; its comment changes accordingly. This
replaces the issue's "a reader that never modifies what it reads": the rule costs nothing where it does
not apply and holds for every ring user, the benchmark's peer included.

**Fast-free stays off.** `port.cpp`'s transmit offload mask gains a `static_assert` that it does not
contain `RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE`, with the reason beside it: fast-free frees a transmitted mbuf
straight to its pool assuming one reference, which a lent packet does not have. The device page states the
invariant.

### The fabric

`fabric::Packet` gains a `borrowed` mark and `detail::Block` gains `lent` and `orphaned` marks. `lend()`
asserts the block is not lent, marks it, and returns a borrowed handle over the same block with the same
`begin_`, `end_` and transmit metadata. `lent()` reads the block's mark. Releasing the borrowed handle
clears the mark and, if the block is orphaned, gives it back to the pool. Releasing the lender while the
block is lent marks it orphaned and gives nothing back. `Pool`'s destructor assertion ("every packet must be
destroyed before its pool") stays and now also proves that a loan returned exactly once.

`Port::transmit` of a borrowed handle delivers the frame as today, a copy into every destination queue, and
then moves the handle into the transmitting queue's in-flight list instead of dropping it. The wire is done
with a loan when the port's loan policy, `PortConfig::loans`, says so, and the handle is released, so that
`lent()` flips, on the queue's own thread, as a DPDK driver frees a transmitted mbuf on a later burst:

- `LoanReturn::OnStep`, the default: `Fabric::advance(now)` marks every loan transmitted before the call as
  finished, and a finished handle is released by the queue's next `transmit`, `receive` or `reclaim`, the
  calls that already take the queue's lock. `reclaim(queue)` is the eager path: on an `OnStep` port it
  releases every in-flight handle of its queue, finished or not, since the fabric delivered the frame at
  transmit, and returns the count. A test that never advances or reclaims sees the loan out until the port
  is destroyed, which the `Pool` assertion then reports.
- `LoanReturn::Held`: nothing finishes on its own and `reclaim` returns zero; `Port::return_loans(queue)`,
  called on the queue's thread, releases every handle of that queue. `advance` leaves them. This is the
  driver that will not let go, for the deferred-retransmit tests of #18, and the only way on CI to make a
  packet look lent long after its frame was delivered.

The pool and the block marks stay the queue thread's, unsynchronised as today: `advance` touches only the
finished flags, under the queue's lock, and never the pool. In a single-threaded test the full tick, `advance`
and then the flush, brings the loan back; `test_fabric_threads` runs `advance` from the test thread against
transmitting threads.

`Port::loans_outstanding(queue)` counts the list, for tests. `Capabilities::tx_reclaim` is true on every
fabric port; a `Held` port reports it true and returns zero, which is what a driver that advertises the call
but has nothing finished does.

### The loop

`loop::ShardQueue::reclaim()` calls the device's reclaim on its queue and returns the count. Nothing else in
`loop` changes: `transmit` takes the borrowed handle as it takes any packet, `flush` hands it to the
device, `discard` drops it and so returns the loan. `net::Ipv4` does not change in this step.

### What #18 does with it

Recorded so the device side is shaped for it, not to be built now. TCP will keep the whole frame as sent,
headers included, so a retransmit patches in place. The lend therefore happens after IP has prepended its
headers and set the transmit metadata: `Ipv4` gains a send that takes `Packet&` and keeps it, lends inside,
and transmits the handle; `commit` calls that and puts the packet in the connection's retransmission queue.
A retransmit asks `lent()`, and when it is true calls `reclaim()` and asks again, then defers and counts,
then after the bound copies the one segment. An acknowledgement drops the packet, lent or not. `release` and
`abort` drop every packet in the queue. The send cap and the pool sizing rule are #18's.

## Scripted impairment in the fabric

### The model

A port's egress can be impaired; `Port::set_impairment(const Impairment&)` sets it, and a default
`Impairment` is no impairment. Delivery runs the script first and the random model second, per frame, in a
fixed order, so a seed reproduces a run exactly:

```cpp
namespace aloe::fabric {
    enum class Action : std::uint8_t { Deliver, Drop, Duplicate, Reorder, Delay };

    struct Verdict {
        Action action = Action::Deliver;
        core::Duration delay{};  ///< For Delay: this frame's own delay, instead of the model's.
    };

    /// What the script sees of a frame about to leave a port.
    struct Egress {
        std::uint64_t index = 0;             ///< Frames offered to this port's impairment so far, from zero.
        std::span<const std::byte> frame;    ///< The whole frame; a test parses it with frames::parse_frame.
    };

    struct Impairment {
        double loss      = 0;  ///< Probability a frame is dropped.
        double duplicate = 0;  ///< Probability a frame is delivered twice, back to back.
        double reorder   = 0;  ///< Probability a frame is held until the port's next frame has been delivered.
        core::Duration delay{};   ///< Added to every delivered frame.
        core::Duration jitter{};  ///< A uniform extra delay in [0, jitter], on top.
        std::uint64_t seed = 1;
        std::function<std::optional<Verdict>(const Egress&)> script;  ///< First word; nothing means "the model".
    };

    struct ImpairmentCounters {
        std::uint64_t dropped = 0, duplicated = 0, reordered = 0, delayed = 0;
    };
}
```

A drop is counted on the source port's impairment counters and the frame is gone; the source's `transmitted`
counter still counts it, because it left the port. A duplicate delivers the frame twice, in a row. A reorder
holds the frame on the port until the port's next frame has been delivered, then delivers it; if no next
frame comes, the next `Fabric::advance` releases it. A delay queues the frame on the fabric with a due stamp
and `advance` delivers it when due. The script decides any of these for one frame, with its own delay for
`Delay`; a script that returns nothing leaves the frame to the model. The random model draws from a
`std::mt19937_64` seeded with `seed`, as a 53-bit fraction of the engine's raw output rather than through
`std::uniform_real_distribution`, which differs between standard libraries; the same seed gives the same
sequence under clang and GCC. With no script and every probability and duration zero, `transmit` runs as it
does today plus the branches that find that out.

### The fabric's clock

The fabric has had no notion of time: delivery is immediate. Delay needs one, and so does the loan's
default return. `Fabric::advance(const core::TimePoint now)` is the step: it delivers every delayed frame due
at or before `now`, in order of due stamp and then of transmission, releases reorder holds that have waited
through a step, and marks the `OnStep` loans transmitted before the call as finished. A delay is measured from the
fabric's current stamp, which is the stamp of the last `advance` (the epoch before the first), because
`transmit` has no time parameter: a test that delays frames advances the fabric at every tick of its loop,
as the TCP harness's `advance` and the two-stack test's tick now do. `advance` is safe to call from any
thread; the frames and holds live under the fabric's and the ports' locks, as delivery does today.

## The scenario-script runner

### Module and layout

A new fixture module, `scenario` (`fixtures/scenario/`, target `Aloe::Fixtures::Scenario`, umbrella
`#include <aloe/scenario>`, namespace `aloe::scenario`). It links `tcp`, `net`, `loop`, `fabric`, `frames`,
`wire` and `core`, and no gtest: it reports results as values. The parts, one class per file:

- `Script`: the parsed model, a list of events, each with its time spec and either a packet or a call, with
  the source line for every event; the script's options.
- `parse_script(std::string_view text) -> std::expected<Script, ParseError>`: the grammar subset below.
  A `ParseError` carries the line, the column and what was expected.
- `Harness`: a fabric with the stack's port and the peer's port, the bricks over queue 0 of the stack's
  port (`ShardQueue`, `Ipv4`, `TimerWheel`, `tcp::Stack`), both MACs learned, built from a `HarnessConfig`
  (addresses, ports, the `TcpConfig`, the fabric's checksum mode).
- `Runner`: executes a `Script` over a `Harness` and returns `std::expected<void, Failure>`. A `Failure`
  names the script line, what was expected, what happened and at what time, in the script's own notation.
- `Peer`: the sequence-number bookkeeping of the other end: its ISN, the stack's ISN once seen, the
  relative-to-absolute mapping both ways, the ports in use.

Scripts live under `tests/scenarios/tcp/<area>/<name>.pkt`. One test binary, `tests/unit_tests/scenarios/`,
finds every `.pkt` under that directory at run time (the directory is a compile definition the CMake file
sets on the target), and registers one gtest per script per checksum mode of the fabric with
`::testing::RegisterTest`, named `Scenario.<area>/<name>/Checksums` and `.../NoOffloads`. CTest sees one
entry, the binary, as it does for every other test binary; `--gtest_filter=Scenario.close/*` runs one area.
A failing script prints the `Failure`.

### The script language

The runner accepts this subset of packetdrill's syntax, and fails a script at parse time for anything
outside it, naming the line:

- **Comments and blank lines.** `//` to the end of the line.
- **Option lines** before the first event: `--name=value`. The runner knows `--tolerance_usecs`; any other
  name is a parse error, so the dialect grows deliberately.
- **Backtick lines** are a parse error: the runner has no shell, and the defaults a corpus script sets that
  way are the harness's configuration.
- **Event times.** `+t`, `t`, `t1~t2`, `+a~+b` and `*`, with `t` an integer or a decimal number of seconds.
  A relative time is from the previous event's actual time (for an outbound line, the time the segment was
  seen; for the others, the time of the event). A window admits the segment anywhere in it. `*` admits it
  whenever it comes, within a bound the runner config sets (120 virtual seconds by default). Every time is
  widened by the tolerance on both sides; the default tolerance is one timer-wheel resolution, since the
  stack's timers are late by up to that much.
- **Packet lines.** `<` and `>`; flags from the letters `S`, `F`, `R`, `P`, `U`, with `.` for ACK, alone or
  as a suffix; `first:last(length)` with `last - first == length` checked at parse time; `ack n`; `win n`;
  `urg n`; options `<mss n>`, `<nop>`, `<eol>`, `<sackOK>`, `<wscale n>`, `<sack a:b,c:d,...>`,
  `<TS val v ecr e>` in any order and combination, or `<...>`. IPv4 info in parentheses or brackets, UDP,
  ICMP and IPv6 forms are parse errors.
- **Call lines.** `name(args) = result`, with an optional `errno` word after a `-1` and an optional
  `(text)` after that, as packetdrill writes `= -1 EINPROGRESS (Operation now in progress)`. Arguments are
  integers, `...`, bare words (`SOCK_STREAM`, `IPPROTO_TCP`, `SOL_SOCKET`, `SO_REUSEADDR`, `SHUT_WR`), a
  bracketed value (`[1]`) or a quoted string. The parser keeps the call generic; the runner decides which
  names it binds.

### The binding to the brick

The script is written from the stack's point of view, as packetdrill scripts are: `>` is what the stack
sends, `<` what the peer injects, and the calls are the application. The runner binds the calls to the
brick's verbs; a call it does not bind fails the script with its name and line, never silently:

| Call                                        | Binding                                                                                                                                                      |
|---------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `socket(..., SOCK_STREAM, IPPROTO_TCP) = n` | A script descriptor `n`, bound to nothing yet.                                                                                                               |
| `setsockopt(n, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0`, `TCP_NODELAY` | Accepted and ignored: Aloe reuses ports and has no Nagle. Any other option fails.                                                 |
| `fcntl(n, F_SETFL, O_RDWR\|O_NONBLOCK) = 0` | Accepted and ignored: every verb is non-blocking.                                                                                                            |
| `bind(n, ..., ...) = 0`                     | Binds `n` to the harness's listen port.                                                                                                                      |
| `listen(n, backlog) = 0`                    | `tcp.listen(port)`.                                                                                                                                          |
| `accept(n, ..., ...) = m`                   | An `Accepted` event must be pending in the stack; its connection becomes `m`. None pending fails the script.                                                 |
| `connect(n, ..., ...) = 0` or `= -1 EINPROGRESS (...)` | `tcp.connect(peer, now)`; the connection becomes `n`. A `ConnectError` fails the script.                                                          |
| `write(n, ..., count) = count`, `send(n, ..., count, 0) = count` | `prepare` and `commit` in segments of at most the MSS until `count` bytes are committed, with a recognisable pattern; fewer than `count` accepted fails the script with the number accepted. |
| `read(n, ..., count) = m`, `recv(n, ..., count, 0) = m` | `m` at most `unread().size()` is consumed; `m == 0` needs `peer_closed()` with nothing unread; `= -1 EAGAIN` needs nothing unread and the peer open. |
| `close(n) = 0`                              | On a listener, `unlisten`. On a connection, `close()`; the runner releases the slot when `Closed`, `Reset`, `TimedOut` or `Unresolved` arrives.               |
| `shutdown(n, SHUT_WR) = 0`                  | `close()`: a FIN after what was committed; the connection stays readable.                                                                                    |
| anything else                               | Fails the script: `getsockopt`, `epoll_*`, `poll`, `sendfile`, `ioctl`, `TCP_INFO` and the rest.                                                             |

The errno words a `read` or `recv` may expect: `EAGAIN` (nothing unread, the peer open), `ETIMEDOUT`
(`TimedOut`), `ECONNRESET` (`Reset` on a connection that was established), `ECONNREFUSED` (`Reset` in
`SynSent`), `EHOSTUNREACH` (`Unresolved`). `connect` returns `0` or `-1 EINPROGRESS`.

`close` always sends a FIN. Linux resets instead when unread data is pending (RFC 2525 2.17); whether Aloe
follows is a lifecycle question for #21, and no script in the first set closes with unread data.

The peer side: inbound lines become frames from the peer's MAC and address to the stack's, from the peer's
port to the stack's listen port, or to the stack's ephemeral port once its SYN has been seen. The peer's ISN
is a constant the runner chooses, not zero, so a stack that assumed zero would fail; the script's inbound
sequence numbers are relative to it and inbound acknowledgements relative to the stack's ISN, read from the
stack's SYN or SYN-ACK. Outbound numbers are checked the other way round. Both MACs are learned before the
script starts, with `arp_reachable` and `arp_expire` of an hour in the harness's `Ipv4Config`, beyond the
`*` bound and the longest schedule, so a script past the cache's default minute never meets an ARP request;
ARP never appears in a script. The frames are built with `frames::tcp_frame`, which gains raw
option bytes on `TcpSpec` for the option lists the grammar admits beyond MSS; the checksums are correct.

### Time

The runner's clock is virtual: `now` starts at the epoch and moves only as the script says, one wheel
resolution at a time. One step is: `now += resolution`, an empty `tcp.process` at the new stamp,
`wheel.advance`, `fabric.advance`, a drain of the stack's events (which is where a closed connection's slot
is released), `tcp.flush`, `queue.flush`, and then everything the peer's port received is appended to the
runner's queue of sent segments, each with its stamp. The resolution is the wheel's, one millisecond; a
script that needs finer timing is asking for timing the stack does not have.

An inbound line and a call act at their nominal time, with no tolerance: the runner steps up to that stamp,
injects or calls, then flushes and collects at the same stamp, so a `+0 >` line right after is checked
against what that action produced. A call runs after the step's process, wheel advance and event drain, so
a call whose outcome is a timer's event, written at the timer's nominal time, finds the event. `accept`,
`read` and `recv` written with a window are retried once per step until their expected result holds or the
window closes, which is how a script waits for an event without knowing the exact tick; every other call
runs once, at the window's start.

An outbound line takes the head of the queue of sent segments: the runner steps until the queue is not
empty or the line's window has closed, then matches the head against the line and checks that its stamp
lies in the window, widened by the tolerance on both sides. A window is relative to the previous event's
actual time: for an outbound line the stamp of the segment it matched, for the others the stamp of the
action. This is packetdrill's model: segments wait in order with their stamps, and the script's lines take
them in order, across the inbound lines and calls in between, so three segments from one `write` can be
acknowledged one by one. A segment the script did not expect is caught where it lands: at the next outbound
line, whose head it is and does not match; by its stamp, outside the window of the line it would have
matched; or at the end of the script, where a queue that is not empty fails it.

### Matching

An outbound line matches a segment when every stated field agrees: the flags exactly; the relative sequence
and the length; `ack`, `win` and `urg` when written; the option list byte for byte when written, any options
for `<...>`, and no options when nothing is written. Ports and addresses are not matched, as packetdrill does
not match them. The TCP and IPv4 checksums of every segment the stack sends are verified regardless of the
line, in both fabric checksum modes, so a script that passes has also checked the software and the offload
path. A mismatch names the field, the expected value and the seen value in the script's notation, with the
line number and the stamp, for example:

```
tests/scenarios/tcp/close/remote-fin-then-local-close.pkt:21: expected "> F. 1:1(0) ack 2" at +0.010 (window +0.009..+0.011)
  the stack sent "> . 1:1(0) ack 2" at +0.010: flags differ
```

### The first set of scripts

Each asserts the TCP of today, with Aloe's option list (`<mss 1460>` on SYNs, nothing elsewhere), Aloe's
retry schedule (1, 3, 7, 15 and 31 seconds after the first send, `TimedOut` at 63) and no delayed ACK, with a
comment at the top saying what it checks and, where Linux would differ today, how. Later steps change the
scripts as the behaviour lands, which is the point of having them first.

- `handshake/passive-open.pkt`: listen, SYN in, SYN-ACK out with `<mss 1460>`, ACK in, `accept`.
- `handshake/active-open.pkt`: `connect`, SYN out, SYN-ACK in, ACK out.
- `handshake/syn-retransmitted-on-schedule.pkt`: no reply; the SYN again at +1, +2, +4, +8, +16; then the
  descriptor reports it: `+32~+32.01 read(3, ..., 1) = -1 ETIMEDOUT`.
- `handshake/syn-ack-retransmitted-on-schedule.pkt`: the passive side's schedule.
- `handshake/syn-to-closed-port-is-reset.pkt`: no listener; `R.` with `ack 1`.
- `handshake/stray-ack-in-listen-is-reset.pkt`: an ACK with no connection; `R` at its acknowledgement.
- `data/inbound-data-is-acked-at-flush.pkt`: `P.` in, pure ACK out at `+0`, `read` returns it.
- `data/write-segments-by-peer-mss.pkt`: the SYN carried `mss 1000`; `write` of 2500 leaves as three `P.`
  segments of 1000, 1000 and 500, each acknowledged by the peer in turn.
- `data/duplicate-is-acked-at-once.pkt`: a repeated segment; an immediate ACK; `read` sees it once.
- `data/out-of-window-is-acked-not-stored.pkt`: a segment above a gap; immediate ACK at the gap; `read`
  returns what came before.
- `close/local-close-then-remote-fin.pkt`: `close`, `F.` out, ACK in, `F.` in, ACK out.
- `close/remote-fin-then-local-close.pkt`: `F.` in, ACK out at `+0` (Linux: `+.04`), `read` returns 0,
  `close`, `F.` out, ACK in; a later segment is reset, since the slot was released at `Closed`.
- `close/fin-retransmitted-on-schedule.pkt`: no ACK of the FIN; `F.` again at +1, +2.
- `reset/rst-in-established.pkt`: `R.` in; `read` fails; the slot is released.
- `reset/rst-to-syn-is-refused.pkt`: `connect`, SYN out, `R.` in; the descriptor reports the refusal.

## Testing

**Conformance suite** (`tests/shared/device/device_conformance.hpp`, both backends):

- `ALentPacketIsReadableAndBackOnceTheDeviceIsDone`: lend, transmit the handle, the lender still holds the
  frame bytes while lent; receive the frame, trim the received packet's front and drop it, `reclaim`; the
  lender's `size()` and bytes are unchanged and `lent()` is false. On the ring this is the loopback hazard,
  closed by the receive copy; on the fabric the eager `reclaim` of an `OnStep` port.
- `AHandleDroppedWithoutTransmitReturnsTheLoanAtOnce`.
- `ADroppedLenderIsFreedOnceWhenTheDeviceReturnsIt`: the pool's count, taken as `ThePoolRunsOutAndRecovers`
  takes it, is the same after the handle has been transmitted, received, dropped and reclaimed.
- `ReclaimAgreesWithItsCapability`: `tx_reclaim` false means `reclaim` returns zero; the fabric reports true.
- The existing tests pass unchanged, which proves a packet that is never lent behaves as before.

**Ethdev** (integration, ring and null; manual, tap):

- `ethdev::Packet`: `lend` leaves the mbuf's count at two and `lent()` true; dropping the handle leaves one
  and `lent()` false; dropping the lender first and the handle second frees once.
- Ring: a received frame whose mbuf was shared is a copy (`get()` differs from the lender's), and the
  lender's count is one as soon as `receive` returns; a receive whose copy finds no mbuf drops and counts.
- Null: the loan is back as soon as `transmit` returns; `tx_reclaim` is false and `reclaim` zero. Ring the
  same for the reclaim.
- Tap (manual, root): the loan is back after `transmit`; the frame reaches the kernel intact.
- The fast-free `static_assert` is its own check; the null test also reads the configured transmit offloads
  back with `rte_eth_dev_conf_get` and asserts the bit is clear, as a guard against a future mask.

**Fabric** (unit):

- Loan policy: `OnStep` finished by `advance` and released by the queue's next `transmit`, `receive` or
  `reclaim`, and released at once by `reclaim` alone; `Held` released only by `return_loans`;
  `loans_outstanding` counts; a lender dropped while in flight gives the block back at the return, and the
  pool's destructor assertion holds at the end of every test.
- The asserts on writing a lent packet or its handle are contract checks like every other assert in the
  repository, which has no death tests; the `Pool` destructor assertion at the end of every fabric test is
  the check that a loan was returned exactly once.
- Impairment: the same seed gives the same drops twice; `loss` drops and counts; `duplicate` delivers twice
  in a row; `reorder` delivers after the next frame and alone at the next `advance`; `delay` and `jitter`
  deliver at `advance` in due order and never early; the script's verdict wins over the model and `Delay`
  takes its own duration; a default `Impairment` changes nothing; counters agree with what the receiver saw.
- Threads: `test_fabric_threads` gains a case with delay on, `advance` from the test thread while the
  senders transmit, every frame arriving exactly once.
- Two stacks under impairment (`test_tcp_two_stacks`): a dropped SYN is retransmitted after one second and
  the connection establishes; a duplicated data segment is counted `dropped_duplicate` and read once; a
  delayed burst arrives in order after `advance`. These are the first tests of the stack against a wire
  that misbehaves, and they pass with today's TCP because control segments already retry.

**Scenario** (unit):

- Parser: every time form, every flag spelling, every option, call lines with `...`, errno and the trailing
  text, comments and blank lines; and the errors: a backtick line, an unknown option, a length that does not
  match its range, an IPv6 or UDP form, each with its line.
- Peer bookkeeping: relative numbers map both ways once the stack's ISN is seen; the ephemeral port is
  adopted from the stack's SYN.
- Runner: an outbound segment early, late and within its window; an unexpected segment; a missing one;
  `write` segmented by the peer's MSS; `read` with each result form; `close` on a listener and a connection;
  the release at `Closed` and the RST that follows; each unbound call failing with its name; both checksum
  modes.
- The first set of scripts, through the test binary.

**Harnesses:** `TcpHarness::advance` and the two-stack `Brick` call `Fabric::advance`; `TcpTestDevice`, the
benchmark's `StampedDevice` and the device fakes in `test_shard.cpp` and `test_net_send.cpp` gain `reclaim`
and `tx_reclaim` and keep modelling `IsDevice`; `test_concepts.cpp` covers the new members.

## Documentation

- `docs/architecture/device.md`: the loan in the concept section (lend, lent, back, the contract), the
  reclaim and `tx_reclaim`, both backends' implementations, the receive copy of a shared mbuf, the fast-free
  invariant, and the loan policy of the fabric.
- `docs/architecture/fixtures.md`: impairment, the fabric's `advance`, the loan policy, and a row and a
  section for `scenario`.
- `docs/architecture/scenario.md`, new: what the runner is, the grammar subset, the call bindings, time and
  matching, how to write and run a script, what the first set covers.
- `docs/architecture/loop.md`: `ShardQueue::reclaim`.
- `README.md`: the `scenario` row in the fixtures table.
- `AGENTS.md`: the layout row for `fixtures/scenario/`, the namespace list, and the traps found: no CI
  driver returns lazily and the ring hands the very mbuf; the fabric needs `advance` for delay and for the
  default loan return; `std::uniform_real_distribution` differs between standard libraries.
- `docs/roadmap.md`: step 2's wording, "Aloe's own scripts in packetdrill's syntax, with Linux's corpus as
  the reference", and the step marked done when it lands.
- `docs/guides/getting-started.md`: running one scenario or one area with a gtest filter.
- Issue #17: a closing comment with what changed against its text (the licence, the lazy-return premise, the
  receive copy in place of reader discipline, TCP untouched until #18).

## Delivery

- Branch `device/packet-loan` from `main`. Conventional commits by module: `feat(device)`, `feat(ethdev)`,
  `feat(fabric)`, `feat(loop)`, `feat(frames)`, `feat(scenario)`, `test(...)`, `docs:`.
- Suggested order, each commit green on `debug`: the concept and the conformance tests (red on both
  backends), ethdev, fabric, loop; the fabric's clock and impairment with their tests; the two-stack
  impairment tests; frames' raw options; the scenario parser, peer, harness and runner with their tests;
  the scripts and the test binary; docs and roadmap.
- PR title: `[DEVICE][ETHDEV][FABRIC][LOOP][FRAMES][SCENARIO] Packet loan, scripted impairment and a
  scenario-script runner`. Description: what changed and why in a few lines, the verification, and the
  premises that failed.
- Verification before the PR is ready: `debug`, `gcc-debug`, `asan` and `tsan` build and pass; the format
  check passes; `-DDO_BENCHMARKS=ON` builds and its tests pass; the tap manual test's loan case run once as
  root.

## Consequences for later steps

- **#18 needs the fabric's `Held` loans** for every "still lent" path, since no CI driver is lazy; the
  tests the draft lists (deferred and counted, sent once returned, copied after the bound, acknowledged while
  lent, released with loans out) are fabric tests with `LoanReturn::Held`.
- **#18's benchmark rerun changes the mbuf flow.** Once the stack lends its orders, the venue peer's
  `receive` copies each order into the peer's pool and frees the stack's mbuf there, so the carrier becomes
  the peer's own mbuf and the peer's rewrite is safe without a change to `VenuePeer`. But the stack's order
  mbufs are then freed on the peer core and the peer's carriers on the stack core: the one-way flow the
  AGENTS.md trap warns about, a mempool ring operation every few hundred mbufs on each side. #18 either
  accepts that in its numbers and says so, or sizes the per-lcore caches, or has the peer return each order
  mbuf untouched; the benchmark page records the choice.
- **`Ipv4` gains a keeping send in #18**, taking `Packet&`, lending after the headers and the transmit
  metadata are in place. The refused-send path hands the packet back as built, as `send` does today.
- **Scripts for later steps** add `sackOK`, `wscale` and `TS` to the SYN lines as those land, and the
  `+.04` delayed ACKs when step 4 adds them; the grammar already carries every option they need.

## Decisions made in the brainstorm

- Aloe's own scripts in packetdrill's syntax; Linux's corpus is a reading reference and nothing from it is
  copied in (author: "Own scripts").
- TCP does not lend in this step; the loan is a device and loop capability until #18 (author: "loan stays
  in device until #18").

Claude's calls, stated in the brainstorm and not objected to:

- The loan is spelled on the packet, `lend()` and `lent()`, a second handle for the device and a poll on
  the lender; ethdev backs it with the mbuf's count, the fabric with its in-flight list.
- Ethdev's receive copies a shared mbuf, so a received packet is exclusively owned on every backend and the
  ring's loopback needs no reader discipline.
- The fabric gains a clock step; impairment is per port's egress, a seeded model plus a scripted verdict.
- The runner is a fixture module with its own namespace; scripts live under `tests/scenarios/`.

Settled while writing, Claude's calls, listed for the author to overrule:

- One borrowed handle at a time; a second `lend()` while lent is asserted, not counted.
- `reclaim(queue)` on the device concept and `Capabilities::tx_reclaim`, probed on ethdev after the start.
- The fabric's `LoanReturn` policy with `OnStep` as the default, `Held` for the tests that need a driver
  that will not let go, and `Fabric::advance(now)` as the step.
- A dropped lender orphans the fabric block rather than counting handles, since there is one handle.
- A failed copy of a shared mbuf on receive counts in `QueueCounters::dropped`.
- The fast-free invariant is a `static_assert` on the mask plus a read-back in the null test, since no CI
  driver advertises the offload.
- Randomness from the engine's raw bits, not `std::uniform_real_distribution`.
- One test binary discovering scripts at run time and registering one gtest per script and checksum mode;
  no per-script CTest entries, to keep the repository's one-entry-per-binary pattern.
- A peer ISN that is a non-zero constant; the stack's ISN read from the wire.
- Every script runs in both fabric checksum modes and verifies every outbound checksum.
- Unbound calls and unsupported syntax fail the script; nothing is skipped silently.
- `close` binds to `close()` and always sends a FIN; the reset-on-unread-close question goes to #21.
- The tolerance defaults to one wheel resolution; `*` is bounded by the runner's configuration.

Settled after the builder check, Claude's calls, listed for the author to overrule:

- The runner matches as packetdrill does: sent segments queue in order with their stamps, and outbound lines
  take them in order across the inbound lines and calls between; an unexpected segment fails at the next
  outbound line, by its stamp, or at the end of the script.
- Inbound lines and calls act at their nominal time; the tolerance applies to outbound lines only; `accept`,
  `read` and `recv` with a window are retried per step until the window closes.
- A device may complete the checksums it was asked to fill inside the lent buffer, as tap and the fabric do;
  the fabric keeps filling in place, so the tests exercise the weaker case; everything else stays as lent.
- The fabric finishes `OnStep` loans at `advance` and releases them on the queue's thread at its next
  `transmit`, `receive` or `reclaim`, with `reclaim` releasing eagerly; the pool stays unsynchronised.
- The scenario harness learns ARP with hour-long lifetimes.

## Left to the builder

- The exact file split inside `fixtures/scenario/` beyond one class per file, and the parser's technique.
- The `Failure` and `ParseError` layouts, as long as they carry the line, the expectation, the observation
  and the stamp in the script's notation.
- How `Egress` and `Verdict` are refined if a test needs more than the frame and its index.
- The fabric's internal queues for delayed and reordered frames and which lock covers each.
- Whether ethdev's receive copy uses `rte_pktmbuf_copy` or a hand-rolled copy that preserves the headroom
  and the receive flags; the result must keep `packet_headroom` of headroom and the RSS and checksum flags.
- Where in the constructor the `tx_reclaim` probe sits, after `rte_eth_dev_start` and before the MAC read.
- The pattern `write` fills its segments with, and how `read` checks what it consumed.
- The bound for `*` and the exact shape of the runner's configuration.
- Test names and the split of the scenario tests across files.
- The wording of the closing comment on #17.
