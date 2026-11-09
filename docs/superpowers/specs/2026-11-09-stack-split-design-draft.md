# Splitting the TCP stack and the IPv4 brick by responsibility (draft)

Status: design, approved by the author on 2026-11-09 with decision 9 added; from the brainstorm of the same day,
checked against `main` at d15f732. It records what was decided about carving `tcp::Stack` (1646 lines) and
`net::Ipv4` (776 lines) into classes with responsibilities of their own before phase 2 steps 4 to 6 grow both,
and what is left for the builder. It is a pure refactor: no behaviour, counter, public name or test changes.
Private, like every file under `docs/superpowers/`; the public summary is the design-notes paragraph of the TCP
and net pages once the work lands.

## Goal

`tcp::Stack` holds the whole RFC 9293 procedure in one class body under three section comments: the receive
path, the transmit path and the control path. The per-connection state is already one class per concern
(`ReceiveQueue`, `SendSequence`, `RetransmissionQueue`, `LossRecovery`, `RttEstimator`, `ZeroWindowProbe`,
`Timestamps`, `AckPolicy`), and so are the shard-level parts (`ConnectionTable`, `SegmentWriter`, `IsnGenerator`,
the two node pools). What is not split is the procedure that drives them. Step 4 (out-of-order receive, SACK
blocks, window scaling, delayed ACK), step 5 (RACK-TLP) and step 6 (TIME_WAIT, keepalive, ICMP errors) each add
to the receive path, the transmit path and the timers at once; left as it is, the class passes 2,500 lines
within the phase. `net::Ipv4` is smaller and has the same shape: frame input, frame output and ARP in one class.

The split must satisfy the ruling of 2026-10-11: real classes that own a responsibility, each in its own
header with its members in its body, never one class's members spread over files by code path.

## Decided

1. **Three classes over one control block, for TCP.** The procedure splits into a `Receiver` (what a datagram
   does to a connection), a `Transmitter` (everything that leaves, and the timers that guard what left) and a
   `Lifecycle` (slots, states to `Closed`, the timer expiries, close, abort, release). `tcp::Stack` stays as the
   public façade and the owner of what is shared. The names are provisional (see "Left for the builder").
2. **The dependency runs one way: Receiver → Lifecycle → Transmitter.** The Transmitter names neither of the
   other two. What it needs from the receive side is state, not behaviour: RCV.NXT, the window offer, TS.Recent
   and Last.ACK.sent, all members of the connection already. That is the author's "export the dependency into
   the state", and it is Linux's layout too: `tcp_input.c` and `tcp_timer.c` call into `tcp_output.c`, which
   calls neither back (only the timer-arming primitive). Four moves keep the graph acyclic:
   - The immediate ACK today finishes the close after a successful send (`ack_now` → `maybe_finish`). The
     Transmitter only sends; the Receiver, which decides on the immediate ACK, finishes the close.
   - `flush`'s second loop does the same for every ACK that waited. That loop stays on the façade:
     `Stack::flush` walks the Transmitter's ACK list, sends through the Transmitter and finishes through the
     Lifecycle, as `Stack::process` already drains the retry list and raises through the Lifecycle. The first
     loop, the deferred resends, is the Transmitter's alone.
   - The RST for a stray segment takes the datagram's source, the parsed header and the sequence length it
     occupied, not the Receiver's parse result, so the Transmitter's header never includes the Receiver's.
   - Timer arming is the Transmitter's, not the connection's as the brainstorm first said: the RTO's clamp to
     the user timeout reads the head of the retransmission queue, which the Transmitter owns. Expiry handling
     (`on_timer` and the three handlers) is the Lifecycle's and calls down.
3. **Triggers from the receive side to the transmit side are direct calls (my call).** The immediate ACK, the
   fast retransmit, the SYN-ACK, the RST for a stray segment and the FIN a window update releases are sent in
   place, as today, because their order against the next datagram of the burst is observable and asserted by the
   scenario scripts. State defers work only where the stack already defers it: the ordinary ACK until `flush`
   (the ACK list), the retry hint until the next `process` (the retry list), the resend that found its packet
   lent until the next `flush` (the deferred list). No flag-drain pass is added.
4. **The control block is single-thread state.** A connection's receive and transmit run on one shard thread by
   the threading contract. Nothing in the block is atomic, marked shared or treated as remote; the split
   changes the layout of the code, not what the CPU does per segment. A dedicated transmit core, if one ever
   comes for pacing, takes finished frames over a ring, never the control block.
5. **`detail::ControlBlock` is the state `Connection` holds privately today (my call).** It becomes a struct with
   the parts as public members: identity and flow key, the frame template, the events and the four list links,
   the eight protocol parts, the timer node and its kind, the retry counts. `Connection` keeps the public stream
   contract and the queries, holds one block, and forwards its verbs to the stack as it does now; the three
   classes take `ControlBlock&`. The alternative, three `friend` declarations on `Connection` in place of the one
   for `Stack`, is cheaper but hides what is shared inside a class whose public face is the stream.
   The block is what the parts link and time (my call, from the rulings already in force): the four list links
   and the timer node are the block's, so `ConnectionList` and `TimerNode` are typed over `ControlBlock`, which
   `tcp_fwd.hpp`'s generic `Link` and `ConnectionList` allow as they stand; the block carries its slot index and
   the stack pointer, so the stack maps a block back to its `Connection` through the deque and the timer
   trampoline reaches the stack from the block. The deque of `Connection` stays the stack's, since only the
   stack names the passkey. `Connection` gains no friend and no public member. The queries the parts read from
   the connection today (`mss`, `probing`) become the block's, with the stack's MSS recorded in the block when
   the slot opens.
6. **Each class owns the shard-level state that only it drives (my call).** The Transmitter owns the segment
   writer, the send pool and the ACK, retry and deferred lists. The Lifecycle owns the connection table (the
   slot directory, listeners and ephemeral ports), the ISN generator and the event list. The Receiver owns
   nothing: it is the procedure of RFC 9293 section 3.10.7 over the other two. The stack keeps what every part
   reads: the IP brick, which all three take (`connect` routes, `on_datagram` reads the queue's steering, the
   Transmitter sends), the wheel, the config, the MSS and byte budget, the deque of connections, the held-segment
   pool, the counters and the stamp.
7. **Three parts for IPv4, the same way.** An ARP part (the cache, which exists, plus the request, the reply, the
   learn paths and the `resolved()` list), an output part (`send`, `prebuild`, `send_frame`, the checksums and
   the identification counter) and an input part (frame dispatch, the IPv4 checks, delivery to the transport
   lists, the echo reply). Input → output for the echo reply's checksums and identification; input → ARP for ARP
   frames; output → ARP for the next hop's MAC; ARP → the queue alone. `net::Ipv4` stays the façade with the
   config, the subnet and the counters.
8. **Nothing observable changes.** Same public types and members, same counters, same scripts, same test files:
   no test or fixture reaches a private member of either class today, so none changes. The proof is the existing
   suite.
9. **A header whose names are all in `detail` lives in a `detail/` folder (the author's rule, 2026-11-09).** In
   `common/tcp` and `common/net` for now, one flat `detail/` per module at the module's root: every header that
   declares only `aloe::tcp::detail` or `aloe::net::detail` names moves there, the fifteen TCP has today (the
   eight connection parts, the frame template, the three pools, the connection table, the ISN generator and
   the segment writer) as well as the new ones (the three classes and the control block of TCP, the three
   parts of IPv4). They are included as `#include <detail/name.hpp>`, the spelling codestyle.md already gives
   and `check-includes.sh` already resolves by path suffix, as `fabric/detail/frame.hpp` is included today; the
   module root joins the target's include directories so the spelling resolves. A header that mixes public and
   `detail` names stays where it is: `tcp_fwd.hpp`, whose intrusive list is issue #14's. After the move `pool/`
   is gone, `connection/` keeps the connection and its read view, `stack/` keeps the forward header and the
   stack, `arp/` keeps the public `ArpCache`. The grouping the folders gave is carried by the TCP page's list of
   parts instead. The move is the first commit of each PR, a pure move with the include spellings, the CMake
   lists and the umbrella updated, so the split's diff reads on its own.

## The TCP brick after the split

```
Stack<Ip, CC>      process (the stamp, the retry hints, then the Receiver per datagram), flush (the
  │                Transmitter's deferred resends, then the ACK pass: send through the Transmitter, finish
  │                through the Lifecycle), connect, listen, unlisten, poll_event, the accessors; owns ip, wheel,
  │                config, mss, byte budget, the deque of connections, held pool, counters, now; forwards
  │                Connection's verbs; maps a block to its Connection
  ├─► Receiver     on_datagram → checksum, lookup, passive_open, receive_syn_sent, receive_synchronized
  │     │            (accept_sequence, receive_reset, receive_ack with acknowledge_data, on_acknowledged,
  │     │            on_duplicate_ack, fast_retransmit, update_probing, send_unsent; receive_payload;
  │     │            receive_fin); consume; the immediate ACK and the finish behind it
  │     ├─► Lifecycle
  │     └─► Transmitter
  ├─► Lifecycle    table, ISN, event list: bind_flow, open, free_slot, set_closed, finish, maybe_finish,
  │     │          time_out, fail_unresolved, close, abort, release, raise; on_timer and the RTO, ARP-poll and
  │     │          probe expiries; connect's body
  │     └─► Transmitter
  └─► Transmitter  writer, send pool, ACK/retry/deferred lists: emit and its offer bookkeeping, entry, enqueue,
        │          enqueue_fin, reserve_fin_node, transmit_entry, queue_control, acknowledge, resend_head, the
        │          busy rule and the copy, the bare/pure ACK, probe, RST and the stray-segment RST, queue_ack,
        │          mark_retry, writable, prepare, commit, in_flight, drop_queue, arm_*/disarm, the deferred
        │          resends of flush
        └─► SegmentWriter, TimerWheel

all three ─► ControlBlock (per connection), Ip, and the stack's config, counters, held pool and stamp
```

The datagram's path is unchanged: `Stack::process` records the stamp, publishes the retry hints, and hands
each datagram to the Receiver. `Connection::prepare`, `commit`, `writable` and `consume` reach the Transmitter
and the Receiver through the stack; `close`, `abort` and `release` reach the Lifecycle; the timer trampoline
reaches the Lifecycle's `on_timer`.

## The IPv4 brick after the split

```
Ipv4<Device>   process, received, resolved, allocate, learn, resolve, queue, the accessors; owns the queue
  │            reference, config, subnet, counters
  ├─► Input    receive → receive_ipv4 (checks, martian, fragment, broadcast rules) → deliver to tcp/udp lists;
  │     │      receive_icmp builds the echo reply in place
  │     ├─► Output   (checksums, identification, transmit of the reply)
  │     └─► Arp      (receive_arp)
  ├─► Output   send, prebuild, send_frame, finish_checksums, next_mac, next_identification
  │     └─► Arp      (resolve for the next hop's MAC)
  └─► Arp      ArpCache, request_arp, learn, learn_from_wire, the resolved list, the ARP reply
```

Routing queries (`next_hop`, `is_broadcast`, `for_me`) are read by input and output alike; where they sit, on the
façade or in a small value type over the subnet and gateway, is the builder's.

## Prior art

Linux splits the same way and in the same direction: `tcp_input.c` (input, `tcp_ack`, loss recovery),
`tcp_output.c` (every send, retransmit, ACK, probe and RST; it arms the xmit timer), `tcp_timer.c` (expiries,
calling into output), `tcp.c` and `tcp_minisocks.c` (close, `tcp_done`, states). lwIP has `tcp_in.c`,
`tcp_out.c` and `tcp.c` with the same arrows. No userspace stack splits one flow's receive and transmit across
cores; mTCP, Seastar and IX keep a flow's control block on one core, as Aloe's shard does.

## Hazards

- **A template member declared and never defined fails at link time, only once instantiated.** Each new class is
  a template over `<Ip, CongestionControl>`; every member a path reaches is defined in the body in the same
  commit (AGENTS.md trap).
- **Stale objects after the control block's layout changes.** The scenario binaries aborted once after a struct
  the stack compiles in changed shape; rebuild `gcc-debug` with `--clean-first` before chasing an abort.
- **clang-tidy wants `const` on a member that only writes through a reference member.** `SegmentWriter` meets
  this today; the pattern from the traps applies (`core::force_non_const` where the class owns what the reference
  reaches, `const` where it is a non-owning handle).
- **Declaration order.** A part that holds a reference to the counters, the pools or another part is declared
  after them in `Stack`, as `writer_` is declared after `counters_` today.
- **The refused-transmit `NOLINT(bugprone-use-after-move)` lines move with the code**, in both bricks.
- **One more reference per call.** Every part call goes through a reference member; within one translation unit
  clang inlines it, but the benchmark decides, not the assumption.

## Testing

- The existing suite is the specification: `debug`, `gcc-debug`, `asan` and `tsan` green, the scenario corpus
  unchanged and passing in both checksum modes, the format and include checks green. No test file changes; a test
  that would have to change is a sign the split changed behaviour.
- The tick-to-trade benchmark in `release` on hugepages, the same machine as the recorded results, before and
  after each PR; a difference beyond the run-to-run noise recorded in `docs/benchmarks/` blocks the merge. This
  run needs root and is the author's.
- The dependency direction is a review item, read off the includes: `receiver.hpp` includes the other two,
  `lifecycle.hpp` includes the transmitter's header, the transmitter's includes neither.

## Delivery and documentation

- Two pull requests, IPv4 first: it is the small one and sets the pattern (parts in `detail/`, shared counters
  by reference, a façade that owns the rest). Then TCP. Each opens with the `detail/` move as a commit of its
  own (decision 9). Subjects `refactor(net): ...` and `refactor(tcp): ...`; titles `[NET] ...` and `[TCP] ...`.
- Each PR updates its architecture page's design notes (`tcp.md` "One class per concern", whose folder sentence
  now says `detail/` holds every part and names the three classes; `net.md` gains a paragraph on the three
  parts) and the module's row in the AGENTS.md layout table. The roadmap does not change: no phase lands and no
  scope moves.
- Every file under `docs/superpowers/` stays private and is force-added with the PR, as before.

## Left for the builder

- The names: `Receiver`, `Transmitter`, `Lifecycle`, `ControlBlock` for TCP; the ARP, input and output parts of
  IPv4. The author rules on names before the plan is written.
- How the shared services reach the parts: one context struct the stack owns, or individual references in each
  constructor. The stamp is one variable either way.
- Whether `passive_open` splits (the Lifecycle takes and binds the slot, the Receiver fills the handshake state
  and asks the Transmitter for the SYN-ACK) or stays whole in the Receiver with a Lifecycle call for the slot.
- Where the routing queries of IPv4 live, and whether `transmit_built` is the output part's or each part's.
- The accessors each part offers the others for state it owns: the Receiver's `send_unsent` and
  `fast_retransmit` read the queue's head and the deferred list; the Lifecycle's `free_slot`, `set_closed` and
  `connect` read the Transmitter's pool and lists; `passive_open` and `connect` read the ISN offset.
