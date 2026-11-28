# Hardening and edge cases: keyed initial sequence numbers, RFC 5961, the leftovers of the handshake and of the window, and ARP that is static by default

Status: design, from the brainstorm of 2026-11-26 to 28, for the first of four pull requests under issue #21
(phase 2, step 6 of `docs/roadmap.md`). It closes #37 (predictable initial sequence numbers) and #23 (static ARP
entries). The issues' premises were checked against the code on `main` at 7dae733, and what Linux does was read from
its source at commit 4405c18c95d2d0f99f6c581ef0ee8591a1aa6ccd on 2026-11-28; where a premise failed, this document
says so. Private, like every file under `docs/superpowers/`; the public summary is the TCP page, the net page and the
roadmap once the work lands.

## Goal

The stack is about to meet the internet as a client to trading venues, and it is not yet built for anyone but a
friendly peer. Its initial sequence numbers come from a generator an observer can reconstruct. A RST anywhere in a
370 KB window ends an order session, and so does a FIN forged into it. A SYN on an established connection is dropped
without a word. Any ARP reply for the router's address rewrites the router's entry, and a handful of ARP frames
aimed at one window of the cache pushes a live entry out of it.

This pull request closes those, and the small cases the earlier reviews left open on the same code paths:

- initial sequence numbers and timestamp offsets from a keyed hash (RFC 6528);
- the first checks every segment meets, rewritten to RFC 5961: a RST resets only at an exact position, a SYN and an
  ACK of what was never sent or was sent long ago are answered with a challenge ACK, and those answers are limited;
- the handshake's leftovers: a RST for a bad ACK in SYN-RECEIVED, a floor under the peer's MSS, a counter for the
  payload of a SYN;
- the window's leftovers: the acknowledgement of a segment whose data the window refuses is read, a bare FIN is taken
  at a closed window, and resends into the peer's closed window do not end a connection the peer still answers;
- neighbours from a static table that never changes, with learning on the wire off unless the deployment switches
  it on, and a learned table in which no frame creates an entry and no address takes another's place.

What the application sees is configuration fields, counters, and one change of default: the IP brick resolves
nothing on the wire unless its configuration switches learning on. There is no new event, no new state and no new
sender; those are the next pull request's.

## The four pull requests of step 6

The author cut step 6 into four pull requests, in this order:

| PR | Contents |
|---|---|
| 1. Hardening and edge cases | This document. |
| 2. Life cycle | TIME_WAIT, what `release()` means during a close, the FIN-WAIT-2 timer, keepalive, the reset on a close with unread data, the event for a connection that leaves the fast path. |
| 3. ICMP and path MTU | A list of ICMP errors out of `net`, destination unreachable ending a connect, a shrunken MSS and the split of kept segments. |
| 4. Phase close | The comparison against Linux under netem, the soak run, the owed root runs, the documentation; then the author's release and branch. |

The author also decided what "v1" means: a release and a branch, with no promise about the API or the ABI, and a
`main` that may diverge. So no pull request of this step records a frozen surface, and this one is free to change
any signature it needs to.

## Goals, non-goals, anti-goals

Goals:

- An attacker who is not on the path cannot predict a connection's initial sequence number or its timestamp offset
  from any number of other connections it opens or observes.
- A forged RST ends a connection only when its sequence number is exactly one of at most three values. A forged SYN
  never ends one. A forged data or FIN segment is refused unless its acknowledgement number also falls in a bounded
  range.
- Every ACK this change adds, and the ACK that answers a refused segment which occupies no sequence space, is
  limited per connection to two a second. A RST is never limited, as in Linux: each answers one segment and is no
  larger than it.
- A neighbour a deployment names in its configuration is used without ever sending or reading an ARP frame for it,
  and nothing changes it. By default that table is all the brick knows. With learning switched on, no frame
  creates an entry, no address takes another's place, and every learned entry expires.
- The RFCs are the specification; where they leave a choice, Aloe does what Linux does at the commit named above,
  and each place where this design does something else is listed under "Where this departs".
- The tick path stays as it is. An in-order segment and an in-order ACK each gain one comparison that is false on
  that path, and the tick-to-trade benchmark on the finished branch shows no change against `main` at the branch
  point beyond run-to-run noise, 10 to 20 ns on the 64-byte tick at level 0, cross-core.

Non-goals, each with where it belongs:

- TIME_WAIT, keepalive, the FIN-WAIT-2 timer, new events: PR 2. ICMP and path MTU: PR 3.
- SYN cookies and a keyed flow-table hash: after phase 2, as the roadmap says. The hash this change adds to `core`
  is the one the flow table would use.
- A limit on challenge ACKs across connections. Linux has one per network namespace and ships it switched off
  (`tcp_challenge_ack_limit` is `INT_MAX`); the per-connection limit is the one in use, and the one built here.
- Accepting an empty segment whose sequence number lies between the last ACK sent and RCV.NXT, as Linux does by
  testing against `rcv_wup`. Such a segment, an ACK the network delivered after the data that followed it, is still
  refused as out of the window; its answer is now limited. Noted under "Consequences".
- Trimming a segment that starts inside the window and ends past it. Linux trims nothing either. It is refused
  whole, as today, after its acknowledgement was read.
- Linux's rule that an old pure ACK which fails PAWS draws no answer at all (`tcp_disordered_ack_check`): its answer
  is limited here like every other.
- Linux's `locktime`, which keeps a valid ARP entry's MAC for a second against a frame that names another. It
  stops two hosts flapping a learned entry; it does not stop a forger, who sends again a second later. The static
  table is the protection.
- Noticing that a static neighbour died or changed its MAC. Nothing probes a static entry; the conflict counter
  and the connections' own timeouts are what an operator sees.
- Bounding the forwards between shards under a flood of replies, with learning on over several queues: one
  allocation per sibling per reported frame, as today. A follow-up issue records it.

Anti-goals, which would make this a failure even if it built and passed:

- A connection reset by a segment whose sequence number is not one of the exact positions listed under "RST".
- An ACK this change adds that does not go through the limiter. Two kinds of answer stay unlimited on purpose: a
  RST, and the ACK for a refused segment that occupies sequence space, which exists today and which Linux leaves
  unlimited too.
- A value drawn from `std::mt19937` or `std::random_device` in any segment, or key material readable through any
  accessor, counter or log line.
- A static ARP entry changed, aged or evicted by anything but the destruction of the brick.
- An ARP request sent, or anything learned from a frame, by a brick whose configuration did not switch learning
  on.
- A clock read, an allocation after construction, or a log line in `core`'s new helpers on a hot path, in `net` or
  in `tcp`. The key is drawn at construction, which is setup.
- A switch that turns the hardening off. The limit's interval is the only setting that loosens anything.
- A regression of the benchmark's 64-byte tick that is merged without a number and a reason beside it.

## What the issues assumed, checked

Against `main` at 7dae733:

- **#37, the generator.** True as written: `tcp::detail::IsnGenerator` draws the initial sequence number and the
  timestamp offset from one `std::mt19937` seeded with 32 bits (`common/tcp/stack/detail/isn_generator.hpp`).
- **#37, "the timestamp offset comes from a keyed hash of the same 4-tuple under a second key, as Linux's
  `secure_tcp_ts_off` does".** Linux has no such function at this commit. The offset is the other 32 bits of the
  same SipHash call that gives the sequence number, over the same four-tuple, under the same key
  (`net/core/secure_seq.c:107-124`). One hash and one key are enough, and that is what is built.
- **#21, "a RST for a bad ACK in SYN-RECEIVED, which is dropped silently today".** True: `Receiver::receive_ack`
  counts `dropped_unexpected` and returns (`receiver.hpp:439-443`).
- **#21, "refusing a peer MSS option of zero".** Today the option's value is stored as it is
  (`segment.mss.value_or(default_peer_mss)`), so a stated zero makes `mss()` zero, `writable()` zero and the receive
  budget zero. Linux does not refuse: it ignores a zero and raises a small value to 48 bytes before the options come
  off (`tcp_input.c:4631-4640`, `tcp_output.c:2052-2081`). The roadmap says refuse, and this design refuses; see
  "The peer's MSS".
- **#21, "reading the acknowledgement and window of a data segment dropped for the window, which today are
  discarded with it".** True: the sequence check comes first and a refused segment goes no further
  (`receiver.hpp:316-319`). RFC 9293 3.10.7.4 asks for it ("special allowance should be made to accept valid ACKs,
  URGs, and RSTs"). Linux at this commit is stricter than it used to be: its `tcp_sequence` refuses a segment that
  ends past the largest edge it ever offered unless its receive queue is empty, and a refused segment's
  acknowledgement is not read (`tcp_input.c:4873-4900`). So here the roadmap's item follows the RFC against Linux.
- **#21, "a counter for SYN payload a listener discards".** True that none exists. Linux has none either; the
  payload is dropped and not acknowledged (`tcp_input.c:7597-7598`).
- **#21, "ARP-reply rate limiting in `net`, which matters once `accept_unsolicited_replies` is on".** Half the
  picture, in two ways. A request for our address from a sender we hold nothing for creates an entry whether or not
  that switch is on (`ipv4_arp.hpp:76-78`), so the door is open on every deployment. And a rate limit does not
  protect an entry in use: the cache places an address in a window of eight slots and, when the window is full,
  evicts the entry confirmed longest ago, so eight frames aimed at one window displace whatever was there at any
  rate above eight per interval; a sender's address is not even required to be on our subnet. See "Approaches" and
  "Neighbours".
- **Step 3's hand-off, "zero windows with data queued".** Three items. The first two still hold: resends into the
  peer's zero window count towards `data_retries` although the peer answers each one
  (`transmitter.hpp:1437-1442`), and a bare FIN that arrives while our own window is zero is dropped
  (`receiver.hpp:420`). The third is gone: a FIN no longer waits for a send-pool node, since `Established` sets one
  aside for it (`Lifecycle::establish`).
- **The ledger of step 5, "`passive_open` writes `send.wnd` directly, so `max_wnd` is 0 until the handshake ACK".**
  Still true (`receiver.hpp:237`) and harmless here: the ACK that completes the handshake raises `max_wnd` through
  `update_window` before any bound reads it, which is also when Linux sets its `max_window` on a passive open.
  Nothing is changed.
- **Found while checking: an empty segment exactly at the offered edge is refused** (`receiver.hpp:383`, RFC 793's
  strict test), where Linux accepts it. A peer that filled our window above a hole sends its ACKs and its RST from
  its SND.NXT, which is our edge, so today those ACKs are thrown away and answered. See "The first checks of a
  segment".
- **#23, static ARP.** True as written: `Ipv4::learn` seeds an entry that ages, can be evicted and can be rewritten
  by any frame, and the benchmark seeds its venue entry that way (`level_bricks.cpp:42`, `level_runtime.cpp:125`).

## What Linux does, checked

Read by an agent on 2026-11-28 from the source at 4405c18c95d2, every line cited below quoted in its report; the
four files kept from step 5 were compared byte for byte with fresh downloads. The files are in
`.superpowers/sdd/2026-11-28-hardening/reference/linux/`, each named by its kernel path with the slashes replaced,
beside the RFCs in `reference/rfc/`. The plan reads the cited lines again before it writes each part.

- **Initial sequence number.** SipHash-2-4 over the local address, the remote address and both ports, under one
  128-bit key drawn at random once per boot, the low 32 bits, plus the real-time clock in units of 64 ns
  (`secure_seq.c:45`, `107-124`). RFC 6528 says 4 microseconds; Linux's unit wraps every 275 seconds.
- **Timestamp offset.** The other 32 bits of that hash (`secure_seq.h:10-16`). Added to the clock on every segment
  sent (`tcp_output.c:1055`, `1231-1233`), taken off an echo received unless the echo is zero
  (`tcp_input.c:4746-4749`).
- **The sequence check.** `tcp_sequence` (`tcp_input.c:4873-4900`): refused when it ends before `rcv_wup`, or
  starts past the largest edge ever offered, or ends past it while the receive queue holds something. A FIN may
  stand one past the edge, and an empty segment exactly at the edge is accepted. A refused segment is dropped whole,
  its ACK field with it, and answered with a
  duplicate ACK through the limiter (`6449-6476`). A bare FIN at RCV.NXT is taken at a zero window
  (`5708-5710`). In-order data at a zero window, when it does pass the check, has its ACK processed and its data
  dropped (`5700-5714`).
- **RST.** `tcp_validate_incoming` (`tcp_input.c:6429-6518`) and `tcp_reset_check` (`6401-6408`). The connection is
  reset when the sequence number is RCV.NXT; or RCV.NXT - 1 in CLOSE_WAIT, LAST_ACK or CLOSING, in or out of the
  window, because macOS sends its RST with the FIN's number; or the end of the highest SACK block the receiver is
  reporting. Any other RST inside the window draws a challenge ACK; one outside is dropped in silence. A RST skips
  PAWS.
- **SYN.** On a synchronized connection a SYN draws a challenge ACK whatever its sequence number and whether or
  not it passed PAWS (`6432-6433`, `6461-6462`, `6526-6551`). In SYN-RECEIVED, which Linux keeps as a request
  (`tcp_check_req`, `net/ipv4/tcp_minisocks.c:724-900`): a SYN at the first one's sequence number resends the
  SYN-ACK through the limiter without spending a retry; a SYN at another number inside the window resets the
  half-open connection and is answered with a RST; a RST exactly at RCV.NXT removes it in silence, any other RST in
  the window draws a challenge ACK.
- **A bad ACK in SYN-RECEIVED.** `tcp_minisocks.c:836-839`: a segment with ACK set and RST clear whose
  acknowledgement number is not ISS + 1 is answered with a RST whose sequence number is that acknowledgement
  number, with no ACK bit (`tcp_ipv4.c:758-759`). The half-open connection stays. The check runs before the window
  check, and the RST is not limited.
- **ACK validation.** `tcp_ack` (`tcp_input.c:4333-4355`): an ACK below `SND.UNA - min(max_window, bytes_acked)`
  draws a challenge ACK and the whole segment is dropped, payload included; so does an ACK above SND.NXT. Between
  the bound and SND.UNA the ACK is old: its SACK blocks are read, its window is not, and its payload goes on to be
  queued. `max_window` is the largest window the peer ever offered, set on a passive open from the ACK that
  completes the handshake (`tcp_minisocks.c:614`); `bytes_acked` is the sum of SND.UNA's advances. On a passive
  open that sum starts at zero, so until something is acknowledged every ACK below SND.UNA is too old; on an
  active open the SYN-ACK's processing counts the SYN's own number (`tcp_input.c:7030`, `3936-3944`), so there an
  ACK of the ISS is old, not too old.
- **The limiter.** One stamp per socket, `last_oow_ack_time`, and one interval, `tcp_invalid_ratelimit`, 500 ms by
  default (`tcp_input.c:4017-4039`, `tcp_ipv4.c:3506`). A challenge ACK always goes through it (`4097-4113`). The
  duplicate ACK for a segment refused by the sequence check or by PAWS goes through it only when the segment
  occupies no sequence space or carries a SYN (`4048-4057`: `seq != end_seq && !syn` is exempt), so a resent FIN
  is always answered. The stamp is shared, so one answer of any kind holds back
  the others for the interval. A challenge ACK is an ordinary pure ACK: SND.NXT, RCV.NXT, the window, the
  timestamps and whatever SACK blocks are held.
- **The peer's MSS.** Never refused. A zero option is ignored, leaving the default of 536; a small one is raised
  to `tcp_min_snd_mss`, 48, before the options are subtracted (`tcp_output.c:2052-2081`, `tcp.h:72`), so a peer
  that states an MSS of one, with timestamps, is sent 36-byte segments.
- **Resends into a zero window.** `tcp_retransmit_timer` (`tcp_timer.c:563-602`): with the peer's window zero on a
  socket the application still holds, the head is resent and the backoff moves, but the retry count does not and
  `tcp_retries2` does not apply. The connection ends only when both the time since the last ACK it accepted and the
  time since the resends began pass twice `rto_max`, 240 seconds, or by the user timeout
  (`tcp_rtx_probe0_timed_out`, `495-523`). With nothing in flight the probe timer applies instead: `tcp_retries2`
  probes with no accepted ACK in between (`390-439`), which is the rule Aloe's probes follow today.
- **ARP.** A request for one of our addresses creates or updates the sender's entry at once
  (`net/ipv4/arp.c:843-865`). A reply nobody asked for creates nothing unless `arp_accept` is on, off by default
  (`893-922`). A valid entry's MAC is not replaced by another within `locktime`, one second, of its last change,
  except by a gratuitous ARP or a request for our address (`933-945`). A permanent entry is never changed by a
  frame, and nothing counts or logs the attempt (`net/core/neighbour.c:1391-1400`). Nothing limits ARP frames by
  rate; the table's size and a forced collection bound it (`neighbour.c:511-522`).

## Approaches

**For the pull request as a whole there is one sensible way.** The RFCs and Linux fix nearly every rule, and the
code already has the places they go: the `Receiver` makes the first decisions about a segment in RFC 9293's order,
the `Lifecycle` draws a connection's numbers, the `Transmitter` owns the retransmission timer, and `net::detail::Arp`
with `ArpCache` owns what a frame may teach. Two alternatives were considered and dropped:

- *A validator of its own in front of the `Receiver`.* A fifth part for a few dozen lines, every one of which reads
  the state the `Receiver` already has in hand (RCV.NXT, the edge, the peer's FIN, what is held above a gap).
- *A configuration switch for the hardening.* Its off position has no user, and it would double the scenario
  scripts.

**ARP had three candidates.**

1. *A rate limit on replies nobody asked for*, as the issue words it: a count per interval, a setting for it, a
   counter for what it refused. It bounds how fast the cache churns and protects nothing in particular: as "What
   the issues assumed" shows, eight frames evict a chosen entry at any rate the limit allows, and every eviction of
   a next hop costs a connection an ARP round trip and up to one ARP poll, 10 ms, on its next send.
2. *A rule about what a frame may displace* inside today's table, this design's first proposal: an entry that only
   a frame created takes a free or expired slot and is the first to go. It protects an entry a send has used and
   keeps everything else as it is.
3. *Two tables, and learning off by default*, the author's: a static table from the configuration that never
   changes; a learned table that exists only when the deployment switches it on, in which an entry changes only
   through a frame about its own address, nothing is evicted, and everything expires.

The third is built. It is simpler than the second, since a frame that cannot create an entry needs no rule about
what its entry may displace, and it makes the hardened state the default instead of a property to be argued for. A
variant was put to the author and ruled out: learning on by default with entries that never expire. In the
author's words it "is literally vulnerable to memory drain by adding new and new addresses", and a neighbour whose
MAC changed without a frame saying so would never be noticed.

**The peer's MSS** was the one place the roadmap and Linux part ways: refuse, or raise to a floor. Refusing was put
to the author with Linux's behaviour beside it, and was settled on 2026-11-28. Raising would
send a peer segments larger than it said it accepts, against RFC 9293's MUST-16, and would let a venue that states
a broken MSS go on as a connection of 36-byte segments that nobody notices; refusing fails the connect at once with
a counter that names the cause.

## Constraints

- `core` has no dependencies and stays that way: the hash is written here, about forty lines, and checked against
  the reference implementation's published vectors. The key comes from `getrandom(2)` through glibc, which is not a
  library dependency.
- Nothing in `net` or `tcp` reads a clock. Time is the stamp of the last `process`, `flush` or `connect`, and the
  initial sequence number's clock term is formed from it.
- `tcp` and `net` link neither `execution` nor `log`. Every new path is `noexcept` and allocates nothing after
  construction but packets; construction may throw `std::invalid_argument` for a bad configuration and
  `std::system_error` when the kernel gives no random bytes.
- Each commit builds and passes on the presets the delivery section names, and the scenario scripts pass under both
  congestion policies and both checksum modes.
- Every place where the brick does something other than the RFC's text or Linux is written into the TCP page's note
  "Where the brick departs from the RFCs and from Linux", or the net page's design notes for ARP, in the same
  change, with a comment at the code. The author asked for that in step 5 and it holds for every later step.
- The layout of `TcpConfig`, `TcpCounters`, `ControlBlock`, `Ipv4Config` and `Ipv4Counters` changes, so the
  `gcc-debug` run is taken from a clean build (the trap in `AGENTS.md`).

## Initial sequence numbers and timestamp offsets

**The hash.** `core` gains SipHash-2-4: a 128-bit key and a function from bytes to 64 bits, `constexpr`, in a
header, with no state. It is checked against the reference vectors, of which the plan's probe computed four with
the key `00 01 .. 0f` and the input `00 01 .. n-1`: `0x726fdb47dd0e0e31` for no input, `0xa129ca6149be45e5` for 15
bytes (the example in the SipHash paper), `0x3f2acc7f57c29bdb` for 16 and `0x958a324ceb064572` for 63
(`reference/siphash_probe.py`). Beside it, in `core`'s compiled part, one function fills a span with bytes from
`getrandom(2)`, retries on `EINTR` and on a short read, and throws `std::system_error` on any other failure.

**The numbers.** `IsnGenerator` holds one key, takes it in its constructor, and answers one question: the numbers
of a flow at a stamp. It hashes the flow, local address, remote address, local port, remote port, in one call:

- the initial sequence number is the low 32 bits of the hash plus the stamp's nanoseconds since the clock's epoch
  shifted right by six, modulo 2^32: Linux's 64 ns unit;
- the timestamp offset is the high 32 bits of the hash.

So one flow opened again later starts further on by the time that passed, two flows that differ in any field start
at unrelated numbers, and one flow's timestamps keep one offset for as long as the process lives, which keeps them
monotonic across its connections. `force(isn)` stays for the tests and replaces the sequence number only.

**The key** is drawn once per `Stack`, at construction, so per shard. A flow always lands on the same shard while
the card's steering does not change, so per shard is per flow. Nothing reads the key back: no accessor, no counter.
A test of the generator constructs it with a key of its own choosing; the stack has no way to be given one.

**Where it is used.** `Lifecycle::open` already has the block's two endpoints bound when it numbers the SYN or the
SYN-ACK; it passes them and the context's stamp. `Timestamps::offer` takes the offset as today. Nothing else
changes: the scenario scripts read the stack's initial sequence number and its first timestamp from the wire.

## The first checks of a segment

`Receiver::receive_synchronized` handles every state past `SynSent`. Its order becomes the one below, Linux's
`tcp_validate_incoming` followed by `tcp_ack`. `SynSent` keeps its own procedure, which already accepts a RST only
with an acceptable ACK, as RFC 5961 leaves it. A connection that is `Closed` and not yet released answers with a
RST as today.

0. **In `SynReceived` only: an ACK that does not acknowledge the SYN-ACK.** See "The handshake's leftovers".
1. **PAWS**, as today, with two changes: a SYN that fails it is challenged (step 4's answer), and the ACK that
   answers a rejected segment goes through the limiter when the limiter's test below says so.
2. **The sequence check**, with four outcomes where it has three today. One rule changes for every outcome: a
   FIN's own number is not held against the offered edge, as in Linux; it takes no slot.
   - *Refused.* Entirely before RCV.NXT, or starting past the offered edge, or above a gap with payload that ends
     past the edge. A RST here is dropped in silence, except at RCV.NXT - 1 after the peer's FIN (step 3). A SYN is
     challenged, except a duplicate SYN in `SynReceived`. Anything else is counted as today and answered with an ACK
     that restates the edge, through the limiter when its test says so.
   - *In order.* As today, and three cases that are refused today: an empty segment exactly at the offered edge,
     which is where a peer that filled the window above a hole sends its ACKs and its RST from; a bare FIN at
     RCV.NXT while the window is zero; and a segment whose payload ends at the edge and whose FIN stands one past
     it.
   - *Above a gap.* As today, with the FIN's number left out of the edge test here too.
   - *No room*, new. The segment's first new byte is at RCV.NXT, after the old prefix is trimmed as today, and its
     payload ends past the offered edge: the window is zero, or the segment is too long for what is left of it. It
     goes on through steps 3, 4 and 5 like an in-order segment, and at step 6 its payload is refused.
3. **RST.** The connection is reset, as `Lifecycle::peer_reset` does today, when the sequence number is
   - RCV.NXT; or
   - RCV.NXT - 1 in `CloseWait`, `Closing` or `LastAck`, whatever the sequence check said; or
   - the end of the highest range the connection reports as held above a gap, a remembered FIN's number included,
     on a connection that uses SACK. A peer whose data was lost resets from its own SND.NXT, which is that end.

   Any other RST that passed the sequence check is dropped, counted, and answered with a challenge ACK. `SynReceived`
   follows the same rule; a reset there frees the half-open slot in silence, as today.
4. **SYN.** On a connection past `SynReceived`: dropped, counted, challenged. In `SynReceived`: a SYN with no ACK
   at the first SYN's sequence number resends the SYN-ACK from its entry, as today, now through the limiter; any
   other SYN that passed the sequence check ends the half-open connection, which is freed and counted in
   `handshakes_failed`, and is answered with RFC 793's RST for a segment with no connection
   (`Transmitter::reply_reset`), as Linux's request does.
5. **ACK.** A segment with no ACK flag is dropped as today. Then, with SND.UNA as the segment finds it:
   - above SND.NXT: dropped whole and challenged. Today it is answered with an unlimited ACK.
   - below `SND.UNA - min(max_wnd, acknowledged)`: dropped whole, payload and FIN included, counted, and challenged.
     `max_wnd` is the largest window the peer ever offered, as `SendSequence` keeps it today; `acknowledged` is a
     new 64-bit sum of SND.UNA's advances, set to zero when the connection becomes `Established`, so the SYN's own
     number is not in it. Until the peer has acknowledged something, every ACK below SND.UNA is too old.
   - anything else goes to `AckProcessor::on_ack` as today, which already reads the blocks of an old ACK and leaves
     its window alone. Its own test for blocks on an ACK older than the largest window becomes unreachable and goes.
6. **Payload.** A *no room* segment is refused here: counted in `dropped_out_of_window` and answered at once with
   an ACK that restates the edge, never limited, since it carries data. A FIN behind refused data is not taken, as
   today. Everything else as today.
7. **FIN**, as today.

**The challenge ACK** is the connection's pure ACK with the edge restated (`send_pure_ack(block, Edge::Restate)`):
SND.NXT, RCV.NXT, the window, the timestamps and the SACK blocks it holds. A real peer that restarted answers it
with a RST whose sequence number is our acknowledgement number, which is RCV.NXT, and that RST resets the
connection at step 3. A forger off the path never sees it.

**The limiter** is one stamp per connection and one interval, `challenge_ack_interval`, 500 ms by default. An answer
that goes through it is sent when no answer went through it within the interval, and stamps it; otherwise it is not
sent and `acks_rate_limited` counts it. These go through it:

- every challenge ACK, whether or not the segment that drew it carried data;
- the ACK that answers a segment refused by the sequence check or by PAWS, when that segment occupies no sequence
  space or carries a SYN, which is Linux's test;
- the SYN-ACK resent for a duplicate SYN in `SynReceived`.

These never do: the ACK that answers a refused segment with payload or a FIN (a duplicate, data past the edge, data
with no slot or no node, a FIN sent again because our ACK of it was lost), an ordinary ACK, a window update, and
any RST. The stamp is shared, as Linux's is, so a challenge holds back the answer to an empty out-of-window segment
and the other way round. A setting of zero sends every answer.

Two things follow that a reader of the scripts should expect. A Linux peer's zero-window probes and keepalives are
empty segments at RCV.NXT - 1, so they are answered at most twice a second; Linux answers its own the same way.
And Aloe's own probes back off from one RTO, so against an Aloe peer with the defaults the second probe, 400 ms
after the first, goes unanswered and every later one is answered; the count of unanswered probes never passes one.

## The handshake's leftovers

**A bad ACK in `SynReceived`** (step 0). A segment with ACK set and RST clear whose acknowledgement number is not
SND.NXT is answered with `<SEQ=SEG.ACK><CTL=RST>` (`reply_reset`) and dropped, before PAWS and the sequence check,
as Linux checks it before its window. The half-open connection stays and its SYN-ACK timer runs on. The RST is not
limited, as none is.

**The peer's MSS.** A new setting, `min_peer_mss`, 48 by default, Linux's `tcp_min_snd_mss`. The peer's MSS is the
value of its option, or 536 when its SYN or SYN-ACK carries none. When that is below the floor, a stated zero
included, the handshake is refused:

- on a passive open the SYN takes no slot and is answered with RFC 793's RST for a segment with no connection;
- on an active open the SYN-ACK is answered with `<SEQ=SEG.ACK><CTL=RST>`, the connection is closed and `Reset` is
  raised, so a hand-written loop and the runtime's `connect`, which reports `Refused`, see an open that was refused.
  It is not counted in `connections_reset`, which counts RSTs received.

Both count in one new counter. The constructor refuses a floor below 48 or above the stack's own MSS. With the
floor at its default, the smallest `mss()` a connection can have is 36 bytes with timestamps and 48 without, so the
zero budget and zero `writable()` of today cannot happen.

**The payload of a SYN** on a passive open is not held and not acknowledged, as today and as in Linux without Fast
Open; the peer sends it again after the handshake. A new counter counts such SYNs.

## The window's leftovers

**The acknowledgement of a segment the window refuses** is read: that is the *no room* outcome above. The case it
serves is a peer that sends into our closed window, or one segment too many, while it acknowledges what we sent.
Today its ACKs are thrown away with its data, so our retransmission timer fires for segments the peer has. The
segment starts exactly at RCV.NXT and its acknowledgement number passed step 5's bounds, which is as much proof as
any accepted segment gives.

**A bare FIN at a closed window** is taken (the *in order* outcome above). A peer that closes while our reader is
stalled is told so, and the application learns it from `PeerClosed` once it reads. Linux does the same because
some stacks resend a bare FIN forever otherwise.

**Resends into the peer's closed window.** RFC 9293's MUST-37 (RFC 1122 4.2.2.17) says a connection stays open
while the peer keeps answering with a zero window. Today an expiry of the retransmission timer counts towards
`data_retries` whatever the peer's window is, so a peer that holds a hole and a full buffer for fifteen expiries
loses the connection although it answered every resend. The rule becomes:

- An expiry of the retransmission timer on a synchronized connection that finds the peer's window zero does what
  every expiry does: the loss state, the resend of the head, the backoff. It is counted apart: not in the count
  `data_retries` bounds, but against the count of unanswered zero-window probes, which every acceptable ACK clears
  (`ZeroWindowProbe::answered`, called from `AckProcessor::update_probing` on every ACK at or above SND.UNA).
- So the connection ends as `TimedOut` after `data_retries` such expiries in a row with no acceptable ACK between
  them, or by the user timeout, checked first as it is today.
- An expiry counted this way must not bring the connection nearer to `TimedOut` once the window opens: the count
  `data_retries` bounds stands where it stood.

This is the rule Aloe's zero-window probes already follow with nothing in flight, which is Linux's
`tcp_probe_timer`; Linux's own comment on the branch in question is "our retransmits become zero probes". Linux
bounds that branch by time instead, 240 seconds since the last accepted ACK and since the resends began; the count
is used here so that one rule covers a closed window with and without data in flight.

## Neighbours: a static table, and learning that is off by default

The author's design, of 2026-11-28. The IP brick knows its neighbours from two places.

**The static table** is `Ipv4Config::static_arp`, a list of address and MAC pairs (`ArpResolution`, the type
`resolved()` already reports). It is built once, in the constructor, and is frozen for the life of the brick: no
frame, no call and no timer adds to it, changes it or removes from it. The constructor refuses, with
`std::invalid_argument`: an address that is not a unicast host on the subnet, or is ours; a MAC that is zero or a
group address; an address listed twice. It is searched first, before anything learned. It is an array sized from
the configuration: a handful of addresses are compared in a row faster than any table is hashed, and the author and
this design agree a perfect hash would buy nothing, the more so since the order path makes no lookup at all: each
connection keeps its link header built, and with learning off nothing ever makes it build again.

**Learning** is what today's cache does: asking on the wire, keeping the answer, aging it. It is **off by default**,
because the first deployment is a trading system that knows its next hop. A deployment switches it on in the
configuration, and a learned entry always expires: no value of the timeouts keeps one for ever. The author ruled
out learning without expiry, since a table that only gains addresses can be filled by whoever supplies them.

With learning off, the default:

- No ARP request is ever sent, and no frame teaches anything.
- A request for our own address is answered as today, to the frame's sender MAC: the router needs ours.
- The route question, `Ipv4::next_hop`, answers "nowhere" for a destination whose next hop has no static entry. So
  `send` and `prebuild` fail with `NoRoute` at once, and `tcp::Stack::connect` returns `NoRoute` with no slot taken,
  as it does today for a peer off the subnet with no gateway. Nothing polls and nothing waits: TCP's ARP poll is
  armed by `Unresolved` alone, which this mode never returns, and since the static table cannot change, a
  connection that opened never meets `NoRoute` afterwards. The question is answered where the route and the
  neighbours are both in reach, the façade's `next_hop` and `Output`'s resolution; `Routing` stays a part with
  no ARP state.
- The constructor refuses a configuration that names a gateway and has no static entry for it: a start that fails
  instead of every connect failing.
- `learn`, the program's seed, stores nothing and answers `Off`, so a program that relied on a seed sees it at the
  call and not as a `NoRoute` later (from the whole-branch review); `resolve` answers from the static table alone.
- `tcp::Stack` refuses, at construction, an IP brick that can resolve no neighbour, one with learning off and no
  static entry: such a stack could never send a segment. The IP brick alone is still built, since an echo
  responder needs no neighbour. (From the whole-branch review, which left it to the author.)

In both modes, a SYN whose source has no next hop is dropped and counted by `passive_open` without taking a slot.
Today such a SYN, one from off the subnet with no gateway configured, takes a slot whose SYN-ACK can never leave
and holds it for the whole retry schedule; with learning off every SYN from a neighbour outside the table would.
The same question is asked before a reset is sent to a segment's source (`Transmitter::reply_reset`): with no
next hop nothing is sent and nothing more is counted, the segment having been counted under its own drop reason.
Without that, every stray segment from a neighbour outside the table would count a refused send, which the
benchmark's sanity check reads as a failure.

For a static address, in both modes:

- A lookup answers with its MAC and asks for no request, at any stamp. `prebuild` gives a template whose
  `valid_until` is the end of time, as for a broadcast destination, and the static table never moves the
  generation. With learning off nothing else moves it either, so a template is built once per connection. With
  learning on the brick still has one generation, so a change to any learned neighbour makes every template
  build again on its next send, a static hop's too, as today. A generation per hop would be a change to TCP's
  template and to what it asks of the IP brick, and is not made.
- An ARP frame whose sender address is static and whose sender MAC is the configured one teaches nothing and is
  reported nowhere. One whose sender MAC is another is counted as a conflict, and nothing else about it changes. In
  both cases a request for our own address is still answered.

**With learning on**, the learned table follows three rules. The first two are the author's.

1. **An entry changes only through a frame about its own address.** A reply, or a request, whose sender is an
   address the table holds fills that entry in or gives it another MAC, as today. It extends the entry's life only
   when it answers a request the brick itself has outstanding for that address: a frame nobody asked for never
   moves the confirmation stamp, so an entry the brick does not use expires whatever frames say (found by the
   security check of the build; Linux's rule too, which leaves such an entry stale). A frame about an
   address the table does not hold changes nothing and creates no entry. Only the brick's own lookup, which a send,
   a `prebuild` or a `resolve` makes, and the program's `learn` create one. The cost: the first answer to a host on
   the subnet that asked for our address takes an ARP exchange of our own, where today its request had already
   taught us its MAC.
2. **No address ever takes another's place.** The windows of eight and their eviction order go. The table is an
   ordinary fixed table of `arp_capacity` entries in which any free slot serves any address, and a resolved entry
   leaves it only by expiring. When it is full, a new address is refused: the lookup fails as unresolved and a
   counter says so. Nothing is evicted.
3. **A slot that never resolved is given up.** An entry that was asked about and never answered is not a mapping.
   Once its last request is an `arp_request_interval` old it may be taken by another address, so sends to addresses
   that do not exist cannot hold the table for the two minutes an expiry takes.

Aging stays as it is: an entry is used without a refresh for `arp_reachable`, refreshed by a unicast request while
it is in use, unresolved again after `arp_expire`, and aged against the stamp the caller passes, with no timer. The
generation moves as today, when a learned entry's MAC changes or goes.

What learning gives and what it does not, which the net page says in these words. The table is of a fixed size and
only the brick's own sends fill it, so no frame makes it grow or pushes a neighbour out. But a forged frame about
an address the table holds is learned, as on any host that uses ARP; and whoever can make the brick send to
addresses on the subnet that do not exist (a SYN from a forged source to a listener, any segment to a closed port)
can keep its free slots busy with unresolved entries, so that a new neighbour waits. Resolved entries are not
touched either way. A deployment that cannot accept that runs with the default.

**Several queues.** On a multi-queue device every shard shares one address and ARP frames land on queue 0, so the
reply to a request shard 2 sent arrives on a shard that never asked. Today that shard stores it
(`accept_unsolicited_replies`), the loop forwards it, and every sibling stores it. Under rule 1:

- `resolved()` reports what it reports today: every mapping a frame gave an entry this shard holds, a refresh
  with the same MAC included. That is what carries a reply to the sibling that asked when queue 0 holds the
  address too, as it does for a gateway every shard uses;
- and, when that switch is on, it also reports a reply addressed to us about an address this shard does not
  hold, which is not stored;
- the loop forwards what `resolved()` reports, as today;
- a sibling hands a forwarded resolution to a new call of the IP brick, which fills or updates an entry it holds
  and otherwise does nothing. `learn` stays the program's seed, which creates.

So a reply is used only by a shard that asked. `runtime::TcpStack` sets the switch on a multi-queue device as it
does today, and its ARP sink makes the new call. With learning off nothing is reported and nothing is forwarded.
One thing is as it was and is noted on the runtime page: a forward costs one allocation per sibling per reported
frame, so a flood of replies is a flood of forwards.

**Who changes, mechanically.** Every place that builds an IP brick and relies on resolution or on a `learn` seed
switches learning on in its configuration and keeps its seeds, as the author chose: the net fixture's
`stack_config()`, which the TCP fixture uses, the scenario harness, and the tests that build a brick of their own,
the two-stack tests, placement, the runtime's and the ring integration tests among them. The plan lists them
from a search for every `Ipv4Config`, about twenty-five files. The benchmark's venue entry becomes static at
both levels, as #23 asks. `--arp-on-wire` switches learning on and keeps the benchmark's lifetimes of 24 and 48
hours, which are there so that no refresh falls inside a measured run. The examples take static entries and a
switch for learning on their command lines, which means a new argument parser for `tcp_echo`, whose present one
finds the gateway by position; the documented tap command lines switch learning on, since a tap interface's MAC
is new on every run.

**The tap suite under rule 1.** The kernel's own ARP request used to teach the stack the kernel's MAC before its
SYN arrived. Now the first SYN-ACK to a kernel that connects waits for the stack's ARP exchange and one ARP poll,
10 ms, and under netem a lost reply costs a request interval, one second, which is the kernel's SYN timeout. So
the tap fixture resolves the kernel's address once, before any case connects, as a deployment that uses learning
would for a peer it knows, and no case measures the stack's first exchange. The plan checks each case that
counts the kernel's timeouts.

**The operating rule**, for the net page: a static entry does not follow a MAC that changes, so it is pointed at
the router's virtual MAC (VRRP or HSRP), which survives a failover, and the conflict counter is what shows a router
whose MAC changed.

## Where this departs

For the TCP page's note and the net page, each with who decided it.

Against an RFC's text:

- **RFC 6528's clock.** 64 ns units from the stack's own stamp, not a 4 microsecond timer. Linux's unit; the clock
  is the stack's because the brick reads no other. (This design.)
- **RFC 5961's RST rule, widened by two positions**, RCV.NXT - 1 after the peer's FIN and the end of the highest
  range held above a gap. Linux's, for peers that reset from those numbers. (This design, following Linux.)
- **RFC 5961 section 7's throttle**, a count per window of seconds in the RFC's example, is one answer per interval
  per connection. Linux's. (This design, following Linux.)
- **An empty segment exactly at the offered edge is acceptable**, where RFC 9293's test is strictly below it.
  Linux's. (This design, following Linux.)
- **A FIN's number is not held against the offered edge.** Linux's. (This design, following Linux.)

Against Linux:

- **A peer's MSS below the floor refuses the handshake**; Linux raises it to the floor and goes on. (The roadmap's
  text, kept by this design and settled with the author on 2026-11-28.)
- **The acknowledgement of a segment the window refuses is read**; Linux at this commit drops such a segment whole
  unless its receive queue is empty. RFC 9293 3.10.7.4 asks for it. (The roadmap's item.)
- **Resends into a zero window are bounded by a count of unanswered expiries**, where Linux bounds them by 240
  seconds. (This design.)
- **A RST below RCV.NXT is dropped in silence**, where Linux, whose left edge is the last ACK it sent, challenges
  one between that and RCV.NXT. RFC 5961's text. (This design.)
- **The acknowledged sum leaves out the SYN on both opens**, so right after an active open an ACK of the ISS is
  too old, where Linux, which counts the SYN there, takes it as old. (This design; found in the build.)
- **A pure ACK below RCV.NXT that fails PAWS is answered through the limiter**, where Linux's
  `tcp_disordered_ack_check` drops it in silence. (This design; it was under the non-goals.)
- **The RST position at the end of what is held reads the highest range held**, where Linux reads the up-to-four
  blocks it is reporting; with more than four ranges held the two can differ. (This design.)
- **The key is drawn once per stack**, where Linux draws its `net_secret` once per boot. (This design.)
- **A frame that contradicts a static ARP entry is counted**; Linux counts nothing. (#23.)
- **Nothing is resolved on the wire by default.** A host as RFC 1122 describes it resolves its neighbours with
  ARP; the brick does so only when its configuration switches learning on. (The author, 2026-11-28.)
- **A request for our address teaches nothing about a sender the table does not hold**, where RFC 826's
  algorithm adds the sender when we are the target and Linux creates the entry at once. (The author, 2026-11-28,
  on this design's proposal: a frame that can create an entry can fill the table.)
- **A full learned table refuses a new address and evicts nothing**, where Linux collects old entries to make
  room. (The author, 2026-11-28.)
- **A frame nobody asked for does not extend a learned entry's life**, where the brick before this change
  refreshed an entry from any frame that named it. Linux's rule. (This design, from the build's security check.)

## Configuration, counters and what the application sees

`TcpConfig`, validated in `validate`:

| Field | Default | Meaning | Refused when |
|---|---|---|---|
| `challenge_ack_interval` | 500 ms | The least time between two answers that go through the limiter, per connection; zero sends every one. | negative |
| `min_peer_mss` | 48 | A peer whose MSS, stated or assumed, is below it is refused. | below 48, or above the stack's MSS |

`Ipv4Config`, validated in the constructor:

| Field | Default | Meaning | Refused when |
|---|---|---|---|
| `static_arp` | empty | The static table. | as "Neighbours" says; and, with learning off, when a gateway is named that it does not hold |
| learning, a switch | off | Resolve on the wire and keep what is learned, until it expires. | |
| `arp_capacity`, `arp_reachable`, `arp_expire`, `arp_request_interval` | as today | The learned table's size and its aging; read only with learning on. | as today, with learning on; no value disables the expiry |
| `accept_unsolicited_replies` | off | With learning on: report, without storing, a reply addressed to us about an address this shard does not hold. | |

Counters. The names are the builder's to settle; what each counts is fixed here, and each dropped segment still
moves exactly one drop counter.

- TCP, sent: challenge ACKs sent; answers the limiter held back.
- TCP, dropped: a RST inside the window at no accepted position; a SYN on a synchronized connection; a segment
  whose ACK was older than the bound. An ACK above SND.NXT stays in `dropped_unexpected`, and a *no room* segment in
  `dropped_out_of_window`.
- TCP, handshake: handshakes refused for the peer's MSS; SYNs whose payload was discarded.
- Net: frames that contradicted a static entry; lookups and seeds that found the learned table full.
- TCP, dropped: a SYN whose source has no next hop.

Scenario option lines: `--tcp_challenge_ack_interval_ms` and `--tcp_min_peer_mss`.

What the application sees. One change of default: a program that relied on the brick resolving its neighbours
switches learning on, or names them; until it does, `connect` to a neighbour outside the static table returns
`NoRoute`. One existing event gains a cause, `Reset` on an active open refused for the peer's MSS. A forged RST,
SYN or old ACK moves counters and nothing else.

## Parts, and where the state lives

- `core`: the hash (a header) and the random bytes (the compiled part). No state.
- `tcp::detail::IsnGenerator`: the key. Built by the `Lifecycle` from bytes the `Stack`'s construction draws.
- `tcp::detail::Receiver`: every check of "The first checks of a segment", the challenge and the limiter's
  decision. It still owns no state.
- `ControlBlock`: the limiter's stamp, behind the parts an in-order segment touches. `SendSequence`: the sum of
  SND.UNA's advances, written at SND.UNA's one write site, the `AckProcessor`'s cumulative step, and set to zero by
  `Lifecycle::establish`, after the handshake's own ACK passed through that site. It is a 32-bit count that
  saturates: only its minimum with the peer's largest window is read, and that window is below 2^30.
- `tcp::detail::Lifecycle`: the refusal of an active open for the peer's MSS, as one more way a connection ends.
- `tcp::detail::Transmitter`: the expiry at a zero window. `ZeroWindowProbe` may need a second entry point for an
  expiry that sends no probe; its count and its start stamp are the state.
- `net::ArpCache`: the static array and the learned table, rewritten without windows or eviction; whether the
  two are one class or two is the builder's. `net::detail::Arp`: what a frame may change, the conflict, the
  report of a reply this shard does not hold, the call a forwarded resolution arrives through, the counters.
- `Ipv4::next_hop` and `net::detail::Output`: the route question, which with learning off answers nowhere for a
  next hop outside the static table; `net::detail::Routing` does not change. `tcp::detail::Receiver::passive_open`
  asks it before it takes a slot, and `Transmitter::reply_reset` before it sends.
- `runtime::TcpStack`: its ARP sink makes the new call. The loop bricks and the shard context do not change.
- `Ipv4Config`, `Ipv4Counters`, `TcpConfig`, `TcpCounters`: as above.

The dependency directions of the TCP parts and of the IP brick's parts do not change, and no part is added.

## The tick path

What an in-order data segment on an established connection does more than today: the sequence check's new outcome
is one comparison that the existing edge test already makes; the RST and SYN tests are the flag tests of today.
What an in-order ACK does more: one addition to the sum of SND.UNA's advances, and nothing else, since the bound on
an old ACK is read only when the ACK is below SND.UNA. A send does nothing more. A connection's open hashes
twelve bytes once. The limiter's stamp is read and written only on the paths that answer an unacceptable segment.

The benchmark is rerun as every step reruns it: the 64-byte tick at level 0, cross-core, three alternating runs
per side on hugepages, the branch against `main` at the branch point, recorded on
`docs/benchmarks/tick-to-trade.md` whether or not anything moved. Each side runs as it is: the branch with its
static venue entry, `main` with its seeded one, which never refreshes within a run either, so the entry's kind is
not what the comparison measures.

## Fixtures and the scenario runner

- The runner gains the two option lines. Nothing else in the dialect changes: a RST, a SYN or an ACK at any number
  is a packet line today, and a challenge ACK is an outbound `.` line.
- ARP never appears in a script, so the ARP half is tested on the fabric with the net fixture, as the existing ARP
  tests are.
- `frames` already builds every frame these tests inject. If a test of the generator needs the detail header, it
  reaches it as the existing tests of detail classes do.

## Testing

Brick tests, on the fabric, one behaviour each:

- The hash against the reference vectors; the random bytes fill a span and two draws differ.
- The generator: two flows that differ in one field get unrelated numbers; one flow at a later stamp gets a number
  further on by the stamp's difference in 64 ns units; the offset is the same for one flow at any stamp; two keys
  give unrelated numbers; a forced number is used once.
- RST: resets at RCV.NXT; is challenged one byte further and at the last byte of the window; is ignored outside it;
  resets at RCV.NXT - 1 in each of the three states after the peer's FIN and not before it; resets at the end of
  the highest held range, with and without a remembered FIN, when that end is the offered edge too, and not on a
  connection without SACK; a RST with data at RCV.NXT at a zero window resets.
- The edge: an empty segment exactly at the offered edge is accepted and its ACK read, one byte past it is refused;
  a segment above a gap whose FIN stands one past the edge is held and its FIN remembered.
- SYN: challenged in and out of the window on an established connection and after a failed PAWS; the three cases
  in `SynReceived`.
- ACK: above SND.NXT challenged and the payload not taken; older than the bound challenged and neither payload nor
  FIN taken; just inside the bound, payload taken and blocks read; the bound before anything is acknowledged, on an
  active and on a passive open, where the handshake's own ACK is not counted.
- The limiter: a second challenge inside the interval is not sent and is counted; one after it is; an empty
  out-of-window segment and a challenge share the stamp; refused data is answered every time, and so is a FIN sent
  again; a zero interval sends all; a released slot's next connection starts with no stamp.
- The handshake: the RST for a bad ACK in `SynReceived`, with the right number and the slot still half open; the
  floor on both opens, a stated zero and an absent option included; the configuration's bounds; the SYN payload
  counter.
- The window: the ACK of a segment at a zero window frees what it acknowledges and its data is refused; the same
  for a segment one byte too long; a bare FIN at a zero window raises `PeerClosed`; data with a FIN at a zero
  window takes neither; a FIN one past the edge is taken.
- The zero window with data in flight: more than `data_retries` expiries, each answered, leave the connection open;
  `data_retries` in a row with no answer end it, at the expiry the probes' count ends it today; the user timeout
  ends it; after the window opens, the next expiry is the first of `data_retries`.
- Static ARP, the five cases #23 lists: the first send resolves with no request; past `arp_expire` no request goes
  and sends still resolve; a storm of distinct unresolved destinations does not evict it; a reply, a gratuitous ARP
  and a request that name another MAC leave it unchanged and count a conflict; requests for our address are still
  answered. And the configuration's refusals.
- The default: no ARP request ever leaves; a reply teaches nothing; a request for our address is answered; the
  route question answers nowhere for a neighbour outside the table, so `send` and `prebuild` fail `NoRoute` and
  `connect` returns it with no slot taken; a SYN from such a neighbour is dropped, counted and takes no slot; a
  gateway without a static entry is refused at construction; `learn` stores nothing and answers `Off`; a
  `tcp::Stack` over a brick with neither learning nor a static entry is refused, the brick alone is built; nothing
  a test can do changes the static table; a segment from a neighbour outside the table to a closed port draws no
  reset and counts no refused send.
- Learning on: a request from a sender the table does not hold is answered and creates nothing, one from a held
  sender updates it; a reply about an address not held is dropped; a full table refuses a new address, counts
  it and still resolves every entry it held; an expired entry's slot is used again; a slot that never resolved
  is given up after the request interval and not before; the generation moves as today.
- Several queues: a reply about an address this shard does not hold is reported and not stored; a refresh of an
  entry it holds is reported as today; the new call fills an entry on a shard that asked and does nothing on one
  that did not; the runtime's forwarding test, end to end, with learning on. Two existing tests assert that queue
  0 stores a reply it did not ask for (`test_tcp_runtime.cpp`, `test_tcp_steering.cpp`) and are derived again.

Scenario scripts, one per rule a peer can see, in the areas that exist (`reset/`, `handshake/`, `ack/`, `window/`,
`rto/`): a RST inside the window challenged and the exact one that follows resetting; the RST one back after a
FIN; the RST at the end of what is held; a SYN on an established connection; the limiter across two challenges
with the second after the interval; a bad ACK in SYN-RECEIVED; the floor refusing a passive and an active open; an
old ACK and an ACK of unsent data; the ACK of a segment refused at a zero window; the bare FIN at a zero window;
resends into a zero window that keep the connection.

Existing tests and scripts that assert what changes are derived again, not patched to pass: every assertion that a
RST inside the window resets; every assertion of an answer to a second empty out-of-window segment within half a
second, where the test either spaces the segments or sets the interval to zero and says why; the cache's eviction
order; the generation after a seed nobody looked up. The plan lists them from a run of the suite with the change
in.

The two-stack tests and the randomized test keep their assertions about what is delivered. Two things change
between two honest stacks, and the plan derives again whatever counts them: the answers to empty probes are
limited; and an abort that crosses a loss, whose RST leaves from SND.NXT ahead of what the peer has, is now
challenged and resets one round trip later, by the RST that answers the challenge.

No tap case is added. The existing tap suite must still pass, with learning on and the fixture's one resolution
at setup; that needs root and is owed with the earlier ones.

## Documentation

- `docs/architecture/tcp.md`: the key types and configuration and counter tables; the design note on the receive
  path's order; "Hardening is deferred too" rewritten to what is left (SYN cookies, the keyed table hash); the
  entries of "What is still left out" that this closes; the departures note.
- `docs/architecture/net.md`: the static table with the operating rule, the default, learning with what it gives
  and does not give, several queues, and "Deferred" without the rate limit. `docs/architecture/runtime.md`: the
  forwarding as it now works. `docs/guides/getting-started.md` and the TCP page's examples: the examples'
  new arguments, with learning switched on in every tap command line.
- `docs/architecture/core.md`: the hash and the random bytes.
- `docs/architecture/scenario.md`: the two option lines and the new scripts' rows.
- `docs/roadmap.md`: step 6 says what of it has landed. `README.md`: the sentence that says the stack is not
  hardened until step 6 lands says what is and is not yet.
- `docs/benchmarks/tick-to-trade.md`: the rerun.
- `AGENTS.md`: the layout row's description of `core`, `net` and `tcp`, and whatever trap the build finds.

No public page links into `docs/superpowers/`.

## Delivery

Branch `tcp/hardening` off `main`. One pull request, titled
`[TCP][NET][CORE][RUNTIME][FIXTURES] Hardening: keyed sequence numbers, RFC 5961, static ARP by default and the
edge cases`, that
closes #37 and #23 and is the first of four under #21, with a comment on #21 that says what landed and what is
owed. This document and the plan are force-added on the branch, as for every step.

The order, each a commit or a few, each green before the next:

1. `core`: the hash and the random bytes.
2. Keyed initial sequence numbers and timestamp offsets.
3. The limiter, the challenge ACK and the RST rule.
4. The SYN rule, in every state.
5. ACK validation and the bad ACK in SYN-RECEIVED.
6. The sequence check: the fourth outcome with the ACK of a refused segment, the empty segment at the edge, the
   bare FIN at a zero window, the FIN one past the edge.
7. Resends into a zero window.
8. The peer's MSS floor and the SYN payload counter.
9. The static table, and static as the default: the route question, the SYN with no next hop, the fixtures and
   the tap tests switching learning on, the examples' arguments, the benchmark's venue entry.
10. The learned table: no windows, no eviction, a frame changes only the entry of its own address.
11. Several queues: the report without storing, and the call a forwarded resolution arrives through.
12. The documentation, the benchmark's rerun, the gates and the pull request.

Presets, as the author set them for step 5: `debug` with the covering suites on each commit; `debug`, `asan` and
`tsan` at each task's last commit; `gcc-debug`, from a clean build, once at the end; the format and include checks
with every commit. Owed to a machine with root: the tap suite.

## Consequences for later steps

- **PR 2** adds TIME_WAIT on top of these checks: a SYN in TIME_WAIT is the one synchronized state where a SYN may
  open a new connection, and the keyed sequence numbers are what let it be accepted (RFC 6191). It reviews the
  event list, and with it whether an open refused for the peer's MSS keeps `Reset`. Its keepalive probe is an empty
  segment at SND.UNA - 1, which a peer limits as this document limits Linux's.
- **PR 3** validates an ICMP error by the sequence number it quotes, against the same bounds as step 5's.
- **After phase 2**, the keyed flow-table hash takes `core`'s SipHash and a key drawn the same way.
- **A follow-up issue** for the forwarding between shards: one allocation per sibling per reported frame, which a
  flood of replies turns into a flood of forwards when learning is on over several queues.
- **PR 3's ICMP errors and PR 2's keepalive** meet a static next hop that never changes; neither adds a way to
  notice one that died.
- **Not built, and noted on the TCP page:** an empty segment between the last ACK sent and RCV.NXT is refused and
  answered through the limiter, where Linux accepts it.

## Decisions made in the brainstorm

The author's:

- Step 6 is four pull requests, hardening first. "v1" is a release and a branch with no API or ABI promise.
- #37 is part of the hardening, and so is #23, static ARP: one pull request for both halves, the fixtures'
  changes being mechanical.
- ARP, 2026-11-28: a static table from the configuration that is frozen; static only by default, "for trading
  systems"; learning switched on explicitly and always with an expiry; an entry changed only by a frame about its
  own address, and none evicted for another, so the windowed table goes; no perfect hash. Learning on by default
  with entries that never expire was ruled out as open to being filled.
- On this design's proposals, the same day: a frame never creates an entry; a reply that lands on the wrong shard
  is passed on and used only by a shard that asked; the fixtures switch learning on and keep their seeds. The
  peer's MSS below the floor refuses the handshake.

This design's, listed for the author to overrule:

- The MSS floor is Linux's 48, and an active open refused for it raises `Reset`.
- The limiter is Linux's: one answer per connection per 500 ms, one shared stamp, a segment that occupies sequence
  space exempt for everything but challenge ACKs, no limit across connections, and no RST limited.
- A RST resets at Linux's two extra positions as well as at RCV.NXT.
- An empty segment exactly at the offered edge is accepted, and a FIN's number is not held against the edge, both
  as Linux; found by the review of this document, which showed the third RST position could not work without the
  first.
- In `SynReceived`, a SYN at a new number inside the window resets the half-open connection, as Linux, and is not
  challenged.
- The acknowledgement of a segment refused for the window is read, following the RFC and the roadmap where Linux
  no longer does.
- Resends into a zero window are bounded by the count the probes use, not by Linux's 240 seconds.
- One hash and one key give both numbers, as Linux; the key is per stack; the clock term is 64 ns from the stack's
  stamp.
- With learning off: the route question answers nowhere for a neighbour outside the static table, so every
  failure is `NoRoute` and immediate; a gateway with no static entry is refused at construction; a SYN from a
  source with no next hop takes no slot, in both modes.
- With learning on: a slot that never resolved is given up after one request interval; a full table fails the
  lookup as unresolved; the timeouts keep today's defaults, 60 and 120 seconds, and have no value that disables
  them.
- The static table has no fixed limit and is an array; a contradicting frame is counted and not reported.
- `resolved()` keeps reporting what a frame gave an entry the shard holds; the brick keeps one generation, so
  with learning on a learned neighbour's change rebuilds every template once; a reset is not sent where there is
  no next hop; the tap fixture resolves the kernel's address at setup. All four came out of the second review.
- Tests: brick tests and scripts; no new tap case.

The build's, each a ruling made while the plan ran, listed for the author to overrule:

- TS.Recent is written only by a segment that passed every check up to and including the ACK's bounds, as Linux
  updates `ts_recent` inside `tcp_ack`: a forged segment in the window cannot make PAWS refuse the real peer.
- In `SynReceived`, a SYN with ACK at the first SYN's own number is not "a SYN at a new number": it goes on as the
  handshake's ACK with its SYN trimmed, as Linux and the RFC take it.
- The first expiry at a closed window is told by `retransmit_count == 0`, Linux's `!icsk_retransmits`.
- `passive_open` asks in this order: the route, the MSS floor, the node and the slot. A SYN that cannot be answered
  draws nothing, and the floor's RST is one of the answers.
- Only an answer to the brick's own request extends a learned entry's life (rule 1 above, as amended). The author's
  rule said a frame about a held address refreshes it; the security check of the build showed such frames could
  hold an unused entry in the table for ever.
- A forwarded resolution goes through the same door as a frame: it fills or changes an entry the sibling holds,
  extends its life only when that sibling asked, and creates none.
- `ArpCacheConfig::learning` is off by default like the brick's switch, and `arp_reachable`, `arp_expire` and
  `arp_request_interval` each fit a quarter of `core::Duration`.
- `learn` answers what it did, and `tcp::Stack` refuses a brick that can resolve no neighbour (both above, from the
  whole-branch review).
- "A request outstanding" has no time limit: a frame can confirm a stale entry once per request the brick sent
  that was never answered. Bounding it by the request interval is left as a follow-up.
- The benchmark's gate: the first rerun showed p99 50 ns above `main` at a 64-byte tick with p50 equal, the
  second showed none (740 ns on `main` against 720), and no cause was found for the first. The page records both.

## Left to the builder

- Every name not fixed above: the counters, the hash's and the random function's signatures, the new outcome of the
  sequence check, the learning switch, the call a forwarded resolution arrives through, and whether
  `accept_unsolicited_replies` keeps a name that no longer says what it does.
- The learned table's structure, within its three rules; the flow table's is one that fits.
- The examples' argument names.
- Whether the limiter's decision is a member of the `Receiver` or a small class beside `AckPolicy`; where exactly
  the stamp sits in the block.
- How the hash's input is laid out, and whether a fixed-size overload beside the general one is worth having.
- How the zero-window expiry is counted in the code, within the three statements under "Resends into the peer's
  closed window".
- How `prebuild` learns that a hop is static.
- Which existing tests and scripts change, found by running them.
- The wording of every public page.
