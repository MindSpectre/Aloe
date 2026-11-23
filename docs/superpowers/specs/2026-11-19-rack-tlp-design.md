# Loss recovery with SACK: a scoreboard on the retransmission queue, RACK-TLP, one recovery with PRR, undo, and window growth only while the window is used

Status: design, from the brainstorm of 2026-11-18 and 19, checked against the code and the Linux source on
2026-11-19 and read by a builder the same day with the findings folded in, for issue #20 (phase 2, step 5 of
`docs/roadmap.md`). It rests on the reliable-send spec of
2026-10-28 for the queue, the timer and the policy concept it changes, and on the receive-side spec of
2026-11-12 for the SACK blocks it now reads from the other end. The issue's premises were checked against the
code on `main` at 1aeddb9, after the layout round merged as #42; where a premise failed, this document says
so. Private, like every file under `docs/superpowers/`; the public summary is the roadmap's step 5 and the TCP
page once the work lands.

## Goal

Today the sender learns of a loss in two ways only: three duplicate ACKs, or the retransmission timer. A venue
client sends small orders, a few at a time and often one alone, so it rarely has three segments behind a loss
to produce three duplicates. A lost order therefore waits for the timer, 200 ms and more, and so does a resend
that is lost again. The peer's SACK blocks, which say exactly what arrived, are skipped.

This step makes the sender recover from what the peer reports and from time, as Linux does by default. It reads
the peer's SACK blocks into a scoreboard kept on the retransmission queue's own entries. It marks a segment
lost when a segment sent after it has been delivered and a short reordering window has passed (RACK, RFC 8985).
It probes a silent tail by sending the last segment again after about two round trips, ahead of the timer (TLP,
the same RFC). The window during recovery follows Proportional Rate Reduction (RFC 9937, which replaced
RFC 6937 in December 2025), a resend that turns out to have been needless gives the window back, and the
window stops growing while the application is not using it.

The first deployment is a client holding long-lived connections to trading venues, many across the internet.
The case this step exists for is a lost order: alone in flight, at the end of a burst, or in the middle of one.

## Goals, non-goals, anti-goals

Goals:

- A segment lost in the middle of a flight is resent once one later segment is reported received and the
  reordering window has passed, with no need for three duplicates and no wait for the timer.
- A lost last segment is resent by the probe about two round trips after it left. A segment that is alone in
  flight waits a configured allowance longer, 40 ms by default, for the peer's delayed ACK.
- Two lone orders lost one after the other are both probed. RFC 8985's procedure and Linux give the second no
  probe, and it waits for the timer; this is the one place where Aloe departs from the RFC's procedure, by the
  author's decision, and the departure is documented in public.
- A resend that is lost again is detected from later acknowledgements, not by the timer.
- Reordering within the reordering window causes no resend. Deeper reordering on a connection that has seen
  none causes one needless resend, which costs the congestion window nothing once it is recognised, and the
  window adapts from there.
- One recovery for every connection. A peer that offers SACK is read through its blocks; a peer that does not
  is read through duplicate ACKs, as today. What happens after a loss is found is the same for both.
- Everything works on the loan as the timer's resend does: any marked entry is resent from the packet it left
  in, a packet the device still holds is never written, and the busy rule and the copy fallback apply to any
  entry, not the head alone.
- The RFCs are the specification; where they leave a choice, Aloe does what Linux does, and the plan reads the
  Linux source named under "Constraints" before writing each part.
- The tick path keeps its cost: an in-order ACK on a connection with nothing reported out of order does a
  constant amount of new work, an order still costs one arm of the connection's timer, and the tick-to-trade
  benchmark on the finished branch shows no change against `main` beyond run-to-run noise.

Non-goals, each with where it goes:

- F-RTO (RFC 5682): not built. A needless timeout is recognised from the echoed timestamp, under "Undo", on a
  connection that uses timestamps, which is the default; FreeBSD and VPP do the same, and neither they nor
  Onload ship F-RTO. It is filed only if the phase's comparison against Linux under netem asks for it.
- A separate list of holes beside the queue, and splitting an entry that a SACK block covers in part: not
  built. The queue holds at most `send_segments` + 1 entries, and a lent packet cannot be split in place.
- A tail probe that carries new data: there is none to carry. The brick has no send buffer, so the probe is
  always a segment that already left.
- RACK and the tail probe on a connection without SACK: RFC 8985 requires SACK for both, and Linux uses
  neither there. Such a connection keeps detection by duplicate ACKs and the timer.
- RACK, the tail probe and the recovery states during the handshake: a SYN and a SYN-ACK keep the timer alone.
- Switches for RACK or the tail probe apart from `sack`, and settings for the reordering window: none. The
  reordering window's constants are RFC 8985's.
- Shrinking a window the application left unused (RFC 2861, Linux's `tcp_cwnd_application_limited`): not
  built; see "Window growth".
- CUBIC, BBR, pacing: after phase 2, as before. The policy concept changes shape in this step and stays one
  that BBR fits.
- ECN: not in this phase.
- The event raised when a connection first leaves the fast path, TIME_WAIT, keepalive, path MTU, RFC 5961:
  step 6.
- New events, new senders, any change to the stream contract or the runtime: none.

Anti-goals, which would make this a failure even if it built and passed:

- A copy of payload on any path that runs without a loss, or a write into a packet the device may still read.
- Dropping the packet of an entry the peer reported with a SACK block before a cumulative ACK covers it. A
  receiver may discard what it reported, and Aloe's own does when it needs room.
- A resend of an entry the scoreboard holds as received.
- Two recovery paths that can disagree: a state, a window rule or a resend path that exists once for peers
  with SACK and once for peers without.
- A tail probe that counts as a timeout: it backs nothing off, starts no loss episode and takes no retry.
- Logging, allocation beyond the fixed pools, or a clock read inside the brick.
- A scenario script copied from Linux's corpus.
- A regression of the benchmark's 64-byte tick that is merged without a number and a reason beside it.

## What the issue assumed, checked

Checked against `main` at 1aeddb9, and against Linux at 4405c18c95d2:

- **"A lost last segment waits for a full RTO; the tail loss probe resends it after about two round trips"**
  holds for a flight of two or more segments only. For a segment alone in flight RFC 8985 (7.2) lets the probe
  wait longer for the peer's delayed ACK, by an amount it leaves to the implementation, and caps the probe at
  the RTO. Linux adds its minimum RTO, 200 ms (`tcp_schedule_loss_probe`, `packets_out == 1`), so its probe for
  a lone segment fires at the moment the RTO would have: it saves the backoff and the window, not the time. An
  order is usually a lone segment. Following Linux here would not deliver what the issue is for, so the
  allowance is configuration, 40 ms by default; see "The tail loss probe".
- **"Linux's packetdrill scenarios for RACK and TLP are the source of the tests"** finds little to read.
  Neither the kernel's selftests nor google/packetdrill has a script for the tail probe, for the undo of a
  recovery, for a duplicate SACK arriving at a sender, for the reordering window's adaptation or for reneging.
  What exists is four PRR scripts, a handful on fast retransmit and limited transmit, three on SACK marking,
  and one on RACK's reordering timer. Aloe's scripts for the rest are derived from RFC 8985's and RFC 9937's
  own worked examples and from the Linux source; the inventory is under "Testing".
- **"Per-segment transmit times, including retransmitted segments"** exist: `SentSegment::sent` is stamped by
  every transmission, a copy's included, and `transmits` counts them. The stamp is the tick's, so several
  segments share one; RACK breaks the tie by sequence, as the RFC and Linux's `tcp_skb_sent_after` do.
- **"The tail loss probe and its timer, beside the RTO of step 3"** is one timer, not two. The control block
  has one wheel timer for its sends with a `TimerKind` (the RTO, the ARP poll, the zero-window probe), and
  RFC 8985 (8) expects exactly that: the reordering timer, the probe timer, the RTO and the zero-window probe
  are mutually exclusive, "one timer with an additional state variable". Linux has one timer and one pending
  kind for the same four. Two kinds join Aloe's three.
- **"Marking and retransmitting work on the loan as the RTO path does"** needs the resend path widened. Today
  everything that resends is about the head: `Transmitter::resend_head`, the deferred list's retry in
  `flush_deferred`, `AckProcessor::fast_retransmit`. The per-entry state the busy rule needs, `deferrals`,
  `deferred_at` and `copy`, is already on the entry.
- **"A SACK scoreboard on the send side over the retransmission queue"** fits what the queue is: whole
  segments as they left, never coalesced, oldest first, freed from the head by a cumulative ACK. A block marks
  the entries it covers whole. An entry marked this way stays in the chain with its packet until the cumulative
  ACK passes it, so nothing is ever unlinked from the middle.
- **`wire::TcpOptions::parse` skips kind 5** by its length byte. `wire::TcpSackBlock` and the writer exist
  from step 4, and the frames fixture has a reader of its own (`sack_blocks_of`), written because the brick's
  parser did not decode them.
- **Loss recovery is `LossRecovery<CongestionControl>`**: the duplicate count, RFC 3042's allowance, RFC
  6582's `recover`, two flags for fast recovery and the episode after a timeout, and the policy's state. The
  policy concept has four hooks that exist only for RFC 6582's inflation and deflation: `on_loss`,
  `on_dupack`, `on_partial_ack` and `on_recovery_end` with the flight.
- **"Two Aloe stacks stay short until step 5"**, recorded by step 4: the receiver answers the first three
  segments above a gap at once and the rest at the flush, so Aloe's own sender, counting duplicates, has
  little to send new data on. A sender that reads the blocks needs no duplicates.
- **Congestion-window growth on a flow the application limits** was moved here by step 4. NewReno grows its
  window on every ACK today. The entry already records whether its send left window unused (`app_limited`),
  and `AckInfo` carries it; nothing reads it.
- **The scenario runner already speaks the blocks** in both directions, `sack a:b c:d` with a base per
  direction; `sack/peer-sack-blocks-are-skipped.pkt` injects them today. Of the 72 scripts, 22 have a peer
  that offers `sackOK`; the scripts about the timer and about recovery mostly do not, so they keep detection by
  duplicates and change only where a window shows.
- **Scripts run at a round trip of zero.** With a smoothed round trip of zero, two round trips are zero and a
  quarter of the shortest is zero: a probe for a flight of several is due at the next tick of the wheel, and
  RACK marks at once. A script or a fabric test that shows the reordering window or the probe's wait gives its
  path a round trip.
- **"Against Linux over tap under netem loss, tail losses recover without an RTO"**: netem shapes the kernel's
  egress, which is the stack's receive side. The stack's own sends are dropped by the suite's `LossyDevice`,
  which step 3's cases already use.
- **The benchmark's venue offers SACK-permitted**, so every order of the benchmark arms the probe where it
  armed the RTO, and the benchmark measures the new arming. Its sanity check asserts zero drops and gains the
  new counters.

## What other stacks do, checked

Linux settles what is correct. What the stacks built for speed do was read from source on 2026-11-18, each at
its default branch: Onload (6caaed92), TCPDirect (1d934a8a), FreeBSD's default stack and its RACK stack
(5151bc29), VPP's host stack (4fbcc12c), Seastar, mTCP, TLDK and lwIP.

- **A lone segment waits for the peer's delayed ACK in every stack that has a tail probe.** Onload adds its
  minimum RTO, 200 ms, with the comment "We follow Linux instead of the spec" (`ci_tcp_taildrop_timeout`). VPP
  adds 200 ms, fixed at compile time (`tcp_tlp_pto_ticks`). FreeBSD's RACK stack waits
  `max(2 * srtt, 1.5 * srtt + 40 ms)`, the 40 ms a sysctl, with a floor of 10 ms (`rack_calc_thresh_tlp`). No
  stack uses zero, and only FreeBSD makes the allowance a setting of its own.
- **Onload's probe gains nothing on a cross-connect**: when the smoothed round trip is under 2.5 ms at its
  defaults (eight round trips under a tenth of the minimum RTO), its probe waits the initial RTO capped at the
  RTO.
- **A tail probe exists in Onload (on by default), FreeBSD's RACK stack and VPP (with RACK, off by default)**,
  and in none of TCPDirect, Seastar, TLDK, lwIP, mTCP and FreeBSD's default stack, which wait for the timer.
  TCPDirect negotiates no SACK at all.
- **Time-based detection is FreeBSD's RACK stack and VPP's.** FreeBSD waits a quarter round trip beyond the
  round trip, growing per duplicate SACK up to two round trips; VPP follows the RFC, a quarter of the shortest
  round trip times a multiplier that grows per round with a duplicate SACK, at most one smoothed round trip.
  Onload counts three duplicates, a new SACK counting as one, with RFC 5827's early retransmit.
- **The scoreboard is a flag on the queued packet in Onload**, and a block that covers part of a packet is
  ignored there, as it is in Linux for a buffer of one segment. FreeBSD and VPP keep ranges and split them;
  VPP has no segment queue and rebuilds resends from its send buffer.
- **PRR regulates the window in recovery in Linux, FreeBSD (both stacks) and VPP.** Onload sets the window to
  the threshold plus three segments and estimates from the highest block.
- **A needless resend gives the window back** in Linux, FreeBSD's RACK stack and VPP, on a duplicate SACK. A
  needless timeout is recognised by the echoed timestamp in Linux, FreeBSD and VPP. Onload undoes nothing.
- **Nobody but Linux ships F-RTO.**

## Approaches

Two ways to build this were weighed; they differ in what happens to step 3's recovery, which is still needed
for a peer without SACK.

1. **One recovery, shaped as Linux's.** How a loss is found depends on the peer: RACK on a connection that
   uses SACK, three duplicate ACKs on one that does not. What follows is one thing: one set of states, one
   resend path over the kept packets, PRR for the window, one undo. The policy concept shrinks, since the hooks
   that inflate the window per duplicate go. Linux's own behaviour becomes a reference for the windows in
   recovery, which it is not today because Linux runs PRR and Aloe did not. The price: the existing recovery
   scripts are derived again once, and a peer without SACK sees PRR where it saw RFC 6582's arithmetic.
2. **Two recoveries side by side.** Step 3's `LossRecovery` stays untouched for peers without SACK and a second
   one is added for peers with it. No script and no behaviour moves. The price: two recovery paths to keep
   right through step 6 and BBR, and a policy that implements both sets of hooks.

The author picked the first ("Yeah, lets go with one recovery"). It is what Linux does, and it leaves one path.

Dropped before that choice:

- **Onload's probe without RACK**, the cheapest thing that resends a lost tail: reordering would still be read
  as loss, a lost resend would still wait for the timer, and RFC 8985 does not allow the probe without RACK.
- **A list of holes beside the queue**, as FreeBSD's default stack and VPP keep: the issue asks for the
  scoreboard on the queue, and the queue is at most 33 entries.

## Constraints

- The rules of `AGENTS.md`: `tcp` links neither `execution` nor `log`; one class in one place, and a class that
  grows sections is split into classes that own their state; a header whose names are all in `detail` sits in
  the `detail/` folder of the folder it belongs to; `std::expected` on hot paths and exceptions on setup; every
  hot-path member is `noexcept` and allocates nothing but packets from IP's pool and nodes from the stack's
  fixed pools; a change is done when `debug`, `gcc-debug`, `asan` and `tsan` pass with the format and include
  checks.
- Tests need no root, no hugepages, no card. The tap runs are manual tests, run by hand as root.
- The scripts are Aloe's own, in packetdrill's syntax, with Linux's corpus as a reading reference.
- The parts keep their direction: the Receiver calls the Lifecycle, the AckProcessor and the Transmitter; the
  AckProcessor calls the Transmitter; the Transmitter calls none of them; only the Lifecycle writes a
  connection's state.
- The congestion window is counted in bytes, as RFC 9937 and RFC 5681 count it. Linux counts packets, so where
  this document says Aloe does what Linux does, it means Linux's rule stated in bytes. Where an RFC is definite
  and Linux differs, the RFC stands and the difference is named.
- `docs/architecture/tcp.md`, `scenario.md`, `fixtures.md`, `docs/roadmap.md` and `AGENTS.md` change in the same
  change as the code; the benchmark page gains the rerun.
- What this design says Linux does was read from the source on 2026-11-19, torvalds/linux at 4405c18c95d2, and
  each such sentence names its function. The plan reads the same functions again before writing each part and
  records what it relied on: `net/ipv4/tcp_recovery.c` whole (`tcp_rack_reo_wnd`, `tcp_rack_skb_timeout`,
  `tcp_rack_detect_loss`, `tcp_rack_mark_lost`, `tcp_rack_reo_timeout`, `tcp_newreno_mark_lost`);
  `tcp_input.c` (`tcp_parse_options`, `tcp_sacktag_write_queue`, `tcp_is_sackblock_valid`,
  `tcp_match_skb_to_sack`, `tcp_check_dsack`, `tcp_dsack_seen`, `tcp_sacktag_one`, `tcp_rack_advance`,
  `tcp_rack_update_reo_wnd`, `tcp_check_sack_reordering`, `tcp_check_sack_reneging`, `tcp_clean_rtx_queue`,
  `tcp_identify_packet_loss`, `tcp_mark_skb_lost`, `tcp_fastretrans_alert`, `tcp_time_to_recover`,
  `tcp_enter_recovery`, `tcp_init_undo`, `tcp_add_reno_sack`, `tcp_limit_reno_sacked`,
  `tcp_init_cwnd_reduction`, `tcp_cwnd_reduction`, `tcp_end_cwnd_reduction`, `tcp_may_undo`,
  `tcp_packet_delayed`, `tcp_try_undo_recovery`, `tcp_try_undo_partial`, `tcp_try_undo_dsack`,
  `tcp_try_undo_loss`, `tcp_undo_cwnd_reduction`, `tcp_process_tlp_ack`, `tcp_set_xmit_timer`,
  `tcp_rearm_rto`, `tcp_enter_loss`, `tcp_timeout_mark_lost`, `tcp_process_loss`, `tcp_cong_control`,
  `tcp_may_raise_cwnd`); `tcp_output.c` (`tcp_schedule_loss_probe`, `tcp_send_loss_probe`,
  `tcp_event_new_data_sent`, `tcp_xmit_retransmit_queue`, `tcp_cwnd_validate`, `tcp_cwnd_test`);
  `tcp_timer.c` (`tcp_retransmit_timer`, `tcp_write_timer_handler`); `tcp_cong.c` (`tcp_reno_cong_avoid`,
  `tcp_reno_ssthresh`, `tcp_reno_undo_cwnd`); and `include/net/tcp.h` (`tcp_is_cwnd_limited`,
  `tcp_packets_in_flight`, `tcp_skb_sent_after`).

## The life of an acknowledgement

An acceptable ACK on a synchronized connection is processed in this order. Each step is a section below.

1. **The blocks.** On a connection that uses SACK, the segment's SACK option is read: a duplicate SACK is
   recognised from the first block, and each valid block marks the entries it covers whole. Blocks are judged
   against this ACK's acknowledgement number, which is where SND.UNA is about to be.
2. **The cumulative part**, as today: the entries the acknowledgement covers are freed from the head, SND.UNA
   moves, and the round-trip sample is taken by Karn's rule or from the echo.
3. **RACK's state**: the most recently sent segment among those this ACK newly reported, its round trip, and
   whether the ACK showed reordering.
4. **The probe's ACK**: whether a tail probe that is out was needless, repaired a loss, or says nothing yet.
5. **The undo's checks**, which may end an episode and may stop this ACK from marking and resending.
6. **Detection.** With SACK, RACK marks the entries whose time has passed and says how long the rest may
   still wait. Without SACK, the duplicate count marks the head at the third duplicate, and a partial ACK in
   recovery marks the new head.
7. **The recovery state**: an episode begins when the first entry is marked, proceeds, or ends; the window in
   recovery is computed; outside recovery the policy hears of an ACK of new data.
8. **Resends**: the marked entries go, lowest sequence first, as far as the window allows.
9. **The peer's window**, the zero-window probes and the FIN that waited for the window, as today.
10. **The timer**: the reordering timer when RACK asked for one, else the tail probe when one may be scheduled,
    else the RTO while anything is in flight, else nothing.

This is the order of Linux's `tcp_ack` and `tcp_fastretrans_alert`: in recovery and in the loss state the undo
is tried before anything more is marked.

A segment that carries data and a SACK option is processed the same way: the blocks do not depend on the
segment being a pure ACK. The ACK of a segment the Receiver held above a gap is processed since step 4, and
its blocks are read with it.

An ACK below SND.UNA moves nothing today and is otherwise ignored by the send side. Its blocks are now read,
steps 1 and 3 to 8 and the timer, as Linux's `tcp_ack` reads them on its old-ACK path: on a path that reorders,
a duplicate SACK can ride a late ACK, and it is reported once only. An ACK older than SND.UNA by more than the
largest window the peer ever offered is too old and its blocks are ignored (`tcp_sacktag_write_queue`).

## The scoreboard

**Where it lives.** On the retransmission queue's entries. Each `SentSegment` gains two marks: the peer
reported it received, and it is lost and owes a resend. The queue keeps the totals the rest reads: the bytes
reported received, the number of segments reported received, the bytes lost and not yet resent, and the highest
sequence the peer has acknowledged either way.

**Reading the option.** `wire::TcpOptions::parse` records where a SACK option starts and how many blocks it
holds, as it records where the timestamps start; it copies nothing. A SACK option whose length is not two plus
eight per block, for one to four blocks, is ignored and the segment is processed without it, as Linux's
`tcp_parse_options` does; this differs from the other options, whose bad length drops the segment. The blocks
are read only on a connection that negotiated SACK.

**Duplicate SACK is recognised first.** RFC 2883 as Linux reads it (`tcp_check_dsack`, `tcp_dsack_seen`): only
the first block can be one, and it is one when it starts below the cumulative acknowledgement, or when a second
block contains it. Recognition looks at the option alone, before the queue and before the validity test below,
so a duplicate SACK is read even when nothing is outstanding and no episode is open, which is the usual case
for the answer to a needless probe. A report is not believed when it covers more than the largest window the
peer ever offered, or when the peer has by then reported more duplicates than the connection has resent
segments. What a believed report does is described in its sections: the probe's verdict reads it, the undo
counts it, RACK's reordering window grows by it, and it is counted. A duplicate report marks no entry received.

**Which blocks touch the entries.** Linux's `tcp_is_sackblock_valid`, which gates the walk over the queue and
nothing above:

- a block whose end is past SND.NXT, or whose start is not before its end, is ignored;
- an ordinary block must start above SND.UNA;
- a duplicate SACK at or below SND.UNA is applied to the queue only while an episode's undo is open and the
  block is not older than that episode's start.

A block that fails is ignored and the others are applied.

**Marking.** A block marks an entry when it covers the entry's whole range. A block that covers part of an
entry marks nothing. That is Linux's rule for a buffer that holds one segment (`tcp_match_skb_to_sack` splits
only a buffer of several), and Onload's. Part covers are rare: the entries are the segments as they left, a
resend after a partial ACK is trimmed to what the peer lacks, and a copy that returns first shrinks its entry
to the copy's range. Their cost is bounded: the entry is not known to be received, so RACK may resend it, the
peer reports the duplicate, and the undo gives the window back. An entry marked received is no longer lost, if
it was marked so.

**What stays.** An entry reported received keeps its packet and its place until the cumulative ACK frees it.
RFC 2018 lets a receiver discard data it reported, and Aloe's receiver does so when it needs room for a lower
sequence. A timeout leaves the received marks in place too, except on reneging.

**Reneging.** When an ACK moves SND.UNA and the head it leaves is marked received, the peer discarded what it
reported. Linux clears nothing at that point: it arms the RTO short, at `max(srtt / 2, 10 ms)`, and does
nothing else for that ACK (`tcp_check_sack_reneging`). If the head is still marked received when the RTO
fires, every received mark is cleared and every entry is lost (`tcp_timeout_mark_lost`). Aloe does the same,
with the deadline rounded up by the wheel, and counts it.

**What is in flight.** RFC 9937's estimate for a sender that detects by RACK: the sequence space sent and not
acknowledged, less the bytes reported received, less the bytes marked lost and not yet resent. A resend clears
the entry's lost mark, so a resent entry counts as in flight again without a third total; this is Linux's
`tcp_packets_in_flight` in bytes. On a connection without SACK the bytes reported received are replaced by one
segment for each duplicate ACK since SND.UNA last moved, capped so that at least one segment stays counted as
the hole, as Linux's `tcp_add_reno_sack` and `tcp_limit_reno_sacked` emulate them.

`writable()` and the commit read two limits where they read one: the peer's window against the sequence space
outstanding, as today, and the congestion window against what is in flight. This is where limited transmit
(RFC 3042) now comes from: a report of data received, or a duplicate ACK, lowers what is in flight and so lets
new data out. Linux has no separate allowance either (`tcp_cwnd_test`), and no cap of two segments; the
recovery that begins at the third report is what bounds it. `LossRecovery`'s `limited_transmit_` goes.

**Delivered bytes.** The connection's delivered count, which the policy's rate fields read, counts each payload
byte once: when a block first reports it, or when the cumulative ACK first covers it.

## RACK

RFC 8985, sections 6 and 9, on a synchronized connection that uses SACK; Linux runs it unconditionally there.
It applies to every entry that carries data or a FIN.

**State per connection.** The transmit stamp and end sequence of the most recently sent segment the peer has
acknowledged either way; the round trip of the last segment acknowledged; a minimum round trip over a window of
about five minutes, as Linux keeps it; whether reordering has been seen; the reordering window's multiplier,
the count of recoveries it persists for, and what marks the round in which a duplicate SACK last grew it.

**On an ACK** (`tcp_rack_advance`, from `tcp_sacktag_one` and `tcp_clean_rtx_queue`). Each entry the ACK newly
reports, by a block or by the cumulative acknowledgement, updates RACK: its round trip is taken, and its stamp
and end sequence replace RACK's when it was sent later, comparing stamps and then end sequences. An entry that
was resent is skipped when the ACK cannot be for the resend: the entry's round trip would be below the minimum
round trip, or, for a cumulative ACK, the echoed timestamp is older than the resend's.

**Reordering is seen** (`tcp_check_sack_reordering` and its callers) when:

- a block newly reports an entry that was never resent and lies below the highest sequence already reported;
- the cumulative ACK fills a hole below that highest sequence with an entry that was never resent;
- an ACK in recovery is recognised by its echo as answering an original, under "Undo";
- a duplicate SACK reports a resent entry that a block had also reported received.

**The reordering window** (`tcp_rack_reo_wnd`, `tcp_rack_update_reo_wnd`). Zero while reordering has never been
seen and the connection is either in recovery or has three or more segments reported received: with no
evidence of reordering, RACK is as quick as three duplicates were. Otherwise a quarter of the minimum round
trip times the multiplier, and never more than the smoothed round trip. The multiplier starts at one. It grows
by one in a round trip that brought a duplicate SACK, once reordering has been seen, and not for the duplicate
SACK that answers a tail probe. It persists for 16 recoveries: the count is set when the multiplier grows,
drops at the end of each recovery that was not undone, rises by one on an undo by duplicate SACK, and at zero
the multiplier returns to one.

**Detection** (`tcp_rack_detect_loss`) runs when RACK's state moved on this ACK, and when the reordering timer
fires. An entry not reported received, and not already waiting for its resend, is lost when a segment sent
after it has been acknowledged and `sent + RACK's round trip + the reordering window` has passed. Entries whose
time has not yet passed give the wait that remains; the longest of them is what the reordering timer is armed
for, so one expiry settles every entry that was waiting. An entry already resent is judged by its resend's
stamp, so a resend that is lost again is marked again: this is the lost retransmission the timer used to find.
It is counted (`tcp_mark_skb_lost`).

**Its cost when nothing is wrong.** The queue is in sequence order, and for entries that were never resent
that is transmit order. The walk therefore ends at the first entry that was not sent before RACK's segment. An
in-order ACK on a connection with nothing reported out of order and nothing resent updates RACK's fields and
does one comparison at the head.

**Two cases the done-when of the issue names.**

- Reordering shallower than three segments and shorter than a quarter of the minimum round trip causes no
  resend: the late segment's ACK arrives before the reordering timer and cancels it.
- The first reordering deeper than that on a connection that has seen none does cause one resend, since the
  window is zero once three segments are reported. With timestamps the late original's ACK is recognised by
  its echo: the reduction is undone and reordering is recorded, so the window is no longer zero the next
  time. The peer's duplicate SACK undoes the reduction as well. This is Linux's behaviour and RFC 8985's, and
  the tests state it as it is.

## The tail loss probe

RFC 8985, section 7, on a synchronized connection that uses SACK.

**When a probe is scheduled.** After data is sent, and after an ACK that moves SND.UNA and leaves something
unacknowledged, while the connection is in no recovery and nothing is reported received out of order. Those
cases are RACK's and the recovery's. These are the two call sites of Linux's `tcp_schedule_loss_probe`.

**How long it waits.**

- Two smoothed round trips from that moment.
- Plus `tail_probe_ack_delay`, 40 ms by default, when exactly one segment is in flight.
- `rto_initial`, one second by default, when there is no round-trip sample yet.
- Never later than the moment the RTO would fire.

The wheel rounds a deadline up to its resolution, one millisecond in a shard by default, and that is the only
floor. Linux adds two milliseconds for a flight of several and FreeBSD floors at ten.

**The allowance.** RFC 8985 says a sender "MAY inflate the PTO by TLP.max_ack_delay" for a flight of one and
leaves the value open. The allowance is what a lone order waits beyond two round trips before it is sent again:

| Value  | Who uses it                 | A lone lost order goes again after | A needless probe goes when the peer's ACK takes        |
|--------|-----------------------------|------------------------------------|--------------------------------------------------------|
| 200 ms | Linux, Onload, VPP          | the RTO's time, as today           | never, short of the RTO                                |
| 40 ms  | FreeBSD's RACK stack; Aloe  | two round trips + 40 ms            | 40 ms longer than twice the usual                      |
| 0      | nobody                      | two round trips                    | twice the usual                                        |

40 ms is Linux's shortest delayed ACK (`TCP_DELACK_MIN`). The smoothed round trip is measured from the peer's
own ACKs, so a peer that always delays is already allowed for. A needless probe is one duplicate segment, which
the peer's TCP discards by sequence number and reports; its application never sees the order twice. A
deployment whose venue answers every order at once sets the allowance to zero.

**A probe has two things open, and Aloe tracks them apart.** RFC 8985 keeps one variable, `TLP.end_seq`, for
both; Aloe keeps two facts:

- **The probe is in flight** from the moment it leaves until a cumulative ACK reaches its end, the SND.NXT it
  was sent at.
- **The probe's verdict is open** from the moment it leaves until it is known whether the probe was needless
  or repaired a loss. An ACK that lands exactly on the probe's end does not say which: it may answer the
  original or the probe. So the verdict usually outlives the flight.

A recovery or a timeout that begins forgets both.

**When the timer fires.** A probe goes only if no earlier probe is still in flight and a round-trip sample was
taken since the last one. The probe is the highest-sequence entry that has been transmitted, sent again whole
through the resend path, so the busy rule and the copy fallback apply to it. It is counted as a tail probe and
as nothing else: it takes no retry, backs off nothing, starts no episode, and is not one of an episode's
resends. The connection remembers SND.NXT as the probe's end. After a probe the RTO is armed from now; when the
probe was skipped the RTO keeps the moment it had (`tcp_send_loss_probe`, `tcp_rearm_rto`). Either way the timer
is still the last resort.

**A second probe while the first's verdict is open** is where Aloe departs from RFC 8985, by the author's
decision. The RFC's `TLP_send_probe` sends nothing while `TLP.end_seq` is set, and Linux's `tcp_send_loss_probe`
does the same (`if (tp->tlp_high_seq) goto rearm_timer`). After a probe rescues a lost lone order, its ACK
lands exactly on the probe's end, so the verdict stays open until a later order is acknowledged; if that later
order is lost too, the RFC and Linux give it no probe and it waits for the RTO. In Aloe, when the timer fires
with the earlier probe acknowledged and its verdict still open:

- the open verdict is closed as a loss repaired, with its congestion response, as the RFC would close it on
  the next ACK past the probe's end;
- the new probe goes, and its end and its verdict replace the old.

What the RFC's rule protects still holds: 7.3 states it as "no other previous loss probe still in flight", so
that "the sender has at most one additional packet in flight beyond the congestion window limit", and a probe
whose end is acknowledged is not in flight. What changes is the moment of one congestion response, which comes
at the second probe and not at the ACK after it, and one more probe on the wire. The rule errs toward the
smaller window: a first probe that was in fact needless, whose duplicate SACK was lost or is late, is cut for
as a loss, which the RFC's procedure would do too at the next ACK past its end.

The second probe still needs a round-trip sample since the first. On a connection with timestamps, the default,
the rescued order's ACK gives one through its echo. Without timestamps Karn's rule takes none from a segment
sent twice, the second probe is skipped, and the second order waits for the RTO as under the RFC.

**What the probe's ACK means** (RFC 8985 7.4; `tcp_process_tlp_ack`), once an ACK reaches the probe's end:

- It carries a duplicate SACK of at most one segment that ends at the probe's end: the original arrived too,
  nothing was lost, the verdict is closed.
- It acknowledges past the probe's end with no such report: the probe repaired a loss. The policy is told of
  one loss and its window becomes its target at once: the cut a fast recovery would make, with nothing resent,
  no state entered and no undo kept. The verdict is closed.
- It is a plain duplicate ACK with no block: both copies arrived at a peer that sends no duplicate SACK; the
  verdict is closed as needless.
- Otherwise the verdict stays open, until one of the above, a second probe as described, or a recovery or a
  timeout.

When the probe's ACK reports the tail received and earlier segments are still missing, RACK marks them, since
the probe was sent after them and at least a probe's wait has passed. This is how the probe turns a silent
flight into an ordinary recovery.

## The timers

One wheel timer per connection for its sends, with a kind, as today. The delayed ACK's timer is the receive
side's and is not touched.

| Kind                  | Armed when                                                                                                        | Deadline                                                                          | When it fires                                                        |
|-----------------------|-------------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------|----------------------------------------------------------------------|
| Tail probe (new)      | Data was sent, or an ACK moved SND.UNA and left data out; SACK in use, no recovery, nothing reported received      | "How long it waits" above                                                         | Send the last transmitted segment again, arm the RTO                 |
| Reordering (new)      | RACK found unreported entries sent before an acknowledged one whose time has not passed, in any state             | The longest wait that remains, never past the RTO's moment                        | Run detection again, resend what it marks, arm the RTO               |
| RTO                   | Something is in flight and neither kind above applies: after a probe, in the handshake, without SACK, and in recovery or the loss state while RACK has nothing waiting | From now, RFC 6298 with Linux's floor, doubled per expiry, under the user timeout | Resend the head, as today, and enter the loss state                  |
| Zero-window probe     | unchanged                                                                                                         | unchanged                                                                         | unchanged                                                            |
| ARP poll              | unchanged                                                                                                         | unchanged                                                                         | unchanged                                                            |

The connection keeps the moment its RTO would fire, set wherever the RTO is armed today: a send into an empty
queue, an ACK that moves SND.UNA, a resend of the head. The tail probe's deadline is the earlier of its own
wait and that moment. A probe due at the RTO's moment is still a probe. Reneging arms the RTO kind short, as
above.

The reordering timer is armed in any state, open, recovery or loss, whenever detection leaves entries waiting
(`tcp_rack_mark_lost`). When it fires and marks something on a connection that is not in recovery, the
connection enters recovery there, from the loss state too, which sets a new target; the window in recovery
starts at what is in flight plus one segment so the first resend leaves. In recovery the marked entries are
resent under the episode that is open (`tcp_rack_reo_timeout`). Entries that were still waiting are left to the
next ACK; the timer armed after the expiry is the RTO, at the moment it had.

Arming differs from today in one way that shows on the path without loss: a data segment sent while others are
unacknowledged schedules the probe again, where today only a send into an empty queue arms the timer. A lone
order arms once and its ACK cancels once, as today.

A reordering timer's expiry and a tail probe's are not retries: neither moves `retransmit_count`, and neither
is bounded by `data_retries`. The user timeout bounds both through the RTO's moment, which they never pass.

## One recovery

`LossRecovery` keeps three states: open, recovery, and loss, which is the episode after a timeout. They exist
today as two flags. What changes is what enters them and what happens inside.

**Entering recovery.** The first entry marked lost while the connection is open, by either detector
(`tcp_time_to_recover` is "something is marked lost"):

- with SACK, by RACK, on an ACK or at the reordering timer;
- without SACK, at the third duplicate ACK, with RFC 6582's guard kept: only when the acknowledgement covers
  more than `recover`, so a timeout's resends do not start a second fast retransmit. Linux reaches the same end
  by holding the recovery state until something above its mark is acknowledged.

On entry the connection records `recover`, the last sequence sent; opens the undo, with a copy of the policy's
state; tells the policy of the congestion, which sets its target; and starts PRR.

**In recovery.** Each ACK runs the undo's checks, then detection, then PRR, then the resends. Without SACK a
partial ACK marks the new head, as RFC 6582 has it and Linux's `tcp_newreno_mark_lost` does. New data may leave
as far as the window in recovery exceeds what is in flight. A resend found lost again within recovery is
resent under the same episode and reduces nothing further.

**Leaving recovery.** On the ACK that covers `recover`. Unless the episode was undone, the policy's window
becomes its target, PRR's last step. RACK's count of recoveries is updated.

**The timeout.** The RTO's expiry gives up or resends the head exactly as today: the user timeout, the retry
cap, the backoff, one segment at the expiry itself. What it marks changes (`tcp_enter_loss`,
`tcp_timeout_mark_lost`):

- the head is lost;
- with SACK, every other entry not reported received and last sent at least RACK's round trip ago is lost;
  later ones are left to RACK;
- on reneging, as above, every received mark is cleared and everything is lost;
- without SACK, the duplicate count is cleared and everything is lost.

The connection enters the loss state, records `recover` and forgets a probe that was out. The policy's target
is set again only when this window was not already reduced: when the connection was open, or the last
episode's `recover` is already acknowledged, or the connection was in the loss state and this is the first
expiry since an ACK of new data. In every case its window restarts at what is in flight after the marking plus
one segment, which is one segment when everything was marked.

**In the loss state** (`tcp_process_loss`) the policy hears of each ACK of new data and grows by slow start;
the marked entries are resent, lowest first, as the window allows; RACK keeps marking, with a reordering
window of zero unless reordering was seen; and the timer is the RTO, or the reordering timer while RACK has
entries waiting. No tail probe is scheduled. The state ends at the ACK that covers `recover`, or by an undo. A
resend found lost again in the loss state moves the connection to recovery, which sets a new target, as Linux
does. Today's rule, one head resent per ACK of new data, goes.

**A probe that repaired a loss** is the third way the policy hears of congestion: told once, the window set to
the target at once, no state entered.

## The window in recovery, and the policy concept

**PRR.** RFC 9937, in bytes, as written: its initialisation on entering recovery, its per-ACK steps, its count
of what was sent, and its completion.

- With SACK the data an ACK delivered is the change in SND.UNA plus the change in bytes reported received.
  Without SACK it is one segment per duplicate ACK and, when SND.UNA moves, the change less one segment per
  duplicate that preceded it, never beyond the flight recorded on entry.
- While what is in flight exceeds the target, sending follows the proportional part. At or below the target it
  follows the conservative bound, plus one segment when the ACK moved SND.UNA and marked nothing new lost: the
  RFC's `SafeACK`, and the same test as Linux's `tcp_cwnd_reduction`.
- The first step of an episode always allows one segment, so the first resend leaves at once.
- The window in recovery is what is in flight plus what PRR allows this step, and both resends and new data
  are counted against it and into what PRR counts as sent.

Linux divides the proportional part by the window it had on entry; RFC 9937 divides by the flight it recorded,
`RecoverFS`. The RFC is definite, so Aloe follows the RFC.

RFC 8985 (9.3) says a segment RACK marks "MUST NOT be retransmitted until congestion control deems this
appropriate". The resends therefore go under the same window as new data, lowest sequence first, as Linux's
`tcp_xmit_retransmit_queue` sends them.

**A policy with no target.** PRR regulates the way from the window a policy had to the target it set. A policy
whose target is unbounded has nothing to regulate: PRR does not run and its window stands, as Linux leaves the
window alone when the threshold is infinite (`tcp_end_cwnd_reduction`). That is `NoCongestionControl`, for
which this whole section is empty, and it is the door a policy that owns its window in recovery, BBR, will use.

**The concept.** The policy still answers one question, how much may be in flight, and is told what happened.
What it is told changes:

| Hook                                   | Called                                                                                                   |
|----------------------------------------|----------------------------------------------------------------------------------------------------------|
| `reset(state, mss)`                    | At `Established`: the initial window. Unchanged.                                                         |
| `window(state)`                        | By `writable()`, outside recovery. Unchanged.                                                            |
| `on_ack(state, info)`                  | On an ACK of new data, open or in the loss state. `info` says whether the window was in use.             |
| `on_congestion(state, flight, mss)`    | A loss was found, or a timeout is due a new reduction: the policy sets its target. Replaces `on_loss`.   |
| `target(state)`                        | By PRR: the target, RFC 5681's `ssthresh`. New.                                                          |
| `on_recovery_end(state)`               | Recovery ended, or a probe repaired a loss: the window becomes the target. Loses its flight.             |
| `on_timeout(state, in_flight, mss)`    | The RTO expired: the window restarts at `in_flight` plus one segment. It no longer sets the target.      |
| `on_idle(state, mss)`                  | Unchanged.                                                                                               |
| `on_undo(state, prior)`                | A reduction was needless: `prior` is the state copied when the episode began. New.                       |

`on_dupack` and `on_partial_ack` go; they existed for RFC 6582's inflation and deflation, which PRR replaces.
`AckInfo` gains whether the window was in use, and its delivered count now includes bytes reported by a block.

**`NewReno`** keeps RFC 5681's slow start and congestion avoidance, RFC 6928's initial window and its restart
after idle. Its target on congestion stays RFC 5681's, `max(flight / 2, 2 mss)`, where Linux's reno halves the
window. Its undo restores the larger of each pair: the window it has and the one it had, the target it has and
the one it had, as Linux's `tcp_undo_cwnd_reduction` and `tcp_reno_undo_cwnd` do; Linux's habit of recording the
old target as at least three quarters of the old window is not copied. **`NoCongestionControl`** keeps an
unbounded window and an unbounded target, and every hook stays empty.

## Undo

A resend is needless when the original arrived. Then the reduction it caused is taken back.

**What is tracked** (`tcp_init_undo`). From the start of an episode, in recovery or in the loss state: SND.UNA
at that moment, the policy's state at that moment, the stamp of the episode's first resend, and how many
resends may still be reported as duplicates. That count starts with a tail probe that is still out, grows with
each resend of the episode, and drops with each duplicate SACK for resent data at or above the episode's start.

**The two signs** (`tcp_may_undo`):

- **Duplicate SACK**: the count reached zero, so every resend was reported as a duplicate and every original
  arrived (`tcp_try_undo_dsack`). This sign is honoured after the episode has ended too, until the next episode
  begins, since the reports trail the resends.
- **The ACK answers an original** (`tcp_packet_delayed`): an ACK that moves SND.UNA echoes a timestamp older
  than the episode's first resend, on a connection that uses timestamps; or it arrives when nothing of the
  episode has been resent yet, which the busy rule makes possible. It is checked on a partial ACK in recovery,
  on the ACK that ends recovery, and on an ACK in the loss state, where it recognises a needless timeout
  (`tcp_try_undo_partial`, `tcp_try_undo_recovery`, `tcp_try_undo_loss`).

**What an undo does** (`tcp_undo_cwnd_reduction`). The policy's `on_undo` runs with the state copied at the
start, and the undo is closed, so an episode is undone at most once. Beyond that:

- on a partial ACK in recovery, reordering is recorded for RACK at once; the undo itself waits until no resend
  of the episode is still out, and until then that ACK marks and resends nothing more. Then every lost mark is
  cleared and the connection is open;
- on the ACK that ends recovery, the connection is open with its window restored in place of the target;
- in the loss state every lost mark is cleared, the retry count is cleared and the connection is open;
- by duplicate SACK the marks stay, and RACK looks at the queue again.

After an undo RACK runs detection again, so a real loss behind a reordering starts a new episode.

The undo is counted by its sign. The timestamp clock ticks in milliseconds, so an original and a resend within
the same millisecond cannot be told apart and no undo follows; that errs on the side of the reduction.

## Window growth on a flow the application limits

Today NewReno grows its window on every ACK, whether or not the window limited what was sent, so a flow that
sends little accumulates a window nothing has tested. Linux grows only when the window was in use
(`tcp_reno_cong_avoid` returns at once unless `tcp_is_cwnd_limited`):

- when the last send of the round left what is in flight at the window or was held back by it;
- or, in slow start, when the window is less than twice the largest amount in flight in the round.

The connection keeps what that needs, by `tcp_cwnd_validate`'s rule at each send: whether the window limited
the send, the largest amount in flight, and the sequence that ends the round. It passes the answer to the
policy in `AckInfo`, and `NewReno::on_ack` returns without growing when the window was not in use. As today,
only an ACK of new data reaches `on_ack`, and none does in recovery.

A venue client that sends an order at a time therefore keeps its initial window, ten segments, for good. At the
default `send_segments` of 32 a burst of small orders still fits it many times over.

Linux also shrinks an unused window after an RTO of idleness (RFC 2861, `tcp_cwnd_application_limited`). That
is not built: the restart after an idle RTO, which exists, covers the idle case, and the rest is throughput
tuning for a bulk sender.

## Resending

`Transmitter::resend_head` becomes the resend of one entry. What it does to that entry is what it does to the
head today: reconcile a copy that is out; reclaim from the device if the packet is lent; the busy rule if it
still is; trim the acknowledged front when SND.UNA passed the entry's first byte, which only the head can
need; write the headers over the old ones and lend the packet again.

The resend pass walks the entries marked lost, lowest sequence first, and stops when the window allows no more
(`tcp_xmit_retransmit_queue`). A connection is on the deferred list while any of its marked entries waits for
its packet, and the flush runs its pass again.

The resend pass and the timer are told apart as today: a resend at the RTO's expiry is counted as a timer
resend and takes a retry; a resend of a marked entry and a tail probe are not and do not. A resend that leaves
clears the entry's lost mark, counts against PRR in recovery, and counts among the episode's resends for the
undo. When the entry resent is the head, the RTO is armed from now, unless the reordering timer is pending.

## Where this departs from the RFCs and from Linux

The author requires every departure to be documented where a user of the brick reads it. This list is the
source for the TCP page's note of the same name, and each departure carries a comment at its code that names
the RFC section or the Linux function it departs from.

From an RFC's procedure, one:

- **A second tail probe before the first's verdict is known** (RFC 8985 7.3, `TLP_send_probe`, and 7.4.2). The
  RFC and Linux send none; Aloe sends one once the first probe's end is acknowledged, and closes the open
  verdict as a loss. It keeps the limit 7.3 states, one probe in flight. Reason: two lone orders lost one after
  the other.

Where an RFC leaves the choice and Aloe does not do what Linux does:

- **The allowance for a lone segment** is a setting, 40 ms by default (RFC 8985 7.2, "MAY inflate"); Linux adds
  its minimum RTO, 200 ms, which makes a lone segment's probe fire at the RTO's time.
- **No floor under the probe's wait or the reordering timer** beyond the wheel's tick; Linux adds 2 ms to each.
- **A probe never carries new data** (RFC 8985 7.3, "SHOULD transmit a previously unsent data segment, if one
  exists"): none exists, since the brick has no send buffer.

Where an RFC is definite and Linux differs, Aloe follows the RFC:

- **PRR's proportional part divides by the flight recorded on entry** (RFC 9937 6.2, `RecoverFS`); Linux
  divides by the window it had.
- **The target on congestion is half of the flight** (RFC 5681 3.1); Linux's reno halves the window.
- **A connection without SACK guards the entry to recovery by `recover`** (RFC 6582); Linux holds the
  recovery state until something above its mark is acknowledged, to the same end.

Not built, where Linux has it:

- F-RTO (RFC 5682); shrinking a window the application left unused (RFC 2861); recording the old target as at
  least three quarters of the old window for the undo.

Everything else in this document is the RFC's rule, and Linux's where the RFC leaves a choice.

## Configuration, counters and what the application sees

`TcpConfig` gains one field:

| Field                  | Default | Meaning                                                                                              | Rejected when                 |
|------------------------|---------|------------------------------------------------------------------------------------------------------|-------------------------------|
| `tail_probe_ack_delay` | 40 ms   | What a tail probe waits beyond two round trips when one segment is in flight: the peer's delayed ACK. | negative, or above 500 ms     |

`sack` keeps its meaning and widens: off, a connection neither reports nor reads blocks, and its sender detects
by duplicates and the timer.

Counters, by meaning; the names are the builder's:

- ACKs received that carried SACK blocks, and duplicate SACKs received;
- segments marked lost by RACK, and of them, resends marked lost again;
- expiries of the reordering timer;
- times reordering was seen;
- tail probes sent; of them, probes found needless; losses a probe repaired; verdicts closed as a loss by a
  second probe, which is how often the departure from RFC 8985 was used;
- undos, by duplicate SACK and by an ACK that answered an original;
- reneging seen.

Existing counters keep their names and narrow or widen: `retransmits` counts the resends at an expiry of the
RTO, one per expiry, and no longer one per ACK of the episode after it; `fast_retransmits` counts every resend
of a marked entry, in recovery and in the loss state; `recoveries` counts episodes of recovery entered, by
either detector; `duplicate_acks` keeps its test.

The application sees no new call and no new event. For tests and monitoring the connection reports its
recovery state, what it holds as in flight, received and lost, the reordering window, and whether a probe is
out; `Stack::sent_segment` shows an entry's marks.

## Parts, and where the state lives

By the rule that a class owns one concern's state. The names are provisional and the author rules on them.

- **`RetransmissionQueue`** gains the scoreboard: the marks' totals, marking from a block, marking lost,
  clearing at a resend, what is in flight, and the walk RACK and the resend pass use. `SentSegment` gains the
  two marks.
- **`Rack`** (new) owns RACK's per-connection state: the update from newly reported entries, the reordering
  window, detection over the queue, and the wait that remains.
- **`TailProbe`** (new) owns the probe's state: whether one is in flight and where it ends, whether its verdict
  is open, whether a sample was taken since, the wait, and what an ACK or a second probe means for it.
- **`RateReduction`** (new) owns PRR's state: the delivered and sent counts, the flight on entry, and the
  window in recovery.
- **`LossRecovery`** is reshaped. It keeps the state (open, recovery, loss), `recover`, the duplicate count
  for a peer without SACK, the policy's state and the undo's record, and it holds `RateReduction`. It loses
  the limited-transmit allowance and the `Resend` result: it says which state an event leaves the connection
  in, and the entries are marked on the queue.
- **`SendSequence`** is unchanged; what is in flight is the queue's, since it is the scoreboard's arithmetic.
- **`ControlBlock`** gains `Rack` and `TailProbe`, the RTO's moment beside the timer, what the rule on growth
  keeps, and two timer kinds. The parts an in-order ACK touches stay together; the rest sits behind them.

The procedures stay where the split put them. The **AckProcessor** runs "The life of an acknowledgement" in
order and owns no state. The **Transmitter** owns the resend of an entry, the resend pass, the choice and
arming of the timer's kind, and what each expiry does; it says what an expiry leaves for the Lifecycle through
`TimerExpiry`, which gains nothing. The **Receiver** passes the parsed blocks with the header. The
**Lifecycle** is not touched beyond clearing the new parts with the block. Nothing changes direction.

## The tick path

For a connection that uses SACK, with the defaults, nothing lost and nothing reordered:

- **an inbound tick** that acknowledges nothing new: nothing. Its options are walked as today and hold no SACK
  option;
- **an ACK of the order**, usually on the venue's reply: RACK's fields written and one comparison; one branch
  for the probe; the policy's growth skipped when the window was not in use, which is less work than today;
  the timer cancelled as today, or armed again with the probe's deadline in place of the RTO's;
- **the order's send**: two subtractions more in `writable()`; what the rule on growth keeps, updated; the
  probe's deadline computed, a shift, an add and a comparison with the RTO's moment, and one arm of the wheel
  as today. A second order sent while the first is out arms the wheel once more than today;
- no entry walked, no block read, no state entered, no copy.

The check is the tick-to-trade benchmark on the finished branch against `main` at the branch's base, both built
`release` on the same machine, before and after run alternately three times each and compared by medians, with
the `tcp` leg broken out. The bar is the one the author set for step 4: no change beyond run-to-run noise, 10 to
20 ns, at p50 and p99 on the 64-byte tick at level 0, cross-core. A change beyond it is fixed, or merged with
its number and its reason on the benchmark page.

## Fixtures and the scenario runner

- **The scenario runner** gains the configuration line `--tcp_tail_probe_ack_delay_ms`. Its parser, notation and
  matcher already carry the blocks in both directions.
- **`frames`** keeps its SACK option writer; the brick's parser now reads blocks, so the fixture's own reader
  stays only for parsing the frames the stack sends.
- **The fabric** is unchanged: scripted loss, duplication, reordering and delay per frame exist.
- **Existing scripts change** for three reasons, each to be derived, not pasted from a failing run:
  - the windows in the recovery and congestion scripts follow PRR and the rule on growth;
  - a script whose peer offers `sackOK` and that leaves data unacknowledged shows a tail probe before the RTO.
    A script that is about the timer keeps a peer without SACK or shows the probe; which, is per script;
  - `sack/peer-sack-blocks-are-skipped.pkt` is replaced by its opposite.
- **The benchmark's venue** is unchanged. Its sanity check asserts zero for the new counters of loss.

## Testing

Unit tests of the parts, each pinning behaviour that could regress:

- the scoreboard: whole entries marked and part covers ignored; each kind of invalid block ignored; a
  duplicate SACK recognised in both of its forms, with nothing outstanding, and disbelieved in both of its
  cases; the totals and what is in flight after every mark, resend and cumulative ACK; reneging; all of it
  across the wrap of the sequence space;
- RACK: the most recently sent segment chosen at equal stamps by sequence; a resend's ACK ignored when it
  cannot be for the resend; each of the four ways reordering is seen; the reordering window in each of its
  cases, its growth per round with a duplicate SACK, never for a probe's, and its return after 16 recoveries;
  detection with the wait that remains; a resend marked lost again; constant work on an in-order ACK;
- the tail probe: the wait with no sample, with one segment, with several, and capped at the RTO; the
  allowance at zero; no probe while one is in flight or before a sample; each meaning of the probe's ACK; a
  second probe once the first is acknowledged, closing the open verdict as a loss and telling the policy once;
  the RTO from now after a probe and at its old moment after a skipped one;
- PRR: RFC 9937's worked examples as vectors, with and without SACK; the first resend always allowed; an
  unbounded target leaving the window alone;
- the undo: by duplicate SACK, during an episode and after it; by an ACK that answers an original, in recovery
  with a resend still out and with none, at the end of recovery and in the loss state; never twice;
- growth: no growth while the window is unused, in slow start and in congestion avoidance.

A randomized test of the sender's parts together: a stream sent through a model channel that loses, reorders,
duplicates and delays, with a receiver that reports blocks and duplicates and sometimes reneges. After every
step what is in flight is not negative and the totals agree with the entries; no entry marked received is
resent; every byte is delivered; the run ends open with an empty queue.

Tests of the brick, in the existing files by topic: a lone segment lost and probed at two round trips plus the
allowance; two lone segments lost one after the other, both probed with timestamps and the second left to the
RTO without them; the tail of a burst lost and probed at two round trips; a probe answered by a duplicate SACK leaving
the window alone, and one that repaired a loss cutting it once; a loss in the middle marked by the reordering
timer, and at once when three segments are reported; a late segment cancelling the timer; a lost resend found
without the timer; the timeout marking by RACK and the loss state's resends under slow start; reneging; a
peer without SACK recovering by three duplicates under PRR; the busy rule and the copy on an entry that is not
the head, and on a probe; the five timer kinds never armed together; `NoCongestionControl` resending the same
and reducing nothing; the retry cap and the user timeout unmoved by probes and reordering expiries.

Scenario scripts, Aloe's own, run in both checksum modes and under both policies unless a script names one,
by area:

- the tail probe: a lone segment, a burst's tail, the allowance at zero, the probe capped at the RTO, the
  three answers to a probe, no second probe while one is in flight, and two lone segments lost in a row, whose
  script says in its header that Linux sends no second probe;
- RACK: a middle loss with one later segment reported, with three, a late arrival, a lost resend, the window
  growing after a duplicate SACK, a timeout followed by RACK's marks;
- the scoreboard: blocks read, a part cover ignored, an invalid block ignored, reneging;
- recovery: PRR's proportional part and its slow-start part, entry by duplicates without SACK, the full ACK
  ending it, the guard against a second fast retransmit after a timeout;
- the undo: by duplicate SACK, by timestamp after a needless timeout;
- growth: an application-limited flow keeping its window, a window-limited one growing.

Linux's corpus as a reading reference, as found on 2026-11-19 (kernel `tools/testing/selftests/net/packetdrill/`
at 4405c18c95d2, google/packetdrill `gtests/net/tcp/` at 15ab46ae):

- PRR: `fast_recovery/prr-ss-10pkt-lost-1`, `prr-ss-30pkt-lost-1_4-11_16`, `prr-ss-30pkt-lost1_4`,
  `prr-ss-ack-below-snd_una-cubic`, in both;
- fast retransmit and limited transmit: `fast_retransmit/fr-4pkt`, `fr-4pkt-sack`, `fr-4pkt-fack-last-byte`,
  `fr-4pkt-fack-last-mss` (packetdrill only), `limited_transmit/limited-transmit-sack` and `-no-sack` (both),
  `tcp_bidir_fast_retransmit_nosack` (kernel only);
- SACK marking: `sack/sack-shift-sacked-2-6-8-3-9-nofack`, `-7-3-4-8-9-fack`, `-7-5-6-8-9-fack`, in both;
- RACK's reordering timer: `cubic/cubic-rack-reo-timeout-retrans-failed-incoming-data` (packetdrill only);
- none for the tail probe, the undo, a duplicate SACK at the sender, the reordering window's adaptation or
  reneging. Those scripts rest on the RFCs' examples and on the functions named in this document.

Two Aloe stacks on the fabric, over a path with a delay, so the round trip is not zero:

- the final segment of a flight dropped once: resent by the probe well before the RTO, with no expiry of the
  RTO counted;
- one frame held behind the next: no resend;
- a frame held behind four: one needless resend the first time, undone, and none the second time;
- a transfer under random loss and reordering in both directions: every byte delivered in order, and new data
  flowing during recovery, which closes the case step 4 recorded as short.

Against Linux over the tap, as root, by hand, the stack sending through `LossyDevice`: with the first
transmission of the last segment of each flight dropped, lone segments included and one after the other, every
tail loss recovers with a tail probe and no expiry of the RTO; with one segment in
fifty dropped, the transfer completes with RTO expiries a small share of the losses; with frames held behind
their successors, the kernel's count of duplicate SACKs sent stays near zero after the first. The run is owed to
a machine with root, as the earlier tap runs are.

The benchmark rerun, as under "The tick path".

## Documentation

- `docs/architecture/tcp.md`: the congestion-control section's hook table and both models; the counter and
  configuration tables; the design note on loss recovery rewritten around the scoreboard, RACK, the probe, one
  recovery, PRR and the undo; the timer note with its five kinds; the note on two Aloe stacks removed; the
  note on what is left out updated, with F-RTO, the part cover and the unused window.
- `docs/architecture/tcp.md` also gains a design note of its own, "Where the brick departs from the RFCs and
  from Linux", carrying the list of that section of this document: the second probe first, as the one
  departure from an RFC's procedure, with the RFC section, what Linux does, why Aloe differs and what a user
  sees. The setting's row in the configuration table says that its default differs from Linux's.
- The code: a comment at each departure, naming the RFC section or the Linux function.
- `docs/roadmap.md`: step 5 landed, with what it took in beyond its first text, the departure in one clause,
  and what is owed to a root machine.
- `docs/architecture/scenario.md`: the new configuration line, the new scripts in its table, and its paragraph
  on how Aloe's scripts differ from Linux's.
- `docs/architecture/fixtures.md`: if `frames` changes.
- `docs/benchmarks/tick-to-trade.md`: the rerun, with its result file.
- `AGENTS.md`: the opening paragraph and the layout row for `tcp`, and the traps this step finds.

## Delivery

One branch, `tcp/rack-tlp`, from `main` at 1aeddb9, and one pull request, as the author decided. Every commit
leaves the four presets green. The order that keeps it so and derives the existing scripts once:

1. the wire parser reads the blocks;
2. the scoreboard on the queue, fed by the blocks, with its totals and the delivered count; nothing reads
   them yet, `writable()` keeps today's rule, and behaviour does not change;
3. the resend of any entry, the resend pass and the deferred list per connection;
4. the one recovery on today's detection, duplicates for every peer: the states, PRR, the concept's new hooks,
   both models, the rule on growth, and `writable()` and the commit moved to what is in flight, with the
   limited-transmit allowance removed in the same commit as the inflation it would otherwise count twice; the
   recovery and congestion scripts are derived here, once;
5. RACK and the reordering timer on connections that use SACK, and the timeout's marking;
6. the tail probe, its setting and the runner's line; the timer scripts are derived here;
7. the undo;
8. the two-stack tests, the tap cases, the benchmark rerun, the documentation.

The pull request closes #20 with a comment that states what landed and what is owed.

## Consequences for later steps

- **Step 6** raises its fast-path-exit event where a connection leaves the open state or sends a probe, reads
  the scoreboard when a shrunken MSS makes a resend split, and may add F-RTO if the netem comparison asks.
- **BBR** arrives as a policy with an unbounded target that owns its window in recovery, reads the delivered
  count that now includes reported bytes, and adds pacing; PRR and the undo do not run for it.
- **The phase's comparison against Linux under netem** is the first place a bulk sender's recovery is measured.
  What it may ask for is recorded here so it is not rediscovered: F-RTO; shrinking an unused window; splitting
  an entry a block covers in part, which needs a copy.

## Decisions made in the brainstorm

The author's:

- The congestion-control changes are part of this step and this pull request ("I changed my mind. Lets include
  congestion control changes in this PR").
- One recovery, not two side by side ("Yeah, lets go with one recovery").
- Resending from the kept packet, headers rewritten in place, is acceptable for a probe ("Work with a packet
  holds").
- A second probe goes once the first is acknowledged, though its verdict is open, where RFC 8985's procedure
  and Linux send none; and the departure is documented ("Yeah, lets go with 2. Probing opportunisitcally seems
  like a good option. That must be documented(behavior deviates from the actual RFC)"). The question came from
  the builder check: by the RFC, the second of two lone orders lost in a row waits for the RTO.
- Standing from earlier steps: the RFCs are the specification and Linux settles choices; no copy on the data
  path; the scripts are Aloe's own; the bar for the tick path is no change beyond noise, measured by
  tick-to-trade on the finished branch.

Claude's calls, stated in the brainstorm and not objected to:

- The lone segment's allowance is a setting, 40 ms by default, where Linux fixes it at 200 ms.
- One send timer with five kinds.
- PRR, growth only while the window is used, and the undo are the congestion-control changes taken in; F-RTO
  stays out.
- The scoreboard is marks on the queue's entries, with no list of holes.
- No tail probe without RACK.

Settled while writing, Claude's calls, listed for the author to overrule:

- A block that covers part of an entry marks nothing, as in Linux for a buffer of one segment.
- A SACK option with a bad length is ignored and its segment kept, as Linux does, unlike the other options.
- No floor under the probe's wait or the reordering timer beyond the wheel's tick, where Linux adds 2 ms to
  each.
- Before the first round-trip sample the probe waits `rto_initial`.
- The allowance applies to exactly one segment in flight, and is refused above 500 ms.
- A probe is skipped before a round-trip sample was taken since the last one, RFC 8985's second condition.
- No switch for RACK or the probe apart from `sack`, and no setting for the reordering window.
- A connection without SACK keeps RFC 6582's guard on entering recovery, and gets no RACK and no probe.
- The handshake keeps the timer alone.
- PRR as RFC 9937 writes it, dividing by the flight recorded on entry, where Linux divides by the window.
- The policy's target on congestion stays RFC 5681's half of the flight, where Linux halves the window.
- PRR does not run for a policy whose target is unbounded.
- The undo hands the policy the state copied at the episode's start; the concept's hooks as tabled, with the
  target set by `on_congestion` alone and `on_timeout` restarting the window.
- Linux's shrinking of an unused window is not built.
- `retransmits` and `fast_retransmits` keep their names with the meanings above.
- The names `Rack`, `TailProbe`, `RateReduction`, `tail_probe_ack_delay`, `on_congestion`, `target` and
  `on_undo`.
- The order of delivery, with the one recovery landing on duplicates before RACK replaces them for SACK peers.

Settled after the builder check, Claude's calls, listed for the author to overrule:

- The blocks of an ACK below SND.UNA are read, as Linux reads them, unless the ACK is older than the largest
  window the peer offered.
- The reordering timer is armed in any state, and an expiry that marks something outside recovery enters
  recovery, from the loss state too, as Linux's does.
- A duplicate SACK is recognised before any validity test; validity gates only what touches the entries.
- The undo's checks run before detection on every ACK.
- `writable()` moves to what is in flight in the commit that removes RFC 6582's inflation, not before.

## Left to the builder

- The names and layouts of the new parts' members, the counter names, and the connection's new accessors.
- How the parsed blocks reach the AckProcessor, and how `Freed` carries what RACK and PRR need from the
  entries a cumulative ACK frees.
- How the minimum round trip's window is kept, and how a round is marked for the reordering window's growth.
- How the resend pass remembers where it stopped, and how the deferred list's flush finds the waiting entries.
- The exact arithmetic of PRR's steps within RFC 9937, and of the probe's wait.
- Which existing test files take which cases, the scripts' names and folders under `tests/scenarios/tcp/`,
  and for each existing script with a `sackOK` peer whether it shows the probe or drops the option.
- How `LossyDevice` drops a flight's last segment and holds a frame behind its successors, the delay the
  two-stack tests give their path, and which kernel counters the tap cases read.
- The wording of the closing comment on #20 and of the traps added to `AGENTS.md`.
