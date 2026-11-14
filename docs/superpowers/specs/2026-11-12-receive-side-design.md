# The receive side: out-of-order segments held as packets, SACK blocks, window scaling, delayed ACK and silly-window avoidance

Status: design, from the brainstorm of 2026-11-11 and 12, reviewed against the code and the Linux source on
2026-11-12 with the findings folded in, for issue #19 (phase 2, step 4 of `docs/roadmap.md`).
It rests on the reliable-send spec of 2026-10-28 for the send side it touches and on the stack-split design of
2026-11-09 for the parts it grows. The issue's premises were checked against the code on `main` at 6a779fb,
after the split merged as #38; where a premise failed, this document says so. Private, like every file under
`docs/superpowers/`; the public summary is the roadmap's step 4 and the TCP page once the work lands.

## Goal

Today a segment that arrives above a gap is dropped and answered with a duplicate ACK, so the peer must resend
it and everything after it. One lost or late segment on a venue's market-data stream costs the whole flight
behind it, and against a Linux venue the missing SACK blocks leave its sender with duplicate ACKs alone. The
window cannot pass 65535 bytes, which caps a stream at one such window per round trip; every `consume` that
moves the edge costs a window update; and a full-sized timestamped stream meets the packet cap one segment
before it fills the byte window, a regression step 3 left for this step.

This step makes the receive side complete without adding a copy to it. A segment above a gap is kept as the
packet it arrived in, on a second chain beside the in-order one, and joins the in-order chain by a change of
links when the gap fills. The peer is told what is held with SACK blocks (RFC 2018) and what arrived twice with
duplicate SACK (RFC 2883). The window is scaled (RFC 7323) and sized by configuration. Acknowledgements may be
delayed by a configured time (RFC 1122, RFC 5681), off by default. The offered edge moves by full segments, not
by every byte the application reads (RFC 1122 4.2.3.3).

The first deployment is a client holding long-lived connections to trading venues. Its receive side carries
market data: bursts of small messages, large snapshots over a long path, and a peer that is a Linux server.

## Goals, non-goals, anti-goals

Goals:

- Two Aloe stacks on the fabric deliver every byte in order under scripted reordering and loss, and the
  receiver holds what arrives above a gap instead of dropping it.
- Against Linux over a tap device the kernel's SACK-permitted and window-scale options are negotiated and
  honoured, and under netem reordering and loss the kernel recovers from the stack's SACK blocks by fast
  retransmit, not by waiting out its timer.
- No copy on the receive path, with or without loss, with or without memory pressure. An out-of-order segment
  is the packet it arrived in; an overlap is trimmed by moving an offset and a length.
- What is held is bounded: a connection holds at most `receive_segments` packets, in order and out of order
  together, and the shard's connections share `receive_pool` nodes. Nothing an out-of-order peer sends changes
  either bound.
- The RFCs are the specification; where they leave a choice, Aloe does what Linux does, and the plan reads the
  Linux source named under "Constraints" before writing each part.
- The window, the packet cap, SACK, window scaling and the ACK delay are run-time configuration. A wrong value
  costs throughput or memory, never correctness.
- The tick path stays as it is: an in-order segment on a connection that holds nothing out of order gains one
  branch, one shift and one compare, the order that answers it two shifts, and the tick-to-trade benchmark on
  the finished branch shows no change against `main` beyond run-to-run noise.

Non-goals, each with the step that owns it:

- RACK-TLP, PRR, and any use of the SACK blocks the peer sends: step 5. This step skips the peer's blocks as it
  skips any option it does not read.
- Receive-window auto-tuning (Linux's dynamic right-sizing): not planned. The window is what the configuration
  says, from the handshake on.
- Collapsing small held segments into fewer packets by copying, as Linux's `tcp_collapse` does under memory
  pressure: not built. If a deployment needs it, it arrives as a counted fallback, as the retransmit copy did.
- Linux's timer for compressing the ACKs of out-of-order arrivals (`tcp_comp_sack_*`), and its adaptive
  delayed ACK (the ATO estimate, quick-ACK and ping-pong modes): not built. The batch boundary a polling stack
  already has, the `flush` of the tick, does the first job, and the delay is one fixed duration.
- Congestion-window growth on a flow the application limits, the follow-up step 3's hand-off asked to settle
  "before window scaling lets the window matter": not this step. NewReno still grows its window on every ACK.
  At the default `send_segments` a connection's flight stays below 65535 bytes, so the peer's scaled window
  does not become the limit that the congestion window was hiding behind; a deployment that raises the send
  cap for bulk sends meets it. It goes with step 5, which reworks the policy's recovery anyway, and the
  closing comment on #19 names it as open.
- RFC 5961's challenge ACK for a RST or SYN inside the window, TIME_WAIT, keepalive, path MTU: step 6. The
  default window is eight times the old one, so until then a blind RST has eight times the sequence space to
  land in.
- A FIN forged into the window, the same exposure with the same eightfold target (found in the whole-branch review
  of 2026-11-14): it is remembered, and in-order data that crosses it is refused each time it is resent, so the
  stream stalls until the peer gives up, where Linux usually drops the stale FIN once real data passes it. Step 6's
  challenge ACK covers it with the RST.
- A floor under the peer's MSS: step 6, which already lists "refusing a peer MSS of zero". The receive budget
  now reads `mss()`, so a peer that states an MSS of twelve bytes or fewer beside timestamps is offered a zero
  window, where today it only stops the stack's own sends. It harms that peer alone.
- A compile-time policy that drops out-of-order segments instead of holding them: recorded under
  "Consequences", built only if the benchmark asks for it.
- New events, new senders, any change to the stream contract or the runtime: none. `unread()` stays a view over
  the in-order packets.

Anti-goals, which would make this a failure even if it built and passed:

- A copy of payload on any receive path.
- A SACK block that reports data the connection does not hold. The number of a FIN it remembers is not data.
- An offered right edge that moves left, as the peer reconstructs it from the scaled field.
- Out-of-order data that can pin more packets than the cap and the pool allow, or that survives the connection.
- Logging, allocation beyond the fixed pools, or a clock read inside the brick.
- A scenario script copied from Linux's corpus.
- A regression of the benchmark's 64-byte tick that is merged without a number and a reason beside it.

## What the issue assumed, checked

Checked against `main` at 6a779fb:

- **"A segment above a gap is dropped"** is `Receiver::accept_sequence`: `sequence.after(next)` counts
  `dropped_out_of_order`, records the gap's end in the ACK policy and answers with a pure ACK. The segment goes
  no further, so its ACK field and its window are ignored too. Holding it means it passes the sequence check
  and runs the rest of RFC 9293's order, which is a second change of behaviour: the acknowledgement an
  out-of-order segment carries is processed.
- **"A reassembly list beside the in-order chain, bounded by the connection's packet cap and the shard's
  pool"** fits what exists: `ReceiveQueue` chains `HeldSegment` nodes of the shard's `HeldSegmentPool` through
  a `NodeChain`, and one pool with one node type lets a node move from one chain to the other without an
  acquire that could fail. The node needs the sequence of its first byte, which the in-order chain never
  needed.
- **"`unread()` stays a view over packets"** holds unchanged: `ReadView` walks the in-order chain, and the
  join appends nodes to it exactly as `hold` does.
- **"Every consume that moves the edge sends a window update"** is `Receiver::consume`: it queues an ACK
  whenever `offer_of(block)` differs from the offered edge. The update leaves at the next `flush` unless a data
  segment carries it first.
- **"Delayed ACK"** today means deferred to the next `flush`, the same tick. There is no timer, and the
  control block has one wheel timer, shared by the RTO, the ARP poll and the zero-window probe. A delay needs
  a second timer.
- **"`receive_segments`, the byte window and the pool are re-derived together"** has a defect to fix first,
  recorded by step 3's hand-off: the byte budget is `receive_segments * 1460` while a timestamped peer's full
  segment carries 1448, so 32 full segments leave 384 bytes of window that only a 33rd packet could fill, and
  that packet meets the cap. The budget must count the payload a full segment carries.
- **"Depends on #17, independent of #18 otherwise"** is no longer exact. Step 3 fixed the header size of a
  connection's data segments, so a kept packet is rewritten in place and a prepared payload sits at a fixed
  offset (`OutgoingSegment::size`). SACK blocks therefore cannot ride on data segments without moving payload.
  They ride on pure ACKs, which are fresh packets framed once.
- **The scenario runner already speaks the options.** The script parser, the notation and the matcher handle
  `sackOK`, `wscale n` and `sack a:b c:d`, with the sack bases per direction; a script's `win` is the raw
  16-bit field, as in packetdrill. What the runner lacks is the configuration lines for the new fields.
- **`wire::TcpOptions` reads the MSS and the timestamps only** and skips every other kind by its length byte,
  so a peer's `sackOK`, `wscale` and SACK blocks are ignored today, not refused.
- **The largest header fits the headroom.** A pure ACK with timestamps and three blocks is 34 bytes of link and
  network header and 60 of TCP, 94 of `device::packet_headroom`'s 128. The stack's `static_assert` covers the
  SYN's option block today and must cover this.
- **`SendSequence::wnd` is already 32 bits wide**, so the peer's scaled window needs a shift and nothing wider.
  The duplicate-ACK test in `AckProcessor::on_ack` compares the raw field with `wnd` and must compare scaled.
- **Every script's window numbers change.** The 40 scripts that show a window show 46720 and its remainders.
  With the budget counted in 1448-byte segments, and again with a larger default cap, each of those numbers
  moves; so does every SYN the stack sends, which gains two options.
- **The benchmark asserts zero drops** (`stack_sanity.hpp`), including `dropped_out_of_order`; the new counters
  join that list. Its venue offers timestamps as a Linux venue would and should offer the two new options for
  the same reason.

## What other stacks do, checked

Linux settles what is correct. What is fast was read on 2026-11-12 from the stacks built for it, each at its
default branch: FreeBSD 15.0 as F-Stack ships it (its receive files are byte-identical to FreeBSD's), TLDK,
TCPDirect, Onload, lwIP, VPP's host stack, Seastar's native stack and mTCP.

- **The packet itself is held, in a sorted list, by every stack that avoids a copy**: FreeBSD, TLDK,
  TCPDirect, Onload and lwIP. None uses a tree. VPP and mTCP copy out-of-order payload into a buffer at its
  offset; Seastar copies each held segment to the heap and copies a run again each time it grows.
- **A cap in packets with the highest sequence losing** is TCPDirect's rule (64 per connection) and close to
  TLDK's (a quarter of the receive ring, the newcomer dropped). FreeBSD bounds runs, 100 per connection.
- **Held bytes win an overlap and the newcomer is trimmed** in TLDK and FreeBSD's front trim; FreeBSD, TCPDirect
  and lwIP release a held buffer the newcomer covers.
- **A FIN above a gap as one sequence number** is TLDK's representation. FreeBSD, Onload and lwIP keep it with
  its buffer; VPP, Seastar, mTCP and TCPDirect lose it and wait for the peer's resend.
- **SACK blocks on pure ACKs only** is Onload's rule, for this design's reason ("it will change packet
  length"), and lwIP's; VPP sends its duplicate SACK that way. FreeBSD, Linux and VPP put ordinary blocks on
  data segments. TLDK, TCPDirect, Seastar and mTCP send no blocks at all.
- **No delayed-ACK timer, one ACK per connection per loop iteration** is VPP, mTCP and TLDK, which is what
  `flush` does today. FreeBSD (40 ms), TCPDirect, Onload (50 ms), lwIP and Seastar delay by default; Onload
  does not delay for the first 64 KiB of a connection, after 50 ms idle or after a loss.
- **A burst of out-of-order segments is not answered one for one** by VPP (at most three per iteration unless
  the block list needs more), Linux (three, then batched, on connections that use SACK and only there), TLDK
  or Seastar (one per burst). One per burst starves a sender that counts three duplicates; three and then one
  does not start it late, but a sender without SACK also grows its window on every duplicate after the third,
  which is why Linux answers each segment when SACK is off.
- **A duplicate ACK is kept a duplicate** by Onload: the ACK for an out-of-order segment restates the window
  edge and is never carried by a data segment.
- **The window**: a fixed size and no auto-tuning everywhere but FreeBSD and Linux; the smallest shift that
  covers the buffer in Onload; the edge moved by `min(window / 2, MSS)` at least in TCPDirect and lwIP; an
  update on release when the edge can move by an eighth of the window in TCPDirect and Onload, only after a
  zero window in VPP and mTCP.
- **Packet slots against a byte window** is the trap the zero-copy stacks all meet. TCPDirect has 64 slots and
  copies small segments together when they run out, dropping only while the application holds a loan; Onload
  coalesces its receive queues under memory pressure; TLDK drops.
- **The in-order shortcut** is one predicate in TCPDirect and Onload, switched off in Onload while anything is
  held out of order, and per burst in TLDK and VPP. TLDK and Onload read the timestamp option with one compare
  when it has the usual layout.
- **Seastar returns before the ACK field** of a segment that is not at RCV.NXT, as Aloe does today, so a hole
  in one direction stalls the other.

## Constraints

- The rules of `AGENTS.md`: `tcp` links neither `execution` nor `log`; one class in one place, and a class that
  grows sections is split into classes that own their state; a header whose names are all in `detail` sits in
  the `detail/` folder of the folder it belongs to; `std::expected` on hot paths and exceptions on setup;
  every hot-path member is `noexcept` and allocates nothing but packets from IP's pool and nodes from the
  stack's fixed pools; a change is done when `debug`, `gcc-debug`, `asan` and `tsan` pass with the format and
  include checks.
- Tests need no root, no hugepages, no card. The tap and netem runs are manual tests, run by hand as root.
- The scripts are Aloe's own, in packetdrill's syntax, with Linux's corpus as a reading reference.
- The parts keep their direction: the Receiver calls the Lifecycle, the AckProcessor and the Transmitter; the
  Transmitter calls none of them; only the Lifecycle writes a connection's state.
- `docs/architecture/tcp.md`, `scenario.md`, `fixtures.md`, `docs/roadmap.md` and `AGENTS.md` change in the
  same change as the code; the benchmark page gains the rerun.
- What this design says Linux does was read from the source on 2026-11-12, torvalds/linux at 9d66f26
  (7.4-rc2), and each such sentence names its function. The plan reads the same functions again before
  writing each part and records what it relied on: `net/ipv4/tcp_input.c` (`tcp_sequence`, `tcp_data_queue`,
  `tcp_data_queue_ofo`, `tcp_ofo_queue`, `tcp_fin`, `tcp_prune_ofo_queue`, `tcp_sack_new_ofo_skb`,
  `tcp_sack_maybe_coalesce`, `tcp_sack_remove`, `tcp_dsack_set`, `tcp_dsack_extend`, `tcp_send_dupack`,
  `__tcp_ack_snd_check`, `tcp_parse_options`, `tcp_ack_update_window`), `tcp_output.c` (`tcp_select_window`,
  `__tcp_select_window`, `tcp_select_initial_window`, `tcp_options_write`, `tcp_established_options`,
  `tcp_current_mss`, `tcp_send_delayed_ack`), `tcp.c` (`__tcp_cleanup_rbuf`), and the constants
  `TCP_NUM_SACKS`, `TCP_MAX_WSCALE` and `TCP_DELACK_MIN`.

## The life of a received segment

RFC 9293's order on a synchronized connection stays: PAWS, the sequence check, RST, SYN, ACK, payload, FIN.
The sequence check now has three outcomes where it had two.

1. **Not acceptable**: entirely before RCV.NXT, past the offered edge, or any data while the window is zero.
   Dropped, counted and answered at once, as today. The answer to a duplicate carries a duplicate SACK. A
   segment that starts inside the window and ends past the edge is dropped whole, as today, wherever it
   starts; Linux does not trim one to the window either (`tcp_sequence`).
2. **In order**: it starts at RCV.NXT, or overlaps it and loses its old prefix, which is reported as a
   duplicate. The ACK is processed, the packet is held on the in-order chain, and one new question follows:
   is anything held above? When nothing is, which is every segment of a clean stream, the path ends as it does
   today. When something is, the run that now follows RCV.NXT is linked onto the in-order chain, `Readable`
   covers all of it, and the ACK goes at once.
3. **Above a gap, inside the window**: new. The segment goes on through the order. A RST resets the
   connection, RFC 793's rule for a RST inside the window, which a zero-length RST above RCV.NXT already
   follows; step 6 replaces both with RFC 5961's challenge. A SYN is dropped as today. The ACK it carries is
   processed like any other: it may free entries, move the peer's window and end recovery. Its payload goes to
   the reassembly queue, and it is answered with a pure ACK, at once or at the `flush` of the same tick as
   "Acknowledgement timing" says, with SACK blocks when the connection negotiated them. A FIN on it, or a bare
   FIN above a gap, is remembered and takes effect when RCV.NXT reaches it.

The three outcomes are for a segment that occupies sequence space: payload, or a FIN. A segment that occupies
none, a pure ACK, is judged as today: at RCV.NXT, or above it inside the window, its ACK field and its window
are processed, nothing is held and nothing is answered. The peer's pure ACKs arrive above RCV.NXT for as long
as a hole is open, and answering them would hand the peer duplicate ACKs for segments it never sent. They do
not count towards the first three of "Acknowledgement timing" either.

`TS.Recent` needs nothing new: RFC 7323's rule updates it only from a segment that starts at or before
Last.ACK.sent, which an out-of-order segment never does. PAWS applies to every segment as it does today.

## The reassembly queue

A connection's out-of-order payload is a chain of the shard's held-segment nodes, sorted by sequence, each node
the packet a segment arrived in with the offset and length of the part that is kept. The node type gains the
sequence of its first byte. The ranges in the chain are disjoint and none touches RCV.NXT: whatever reaches
RCV.NXT has joined the in-order chain.

**Inserting.** A segment above a gap is placed by sequence. Three rules keep the ranges disjoint without a
copy. The first two are Linux's (`tcp_data_queue_ofo`); the third is what a chain of spans needs where Linux
keeps a partly overlapping buffer whole and sorts the bytes out when they are read:

- A newcomer that held data covers entirely is a duplicate: dropped, counted as `dropped_duplicate`, reported
  as a duplicate SACK.
- A held node the newcomer covers entirely is released and the newcomer takes its place.
- Where the newcomer and a held node overlap in part, the held bytes win: the newcomer loses its front up to
  the end of the node before it and its tail from the start of the node after it. Trimming moves the node's
  offset, length and first sequence. No byte moves.

The common case after a loss is a run of segments in order above the gap, so a segment that starts at or after
the end of the chain's last node is appended without a walk, the fast path Linux keeps with `ooo_last_skb`.
Any other placement walks the chain from its head. The walk is bounded by `receive_segments`. A tree is not
built: with the default cap a walk is a few hundred steps on a path that only a loss reaches, and a deployment
that raises the cap into the thousands against peers it does not trust is the server case this phase leaves
for later.

**Joining.** After an in-order segment is held, the chain's head is compared with RCV.NXT. A node RCV.NXT has
passed entirely is released; one it has passed in part loses its front; then every node that starts at RCV.NXT
is unlinked from this chain and linked at the tail of the in-order chain, RCV.NXT and `unread()` moving past
it, until a node starts above RCV.NXT or the chain is empty. The join cannot fail: it takes no node and moves
no packet.

**A FIN above a gap.** The queue remembers the sequence the peer's FIN occupies, from a FIN on an out-of-order
data segment or a bare one. When RCV.NXT reaches that sequence the FIN is taken exactly as an in-order FIN is
today, through the Lifecycle, and anything still held above it is released, as Linux's `tcp_fin` purges its
queue. Data at or past a remembered FIN is refused as data after the peer's FIN is today. Linux keeps such a
FIN with its buffer in the out-of-order queue and processes it when the buffer moves (`tcp_ofo_queue`);
remembering one sequence number has the same effect without holding a packet for a bare FIN.

**Room.** In-order and out-of-order nodes count against one cap, `receive_segments`, and come from one pool.
When a segment needs a node and the connection is at its cap, or the pool is empty, the lower sequence wins,
as in Linux's `tcp_prune_ofo_queue`, which prunes from the highest sequence down and never below the incoming
segment:

- An in-order segment takes the node of the highest out-of-order segment the connection holds, which is
  released. With nothing held out of order it is dropped as today (`dropped_no_slot` or `dropped_no_node`).
- An out-of-order segment that starts below the highest held one takes that node in the same way. One that
  would be the highest itself is dropped, counted as `dropped_out_of_order`, and its end is remembered by the
  ACK policy as today, so the data that fills the gap is acknowledged at once.
- One node is released for one segment, where Linux frees an eighth of its buffer at a time: Aloe's unit is
  the packet, and a second release would give up data nothing asked for.

Releasing held data that a SACK block reported is what RFC 2018 allows a receiver to do. The block is cut back
in the same step, so no later ACK reports what is gone; Linux forgets every block at a prune, which is as
truthful and tells the sender less. A release for room also forgets a remembered FIN: the peer resends it. The
ACK policy remembers the end of a released segment as it does the end of a refused one, so in-order data is
acknowledged at once until RCV.NXT passes what the peer was once told had arrived.

**The end of a connection.** `release` returns both chains. A terminal event (`Closed`, `Reset`, `TimedOut`,
`Unresolved`) returns the out-of-order packets at once, since nothing can fill the gap any more; the in-order
packets stay readable until `release`, as today.

## SACK blocks and duplicate SACK

**Negotiation.** With `sack` on, a SYN offers SACK-permitted, and a SYN-ACK carries it when the peer's SYN did.
The option is in use when both ends sent it, exactly as the timestamps are negotiated. Without it the
connection still holds out-of-order data and answers with plain duplicate ACKs, one at once for every segment
above a gap ("Acknowledgement timing").

**What is reported.** The connection keeps up to four blocks, Linux's `TCP_NUM_SACKS`, in the order RFC 2018
section 4 asks for: the block that contains the segment just received first, then the others, most recently
changed first. A new out-of-order segment extends the block it touches, merges blocks it joins, or starts a
block at the front; the oldest falls off the end when there are more than four ranges. When RCV.NXT moves,
blocks at or below it go (`tcp_sack_remove`). When a held node is released for room, its block is cut back. A
remembered FIN's sequence number is part of the block that ends at it, or a block of its own for a bare FIN,
as Linux's blocks cover the FIN of a held buffer; otherwise a Linux sender takes the segment that carried the
FIN for not yet received and sends it again.

**Where it rides.** Every pure ACK the connection sends while it holds a block, or owes a duplicate SACK,
carries the option after the timestamps: `nop, nop, sack` and eight bytes per block. Three blocks fit beside
the timestamps and four without them, and a duplicate SACK is one of them, in the first place
(`tcp_established_options`). A data segment never carries blocks, because its header size is fixed. Linux
carries them on data segments too and shrinks that segment's payload for it (`tcp_current_mss`); a kept packet
that is rewritten in place cannot. For the same reason a data segment that leaves does not cancel an ACK that
still owes blocks: that ACK goes as a pure ACK at the next `flush`. Since every out-of-order arrival is
answered with a pure ACK within its tick, the peer sees each change of the blocks as it happens.

**Duplicate SACK.** RFC 2883, which Linux sends by default (`tcp_dsack`). The first block of the next pure ACK
reports a range that arrived twice, and the regular blocks follow it:

- a segment refused as a duplicate or by PAWS that starts before RCV.NXT: its range, up to RCV.NXT
  (`tcp_send_dupack`), with the ACK at once, as such a segment is answered today;
- an in-order segment with an old prefix: the prefix, from its sequence to the RCV.NXT it met, with the ACK at
  its ordinary time, as Linux leaves it (`tcp_data_queue`);
- a segment, or the part of one, already held above the gap: that range, followed by the block that holds it,
  with the ACK timed as for any segment above a gap;
- held data that an in-order segment overlaps: at the join, the part of a held node that RCV.NXT had already
  passed, with the ACK the join sends (`tcp_ofo_queue`).

One duplicate is pending at a time. A second range found while one is pending extends it when the two touch
and is dropped otherwise, as Linux's `tcp_dsack_extend` does. It is forgotten once an ACK carried it, or when
the next data segment arrives, as Linux clears it on entry to `tcp_data_queue`. A Linux sender uses the report to undo a
retransmission that reordering made spurious, which is what keeps a venue's stream fast on a path that
reorders. There is no separate switch: `sack` turns both on.

**The peer's blocks.** Skipped by their length byte, as today. Step 5 reads them.

## The receive window

**The budget.** Each connection has a byte budget, fixed when the handshake has settled the MSS and the
options. What the slots can hold is `receive_segments` full-sized segments, a full-sized segment being the
connection's `mss()`, the negotiated MSS less the timestamp option when it is in use. The budget is the
smallest of

- half of what the slots can hold, or `receive_window` when the configuration sets one;
- the whole of what the slots can hold, which caps a configured `receive_window`;
- what the window field can state: 65535 bytes shifted by our scale when the peer accepted window scaling,
  65535 bytes when it did not.

The budget is computed once the peer's MSS is stored and the connection has left SynSent, because `mss()`
reads both. Today `ReceiveQueue::open` runs before either on the active open and before the peer's MSS on the
passive one, so the budget cannot be computed where the edge is opened now; the opens are reordered.

Counting in `mss()` is the fix step 3 left, and the half is the margin for what real streams carry: a short
segment takes a whole slot, and every message that ends with a push ends with one. With the default, a stream
of full-sized segments closes the window with half the slots still free, and the slots outlast the window for
any stream whose segments average half a full one or more. The same `mss()` replaces the stack's MSS wherever
the receive side asks whether a segment is full-sized, so a peer with a small MSS is acknowledged every second
segment too.

**The scale.** One shift per stack, computed at construction: the smallest that lets the 16-bit field state the
largest budget the configuration allows, at most 14. With `window_scaling` on, every SYN and SYN-ACK offers it,
a zero shift included, since offering is also what lets the peer scale its own window. The option is in use
when both ends sent it. The window field of a SYN or SYN-ACK is never scaled. A SYN-ACK states the
connection's budget or 65535, whichever is smaller, since the peer's SYN has settled the budget by then. A SYN
is written before the connection has a budget and states the stack's largest budget or 65535, whichever is
smaller. The first segment with an ACK after the handshake makes the first offer from the connection's budget,
scaled; against a peer whose MSS is small, or under a small cap, that offer can be less than the SYN's field
said, which no data has relied on yet.

**The offer.** A segment that carries an ACK offers a right edge. The edge the connection would offer is
RCV.NXT plus the budget less the unread in-order bytes. Out-of-order bytes sit between RCV.NXT and the edge and
change nothing. That edge replaces the one already offered only when all three hold:

- a packet slot is free, counting both chains: a connection at its cap stops offering more, as today;
- it lies at least `min(budget / 2, mss())` beyond the edge already offered, RFC 1122 4.2.3.3's rule against
  silly windows, which also keeps a closed window closed until a full segment fits;
- it is to the right of the edge already offered, which it always is unless rounding put that edge ahead.

The field written is the distance from RCV.NXT to the edge shifted right by the scale, rounded up to the
scale's unit as Linux's `tcp_select_window` and `__tcp_select_window` round, and the connection records the
edge the peer will reconstruct from the field, not the one it computed. An edge recorded that way never moves
left.

It can move right with nothing consumed. Each segment that leaves after RCV.NXT moved rounds again from the
new RCV.NXT and can add up to one unit minus one byte, so over many segments the offered edge can stand ahead
of the budget by more than a unit, and a stream whose segments are no larger than the unit, each acknowledged
alone, never closes the byte window. Linux's edge creeps the same way (`tcp_select_window` aligns up the
window it will not shrink). It is harmless here because the budget is not a memory limit, the packet cap is,
and RCV.NXT moves only while a slot is free; and it is why the shift is the smallest that states the budget,
3 with the defaults, a unit of 8 bytes. The alternative, rounding one computed edge down each time, moves the
reconstructed edge left by up to a unit, which is the anti-goal.

**A consume rarely costs a segment.** `consume` queues a window update of its own only when the window it
could offer has at least doubled against the one on offer and the edge may move, Linux's rule in
`__tcp_cleanup_rbuf`. That covers the case that matters, a window that had closed or nearly closed, and leaves
every other movement of the edge to the ACKs and data segments that leave anyway.

**Two limits, still independent.** The byte window and the packet cap remain separate limits. The window is
the one the peer obeys: when unread bytes fill it the peer stops and nothing is lost. The cap is the one a
stream of short segments can reach first, when the application leaves them unread: a window of W bytes over S
packet slots cannot overflow the cap while the segments average W / S bytes or more, which is half a full
segment with the defaults. Below that, a segment that arrives at the cap is dropped and recovered by the
peer's retransmission, as the TCP page states today; the offered window is never taken back. `receive_window`
and `receive_segments` are the controls for a feed of small segments: more slots under the same window.

Where a backlog sits depends on who has stopped. When one thread runs the loop and the application is busy,
the loop does not run, so the backlog waits in the device's receive ring and the connection holds nothing new;
the peer, unacknowledged, can send no more than the window it was last offered. The slots fill when the stack
runs and the application does not read: an application that leaves data unread across ticks, or a stack on a
core of its own beside a stalled reader.

## The peer's window

The window field of every segment on a synchronized connection is shifted left by the peer's scale before
anything reads it: the window update, the duplicate-ACK test, zero-window probing and `writable()`. The field
of a SYN or SYN-ACK is taken as it is. A peer's shift above 14 is used as 14, as RFC 7323 requires. A peer that
did not offer the option has a shift of zero, as today. Nothing else on the send side changes: `SND.WND` was
already wide enough, and a window past 65535 simply lets `writable()` and the congestion window decide.

## The handshake's options

A SYN and a SYN-ACK carry, in Linux's layout (`tcp_options_write`), so a capture and a script read as a Linux
one does:

| Timestamps | Options, in order                               | Bytes |
|------------|-------------------------------------------------|-------|
| in use     | `mss`, `sackOK`, `TS val ecr`, `nop`, `wscale`  | 20    |
| not in use | `mss`, `nop`, `nop`, `sackOK`, `nop`, `wscale`  | 12    |

`sackOK` and `wscale` are each present when the configuration turns them on and, on a SYN-ACK, the peer's SYN
carried them; an absent `sackOK` beside the timestamps leaves `nop, nop` in its place, which is the block every
other segment carries. Every option is offered again on a retransmitted SYN. An offer the peer did not answer
is off for the connection, and an active open that ends before an answer negotiated nothing, as the timestamps
already do (`Timestamps::withdraw`).

`wire::TcpOptions` gains the window scale (kind 3, length 3) and SACK-permitted (kind 4, length 2), read by the
brick from a SYN or SYN-ACK only, as the MSS is, and refused as malformed on a wrong length, as the MSS and the
timestamps are. It gains writers for both and for a SACK option of one to four blocks, and a block type of two
sequence numbers. Kind 5 on receive stays an option skipped by its length.

## Acknowledgement timing

With `delayed_ack` zero, the default, nothing changes for in-order data: its ACK waits for the `flush` of the
same tick, a data segment that leaves first carries it, and every second full-sized segment is acknowledged at
once. A venue that left Nagle on would stall behind a delayed ACK, and a client that answers ticks within the
tick gains nothing from a delay, which is why the default is off.

With `delayed_ack` set, an ACK for in-order data waits for the first of:

- a segment that leaves carrying an ACK;
- unacknowledged in-order data reaching two full-sized segments' worth, counted in bytes, so a stream of small
  segments is acknowledged as often as a stream of full ones. Linux acknowledges once more than one of its
  estimate of the peer's segment size is unacknowledged (`__tcp_ack_snd_check`), which follows small segments
  more closely still; counting bytes against `mss()` is the fixed rule nearest to it;
- the delay, counted from the first segment the pending ACK covers and not extended by later ones, so no ACK
  waits longer than `delayed_ack`.

Only the ACK of in-order data is delayed. The ACK that completes an active open and the ACK of a peer's FIN in
Established stay where they are today, at the `flush` of the same tick, where a commit or a close may carry
them.

In both modes an ACK goes at once, before the next segment is processed, for: the first three segments above a
gap since RCV.NXT last moved, and every segment above a gap on a connection that does not use SACK; in-order
data while anything is held above it, and until RCV.NXT passes the end
of an out-of-order segment that was refused for room; a duplicate; a segment out of window; a segment PAWS
rejected; data after the peer's FIN; and a FIN the connection is closing on. A window update that `consume`
owes goes at the next `flush`.

**A burst above a gap.** On a connection that uses SACK the first three segments above a gap since RCV.NXT
last moved are answered at once, each with its blocks: three duplicates start the peer's fast retransmit, and
the blocks carry everything else. From the fourth on, until RCV.NXT moves, the answer is owed to the `flush`
of the same tick: one pure ACK per connection with the blocks as they then stand. A burst of thirty segments
behind one loss costs four ACKs, not thirty, and the peer still learns everything within the tick. Linux sends
three and batches the rest on a timer of at most a millisecond (`__tcp_ack_snd_check`), VPP sends at most
three per loop iteration; the `flush` is the boundary a polling stack already has.

On a connection without SACK every segment above a gap is answered at once, as today. A sender without SACK
reads the duplicates themselves: the third starts fast retransmit and each one after it grows the window by a
segment (RFC 5681 3.2), which is what lets it send new data while it recovers; and the ACK owed to the `flush`
may move the edge, so such a sender would not count it as a duplicate at all. Linux compresses on SACK
connections only (`__tcp_ack_snd_check` sends at once when `!tcp_is_sack(tp)`).

One case stays short until step 5. Two Aloe stacks negotiate SACK, so the receiver shares its later ACKs,
while Aloe's own sender does not read blocks yet and counts duplicates. Behind a loss it sees three duplicates
and then one ACK per tick: it starts fast retransmit on time and sends no new data until the retransmission is
acknowledged. Step 5's sender reads the blocks, and the two-stack tests of this step expect no more than that.

**A duplicate ACK stays a duplicate.** The ACK sent at once for a segment above a gap, a duplicate or any
refused segment restates the edge already offered, whatever `consume` has freed since. RFC 5681 counts an ACK
as a duplicate only when its window equals the last one's, and a sender without SACK, Aloe's own until step 5,
counts by that rule. Onload freezes the edge the same way. The ACK owed to the `flush` is an ordinary one and
may move the edge.

The delay runs on a second wheel timer in the control block, armed only while a delayed ACK is pending and
cancelled by any segment that leaves carrying an ACK. When it fires the pure ACK goes at once; a refused send
leaves the connection on the ACK list for `flush`, as a refused immediate ACK does today.

## Configuration, counters and what the application sees

`TcpConfig` gains four fields and one default changes:

| Field              | Default  | Meaning                                                                                                    | Rejected when                                           |
|--------------------|----------|------------------------------------------------------------------------------------------------------------|---------------------------------------------------------|
| `receive_segments` | 512      | Packets one connection may hold, in order and out of order together; with `mss()`, the default window.     | zero, or above `receive_pool`                           |
| `receive_window`   | 0        | The byte window when set, capped at what the slots hold; zero means half of what they hold.                | above 65535 shifted by 14                               |
| `sack`             | true     | Offer and use SACK-permitted: report what is held, and what arrived twice.                                 | never                                                   |
| `window_scaling`   | true     | Offer and use RFC 7323 window scaling.                                                                     | never                                                   |
| `delayed_ack`      | 0 (off)  | How long an ACK for in-order data may wait; zero sends it at the `flush` of the same tick.                 | negative, above 500 ms, or set below the wheel's resolution |

With the defaults a connection offers about 370 KB over 512 slots. `receive_pool` stays 2048, which four
connections stalled at the default cap can hold between them, and the cap is per connection, so one stalled
connection cannot take another's nodes; the TCP page's sizing note for the device's packet pool stands and
gains both sentences. A window the configuration
derives beyond what the largest shift can state is capped there.

Counters, by meaning; the names are the builder's:

- segments held above a gap;
- held out-of-order segments released for room;
- ACKs sent carrying SACK blocks, and duplicate SACKs sent;
- ACKs the delay timer sent.

`dropped_out_of_order` stays and narrows: an out-of-order segment that found no room. `dropped_duplicate`
widens to a duplicate of held out-of-order data. `window_updates` keeps its meaning, a segment that extended
the offered edge.

The application sees no new call and no new event. `unread()` shows in-order bytes only; `Readable` is raised
when in-order bytes arrive, whether one segment brought them or a filled gap brought many. For tests and
monitoring the connection reports the out-of-order segments it holds, the window on offer, both scales and
whether SACK is in use; the stack's `byte_budget()` becomes the largest budget the configuration allows.

## Parts, and where the state lives

By the rule that a class owns one concern's state, the receive side becomes these parts of the control block.
The names are provisional and the author rules on them.

- **`ReceiveQueue`** keeps RCV.NXT, the in-order chain, the unread count and the end of the peer's sequence
  space. It gives up the offered edge and gains the append of a joined run.
- **`ReassemblyQueue`** (new) owns the out-of-order chain and the remembered FIN: insert with the three rules,
  the run that follows RCV.NXT, the highest node for room, release.
- **`SackBlocks`** (new) owns whether SACK is in use, the four blocks in order and the pending duplicate: what
  an arrival, a move of RCV.NXT and a release do to them, and what the next ACK carries.
- **`ReceiveWindow`** (new) owns the budget, our scale and the offered edge: the offer with its three
  conditions and its rounding, the record of what left, and whether a consume owes an update. It takes over
  `Transmitter::offer_of`, which the split left on the transmit side.
- **`AckPolicy`** owns every decision of when an acknowledgement goes: at once, at the `flush`, or after the
  delay. It grows the delay (the pending ACK and the bytes it covers), keeps the refused out-of-order end it
  already tracks, and counts the segments above a gap answered at once since RCV.NXT last moved. The Receiver
  asks it and acts on the answer; what the ACK carries is `SackBlocks`' and `ReceiveWindow`'s, and the sending
  is the Transmitter's.
- **`SendSequence`** gains the peer's scale and applies it wherever it reads a window field.
- **`ControlBlock`** gains the three new parts and the ACK timer. The parts an in-order segment touches stay
  together at the front of the receive state; the blocks and the ACK timer sit behind them.

The procedures stay where the split put them. The Receiver's sequence check returns three outcomes and its
payload step gains the reassembly and join paths. The Transmitter's one send function takes the blocks a pure
ACK carries, writes the scaled window, and arms and cancels the ACK timer, since arming is its job.
`send_reset` builds its segment apart from that function and writes the window field itself; it takes the same
scaled field. The Lifecycle carries the negotiation's offers at `open`, takes the remembered FIN through the same state changes
as an in-order one, and returns the out-of-order chain at a terminal event. The `SegmentWriter` writes the new
options. Nothing changes direction.

## The tick path

For an in-order segment on a connection that holds nothing out of order, with the defaults:

- on receive: one shift of the peer's window field; one branch after the packet is held, never taken; one
  compare in the offer when the ACK or the answering order leaves;
- on send: two shifts to write the window scaled and record the edge the peer reconstructs;
- no timer armed or cancelled, no SACK block built or read, no new option parsed outside the handshake, no
  copy.

`consume` does less than today: a small read no longer queues a window update.

The check is the tick-to-trade benchmark on the finished branch against `main` at the branch's base, both built
`release` on the same machine, before and after run alternately three times each and compared by medians, with
the `tcp` leg broken out. The bar is no change beyond run-to-run noise, which the stack-split comparison put at
10 to 20 ns, at p50 and p99 on the 64-byte tick at level 0, cross-core. A change beyond it is fixed, or merged
with its number and its reason on the benchmark page, or answered by the policy under "Consequences". The
first fix to reach for is the one TLDK and Onload have: a segment whose option block is exactly the usual
`nop, nop, TS` is recognised with one 32-bit compare and its two values read directly, in place of the walk
over the options that every tick takes today.

## Fixtures and the scenario runner

- **`frames::TcpPeer`** learns to offer window scaling and SACK-permitted on its SYN and SYN-ACK and to state
  its window scaled, and the frames fixture gains a reader for the SACK blocks of a parsed frame, since the
  brick's own parser does not decode them until step 5.
- **The scenario runner** gains the configuration lines `--tcp_receive_segments`, `--tcp_receive_window`,
  `--tcp_sack=0|1`, `--tcp_window_scaling=0|1` and `--tcp_delayed_ack_ms`. Its parser, notation and matcher
  already carry the options.
- **Existing scripts change** for three reasons, each mechanical and each to be derived, not pasted from a
  failing run: the window numbers follow the new budget; a SYN the stack sends carries `sackOK` and `wscale`,
  and a SYN-ACK echoes what the script's SYN offers; a `read` that does not double the window no longer
  produces a window-update line. `data/above-a-gap-is-acked-not-stored.pkt` is replaced by its opposite.
- **The benchmark's venue** offers window scaling and SACK-permitted on its SYN, as a Linux venue would, and
  reads the stack's window scaled wherever it reads it.

## Testing

Unit tests of the new parts, each pinning behaviour that could regress:

- the reassembly queue: arrival in any order, the three insert rules, the append without a walk, the join with
  a node RCV.NXT passed in part and one it passed entirely, the highest node released for room, a remembered
  FIN;
- the SACK blocks: the order RFC 2018 asks for, merging, the fifth range dropping the oldest, three blocks with
  timestamps and four without, a duplicate first and then forgotten, blocks leaving as RCV.NXT passes them and
  cut back on a release;
- the receive window: the budget from `mss()`, from `receive_window` and from an unscaled peer; the shift;
  the SYN's unscaled field; rounding up and the recorded edge; an edge that never moves left over a long random
  run of arrivals and reads; the silly-window threshold; the doubling rule;
- the ACK policy with a delay: the deadline from the first segment, two full segments' worth, the cancel; and
  above a gap: three at once and then the `flush` with SACK, every one at once without it, the count starting
  again when RCV.NXT moves.

Each of the reassembly queue, the blocks and the window is also tested across the wrap of the sequence space,
where a comparison by raw value fails. The reassembly queue has a randomized test beside its cases: a stream
cut into segments at random, delivered in random order with duplicates and overlaps, read back byte-exact,
with the two chains disjoint and the slots within the cap after every step. The window's creep is pinned as
what it is: segments shorter than the unit, each acknowledged, leave the window open and the edge ahead of the
budget, and no test bounds how far.

Tests of the brick, in the existing files by topic: a held run delivered through `unread()` in order and
byte-exact; the cap and the empty pool with the lower sequence winning; a FIN above a gap; the ACK field of an
out-of-order segment taking effect; the negotiation matrix, each option offered or not by each end, active and
passive; a peer's scaled window opening `writable()` past 65535 bytes in flight; the delay timer; a terminal
event returning the out-of-order packets; a burst of segments above a gap in one `process` call answered by
three ACKs at once and one at the `flush` on a connection that uses SACK, and by one ACK each on a connection
that does not; a pure ACK above RCV.NXT answered by nothing; an ACK for an out-of-order segment restating the
edge after a `consume` that could have moved it.

Scenario scripts, Aloe's own, run in both checksum modes and under both policies unless a script names one:

- reassembly: a segment above a gap held and reported; the gap filled and the run delivered; an overlap
  trimmed; a covering segment replacing a held one; a FIN above a gap; the cap dropping the highest;
- SACK: negotiated on an active and a passive open; refused by the peer, with a plain duplicate ACK for every
  segment above the gap; the newest block first; three blocks with timestamps and four without; a duplicate
  below RCV.NXT, a duplicate prefix, a duplicate of held data and held data an in-order segment overlapped,
  each as a duplicate SACK; blocks leaving as the gap fills;
- window scaling: negotiated on an active and a passive open; the SYN's field unscaled; a peer that does not
  offer it leaving a 65535-byte window; a shift above 14 used as 14; the peer's scaled window letting a write
  pass 65535 bytes in flight;
- the offer: a small `read` sends nothing; a `read` that doubles the window sends one update; a closed window
  reopens once a full segment fits; the edge moves by full segments;
- delayed ACK, with `--tcp_delayed_ack_ms=40`: one segment acknowledged at the deadline; two full segments
  acknowledged at once; a `write` carrying the ACK; a segment above a gap acknowledged at once all the same.

Two Aloe stacks on the fabric: an echo in both directions under scripted reordering and under scripted loss,
every byte delivered in order, the receiver's held-out-of-order counter above zero, and no retransmission
timeout where three duplicate ACKs follow the loss.

Against Linux over the tap, as root, by hand: the handshake negotiates both options in both directions; a
kernel sender fills a window past 65535 bytes; under netem reordering the kernel's transfer completes with no
retransmission timeout on the kernel's side; under netem loss, where a lost retransmission or a lost tail can
cost the kernel a timeout whatever the receiver does, it completes with SACK recoveries counted and timeouts a
small share of them. Both are read from the kernel's counters. The run is owed to a machine with root, as the
earlier tap runs are.

The benchmark rerun, as under "The tick path".

## Documentation

- `docs/architecture/tcp.md`: the key types and the configuration and counter tables; the design notes on the
  two limits, on ACK timing and on what is still left out rewritten; new notes on the out-of-order hold, on
  where SACK blocks ride, on the scaled window and its rounding, and on the small-segment case with
  `receive_window`.
- `docs/roadmap.md`: step 4 landed, with what is owed to a root machine.
- `docs/architecture/scenario.md`: the new configuration lines, the new scripts in its table, and its paragraph
  on how Aloe's scripts differ from Linux's, which loses the SACK, window-scale and out-of-order sentences and
  keeps the delayed ACK's default.
- `docs/architecture/fixtures.md`: what `TcpPeer` offers.
- `docs/benchmarks/tick-to-trade.md`: the rerun, with its result file.
- `AGENTS.md`: the opening paragraph and the layout row for `tcp`, and the traps this step finds.

## Delivery

One branch, `tcp/receive-side`, from `main` at 6a779fb, and one pull request, as step 3 was. Every commit leaves
the four presets green. The order that keeps it so, and re-derives the existing scripts' windows once: the wire
options; the configuration and the runner's lines; the receive window with its budget, its offer and the
consume rule, still unscaled; the negotiation of both options and the scale; the reassembly queue; the receive
path that holds and joins; the SACK blocks and the duplicate SACK; the delayed ACK; the fixtures and the
scripts beside each part as it lands; the two-stack tests; the tap cases; the benchmark rerun; the
documentation. The pull
request closes #19 with a comment that states what landed and what is owed.

## Consequences for later steps

- **Step 5** reads the peer's SACK blocks through the option the wire module now writes, builds its scoreboard
  over the retransmission queue, and finds a receiver that already reports duplicates, which RACK's reordering
  window and the undo of a spurious retransmission both read. Its tail loss probe joins the send timer's kinds;
  the ACK timer of this step is the receive side's and stays apart.
- **Step 6** replaces the RST-inside-the-window rule on both paths with RFC 5961's challenge ACK, and may give
  the reassembly queue a tree if a server's cap makes the walk matter.
- **Closing the window at the cap** is held as a follow-up issue of its own, #39, by the author's decision. Linux
  answers an in-order segment it could not queue for memory with a zero window, the one case where its edge
  moves left (`ICSK_ACK_NOMEM` in `tcp_select_window`), so its peer pauses and a window update resumes it,
  where a drop leaves the peer to its backed-off timer. This step keeps the rule the issue and the TCP page
  state, an edge that is never taken back, and makes the case rare with the default margin.
- **Copying small segments together at the cap** is the question the same follow-up leaves open. The two
  stacks nearest to Aloe's purpose do it: TCPDirect moves payload into earlier buffers when its 64 slots run
  out, and Onload coalesces its receive queues under memory pressure, as Linux collapses. This step does not,
  by the rule that the data path takes no copy. The copy would run on the stack's own thread, which is running
  by definition when a packet finds no slot, and `unread()` is already invalidated by the next `process`, so a
  counted coalescing pass at the cap would break no contract. A measurement of how often a real feed reaches
  the cap decides it.
- **Hold or drop as a policy.** If the benchmark shows the hold on the tick path beyond the bar, the hold moves
  behind a compile-time policy of the brick, as congestion control is, with today's drop as the other model.
  That is the rule of 2026-10-27: a policy only where it picks an implementation and keeps the wire correct,
  and only once a measurement asks for it.

## Decisions made in the brainstorm

The author's:

- This step is the receive side, issue #19 ("I wanted out of order segments support").
- The window is configuration: a wrong value causes problems in that deployment and does not shape the
  structure ("Can we make it configurable?").
- The bar for the tick path is no change beyond run-to-run noise on the 64-byte tick, and the numbers come from
  tick-to-trade on the finished branch compared with `main`.
- The default leaves twice the slots the window needs in full-sized segments, so short segments and a second
  burst fit ("First one is reasonable").
- Closing the window at the cap is not in this step: it is filed as #39 and held, and whether to copy
  small segments together is decided after it ("I will hold it and maybe after we will figure this out,
  should we copy or not").
- Standing from earlier steps: the RFCs are the specification and Linux settles choices; no copy on the data
  path; the scripts are Aloe's own; one stack with a fast path and a slow path, and no stripped stack or
  policy until a measurement asks.

Claude's calls, stated in the brainstorm and not objected to:

- The whole of #19 in one design, one branch and one pull request.
- Nothing is copied under pressure; the highest out-of-order segment is released to make room.
- SACK blocks ride on pure ACKs only.
- Duplicate SACK is in this step.
- Delayed ACK is off by default.
- The default cap rises from 32 packets, to 512 once the window became half of what the slots hold, and the
  pool stays at 2048.
- No window auto-tuning.
- The peer's SACK blocks are skipped until step 5.

Settled while writing, Claude's calls, listed for the author to overrule:

- `receive_window`, an optional byte window below what the slots hold, for feeds of small segments.
- The budget per connection, counted in `mss()`, and `mss()` as the one meaning of a full-sized segment on the
  receive side.
- The smallest shift that states the window, offered even when it is zero.
- The window field rounded up to the scale's unit, with the reconstructed edge recorded.
- Linux's doubling rule for a window update sent by `consume`.
- Under a delay, two full segments' worth counted in bytes, and one fixed delay from the first segment covered.
- One switch, `sack`, for SACK and duplicate SACK.
- A held segment that a newcomer covers entirely is released for it; otherwise held bytes win.
- At the cap the lower sequence wins, one node for one segment.
- A FIN above a gap is remembered as a sequence number, reported in the blocks as Linux reports it, and
  forgotten when a node is released for room.
- One node released and one block cut back for room, where Linux frees an eighth of its buffer and forgets
  every block.
- One pending duplicate, forgotten at the next data segment as in Linux.
- The ACK field of an out-of-order segment is processed, and a RST on one resets, as RFC 793 has it.
- A data segment does not cancel an ACK that owes blocks.
- Out-of-order packets go back at a terminal event.
- The handshake's options in Linux's layout.
- A sorted chain with an append at the tail, and no tree.
- The names `ReassemblyQueue`, `SackBlocks` and `ReceiveWindow`.
- The benchmark's venue offers both new options.

Settled after the builder check, Claude's calls, listed for the author to overrule:

- Congestion-window growth on application-limited flows stays out of this step and goes with step 5; the
  closing comment names it as open.
- A SYN states the stack's largest budget, capped at 65535; the connection's budget first shows in the ACK
  after the handshake.
- Under a delay only data ACKs wait; the handshake's ACK and a FIN's ACK stay at the `flush`.
- A segment released for room is remembered by the ACK policy as a refused one is.

Settled after the check of other stacks, Claude's calls, listed for the author to overrule:

- The first three segments above a gap are acknowledged at once and later ones share one ACK at the `flush` of
  their tick, where the brainstorm said every one is acknowledged at once.
- An ACK sent at once for an out-of-order or refused segment restates the offered edge.
- Reading the usual timestamp layout with one compare is recorded as the first fix if the tick moves, not
  built otherwise.
- Copying small segments together at the cap stays out, with what TCPDirect and Onload do recorded beside it;
  the author has since held that question with the follow-up issue.

Settled in the review of 2026-11-12, found by Claude against the code and the Linux source and accepted by the
author ("You are right, add those to the spec"):

- Three ACKs at once and the rest at the `flush` holds only on a connection that uses SACK; without it every
  segment above a gap is answered at once, as Linux does and as a sender that counts duplicates needs. Two
  Aloe stacks stay short of that until step 5, and the spec says so.
- The rounded edge can creep right over many segments; the bound of one unit was wrong and is replaced by what
  happens and why it is harmless.
- A segment that occupies no sequence space above RCV.NXT is processed and not answered, as today.
- A fourth source of a duplicate SACK: held data an in-order segment overlapped, reported at the join.
- The budget is computed after the peer's MSS is stored and the state has left SynSent; the opens are
  reordered.
- `AckPolicy` owns every decision of when an ACK goes, the count of the first three included.
- `send_reset` writes the scaled field too.
- Tests across the sequence wrap, a randomized test of the reassembly queue, and a tap criterion under loss
  that a correct receiver can always meet.
- The peer's MSS has no floor until step 6, and the wider window widens a blind RST's target until step 6;
  both are recorded under the non-goals.

## Left to the builder

- The names and layouts of the new parts' members, the counter names, and the connection's new accessors.
- How the node carries its sequence, and whether the in-order chain's nodes write it.
- The three-way result of the sequence check and how the Receiver's payload step is divided.
- How the blocks are cut back on a release, and whether the four are kept as an array in report order or
  ordered when written.
- The ACK timer's trampoline and whether the Transmitter or the façade walks the ACK list, which the split left
  on the façade (`ack_front`, `ack_next`).
- The exact arithmetic of the offer, within the three conditions and the rounding stated above, and where the
  shift is computed.
- Which existing test files take which cases, the scripts' names and folders under `tests/scenarios/tcp/`, and
  which existing scripts pin `--tcp_receive_segments` instead of taking the new default.
- Fixtures whose packet pools are smaller than the new default cap: pin the cap or grow the pool.
- The tap cases' netem settings and which kernel counters prove no timeout.
- The wording of the closing comment on #19 and of the traps added to `AGENTS.md`.
