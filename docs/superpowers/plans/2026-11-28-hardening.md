# Hardening and Edge Cases Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the first of four pull requests of roadmap phase 2, step 6 (GitHub issue #21), closing #37 and #23: initial sequence numbers and timestamp offsets from a keyed hash, the first checks of a segment rewritten to RFC 5961 with limited answers, the leftovers of the handshake and of the window, and ARP that is static by default with learning as an explicit, expiring mode.

**Architecture:** `core` gains SipHash-2-4 and a random-bytes call. `tcp::detail::IsnGenerator` holds a key and turns a flow and a stamp into a sequence number and a timestamp offset. The `Receiver` gets the new order of checks, a fourth outcome of the sequence check, and two answering helpers over a small `AnswerLimiter` in the control block; `SendSequence` counts what the peer acknowledged; the `Transmitter` counts an expiry at a closed window with the zero-window probes. `net::ArpCache` becomes a frozen static array and a learned table with no windows and no eviction, behind a switch that is off by default; the IP brick answers the route question from both, and the runtime passes a forwarded resolution through a call that only fills.

**Tech Stack:** C++26 on libstdc++, clang 22 and GCC 16, CMake and Ninja, GoogleTest, the existing vcpkg dependencies, DPDK 26.03 through `Aloe::Dpdk` inside `ethdev` only. No new dependency; `getrandom(2)` comes from glibc.

**Spec:** [Hardening and edge cases](../specs/2026-11-28-hardening-design.md). Read the spec, `AGENTS.md` and `codestyle.md` before implementation. Both documents live in the git-ignored `docs/superpowers/`; nothing tracked may link to them, and both are force-added to the branch. The spec's "Decisions made in the brainstorm" records who decided what; this plan's own choices are marked "plan's call" where the spec's "Left to the builder" left them open.

## Global Constraints

- "A connection reset by a segment whose sequence number is not one of the exact positions listed under RST" is forbidden. The positions: RCV.NXT; RCV.NXT - 1 in `CloseWait`, `Closing` or `LastAck`; the end of the highest range held above a gap, a remembered FIN's number included, on a connection that uses SACK.
- "An ACK this change adds that does not go through the limiter" is forbidden. No RST is limited. The ACK for a refused segment that occupies sequence space and carries no SYN is never limited.
- "A value drawn from `std::mt19937` or `std::random_device` in any segment, or key material readable through any accessor, counter or log line" is forbidden.
- "A static ARP entry changed, aged or evicted by anything but the destruction of the brick" is forbidden. "An ARP request sent, or anything learned from a frame, by a brick whose configuration did not switch learning on" is forbidden.
- No switch turns the hardening off. `challenge_ack_interval` is the only setting that loosens anything.
- The values the spec fixes, verbatim: `challenge_ack_interval` 500 ms, refused when negative, zero sends every answer; `min_peer_mss` 48, refused below 48 or above the stack's MSS; a peer's MSS is its option's value or 536 without one; the sequence number's clock term is the stamp's nanoseconds since the clock's epoch shifted right by six (64 ns units); the sequence number is the low 32 bits of the hash plus the clock term, the timestamp offset the high 32 bits; the ACK bound is `SND.UNA - min(max_wnd, acknowledged)`; learning is off by default; with learning on the defaults stay `arp_reachable` 60 s, `arp_expire` 120 s, `arp_request_interval` 1 s, `arp_capacity` 256, and no value disables the expiry.
- Hot paths: every member of `tcp::Stack`, `tcp::Connection` and `net::Ipv4` reached after construction is `noexcept`, allocates nothing but packets from IP's pool and nodes from the fixed pools, logs nothing, reads no clock. Errors on hot paths are `std::expected`; constructors throw `std::invalid_argument`, and `std::system_error` when the kernel gives no random bytes.
- Every place where the brick does something other than an RFC's text or Linux carries a comment at the code that names the RFC section or the Linux function, and a line in the TCP page's note "Where the brick departs from the RFCs and from Linux" or the net page's design notes (Task 12). The list is the spec's "Where this departs".
- Scripts are Aloe's own, under `tests/scenarios/tcp/<area>/<name>.pkt`; each opens with a comment stating what it pins and where Linux differs. Every number in a script is derived from the rules under "Read This First", never pasted from a failing run. No script is copied from Linux's corpus.
- An existing test or script that fails is derived again when it asserts something the spec changes, and is a regression to fix in the code otherwise. No test is patched to pass without the sentence of the spec that changes it.
- Every test runs without root, hugepages or a card; the tap suite and the benchmark rerun are manual.
- `AGENTS.md` rules apply unchanged: `tcp` and `net` link neither `execution` nor `log`; `core` has no dependencies; `core::TimePoint` and `core::Duration` for every stamp and interval; one class in one place; a header whose names are all in `detail` sits in the `detail/` folder of the folder it belongs to; public headers define no macros; another module's names qualified with its namespace; test helpers in `aloe::testing`; clang-tidy clean inside the clang builds; the include rules of `./scripts/check-includes.sh`.
- `codestyle.md` applies: by-value parameters `const` in definitions and never in bodiless declarations; `[[nodiscard]]` on every value-returning function; a default member initialiser on every struct member; no integer literal suffixes beyond `U`; concepts over SFINAE.
- Presets, as the author set them: `debug` with the suites that cover the change on each commit; `debug`, `asan` and `tsan` at each task's last commit; `gcc-debug`, from a clean build, once in Task 12; `./scripts/check-format.sh` and `./scripts/check-includes.sh` with every commit.
- Conventional commit subjects with the module as scope: `feat(core)`, `feat(tcp)`, `fix(tcp)`, `feat(net)`, `feat(runtime)`, `feat(scenario)`, `test(tcp)`, `bench(tick-to-trade)`, `docs:`.

## Verified Beforehand

Nothing in this plan was built; the author asks for plans without a prototype. These facts were read from the code on `main` at 7dae733 on 2026-11-26 to 28, and from the Linux kernel at 4405c18c95d2d0f99f6c581ef0ee8591a1aa6ccd by a source survey whose cited lines are in the spec's "What Linux does, checked". The Linux files and RFCs 5961, 6528, 9293, 1122 and 826 are in `.superpowers/sdd/2026-11-28-hardening/reference/` (git-ignored), each Linux file named by its kernel path with the slashes replaced by underscores (`tcp_input.c`, `tcp_output.c`, `tcp_timer.c` and `tcp.h` under their short names).

| Fact | Where it matters |
|---|---|
| `core` is a set of small libraries combined by `add_combined_library` (`common/core/CMakeLists.txt`): `Version`, `Clock` (STATIC, the only compiled one), `Values`, `Guards`, `FixedString`; the umbrella `common/core/export/aloe/core` includes each header. Its tests are `tests/unit_tests/common/core/test_*.cpp`. | Task 1 |
| A throwaway probe (`reference/siphash_probe.py`) computed SipHash-2-4 with the key `00 01 .. 0f` over the input `00 01 .. n-1`: n = 0 gives `0x726fdb47dd0e0e31`, 1 gives `0x74f839c593dc67fd`, 8 gives `0x93f5f5799a932462`, 15 gives `0xa129ca6149be45e5` (the SipHash paper's example), 16 gives `0x3f2acc7f57c29bdb`, 63 gives `0x958a324ceb064572`. Key bytes 0 to 7 are `k0` and 8 to 15 `k1`, both little-endian; the result is `v0 ^ v1 ^ v2 ^ v3`. | Task 1 |
| `tcp::detail::IsnGenerator` (`common/tcp/stack/detail/isn_generator.hpp`) has `next()`, `offset()` and `force(isn)` over one `std::mt19937`. Its only users are `Lifecycle::open` (`lifecycle.hpp:375-383`: `block.send.open(isn_.next())`, `block.timestamps.offer(config.timestamps, isn_.offset())`) and `Lifecycle::force_next_isn`. `open` is called by `connect` and `accept_slot` after `bind_flow` set `block.local` and `block.remote`. The umbrella `<aloe/tcp>` includes every detail header, so a test reaches `tcp::detail::IsnGenerator` through it. | Task 2 |
| `Receiver::on_datagram` (`receiver.hpp:56-138`) parses, checks the checksum, looks the flow up, and dispatches: no flow and a listener goes to `passive_open`; no flow otherwise counts `dropped_no_connection` and calls `transmitter_.reply_reset` unless the segment is a RST; `State::Closed` counts `dropped_closed` and replies the same way; `SynSent` goes to `receive_syn_sent`; every other state to `receive_synchronized`. | Tasks 3 to 9 |
| `receive_synchronized` (`receiver.hpp:297-356`): PAWS (`timestamps.rejects`, then `dropped_paws`, `report_old_duplicate`, `ack_now(block, Edge::Restate)`); `accept_sequence`; `timestamps.update`; a RST calls `lifecycle_.peer_reset`; a SYN counts `dropped_unexpected`; `receive_ack`; above a gap `receive_above_gap`; payload; FIN. `Sequenced` has `Refused`, `InOrder`, `AboveGap`. | Tasks 3 to 6 |
| `accept_sequence` (`receiver.hpp:366-424`): a zero-length segment is acceptable when `window == 0 ? sequence == next : !sequence.before(next) && sequence.before(edge)`; `drop(counter)` counts and calls `ack_now(block, Edge::Restate)` unless the segment is a RST; entirely before RCV.NXT is `dropped_duplicate`, except a duplicate SYN in `SynReceived`, which resends the SYN-ACK (`mark_head_lost`, `resend`); a gap with `window == 0 \|\| end.after(edge)` is `dropped_out_of_window`; an overlap is trimmed; then `window == 0 \|\| end.after(edge)` is `dropped_out_of_window`. `end` counts the SYN and the FIN (`Segment::length()`). | Tasks 3, 4, 6 |
| `receive_ack` (`receiver.hpp:432-467`): no ACK flag counts `dropped_unexpected`; in `SynReceived` an ACK other than SND.NXT counts `dropped_unexpected` and returns, else `handshake_acknowledged` and `lifecycle_.accepted`; an ACK above SND.NXT counts `dropped_unexpected` and calls `ack_now(block, Edge::Restate)`; then `acks_.on_ack`. | Task 5 |
| `AckProcessor` writes SND.UNA in one place, `block.send.una = ack` in `acknowledge` (`ack_processor.hpp:544`), which `handshake_acknowledged` also reaches. `read_blocks` ignores the blocks of an ACK older than `SND.UNA - max_wnd` (`:289`). `on_ack` updates the window only when `!ack.before(una)`, and `update_probing` calls `block.probe.answered()` on every such ACK (`:195-214`). | Tasks 5, 7 |
| `SendSequence` (`connection/detail/send_sequence.hpp`) has `iss`, `una`, `nxt`, `wnd`, `wl1`, `wl2`, `peer_mss`, `peer_shift`, `fin_sent`, `max_wnd`; `update_window` raises `max_wnd`. On a passive open the handshake's ACK raises it through `on_ack`. | Task 5 |
| `Transmitter::send_pure_ack(block, edge)` sends a pure ACK at SND.NXT and counts `pure_acks_sent`; `reply_reset(source, header, length)` sends RFC 793's reset through `SegmentWriter::send_detached` and `Ipv4::send`, counting `resets_sent` on success; `SegmentWriter::count` counts every error but `Unresolved` as `send_refused` (`segment_writer.hpp:202-207`). `Receiver::ack_now` queues an ordinary ACK for the flush when the send is refused, and calls `lifecycle_.maybe_finish` when it left. | Tasks 3, 9 |
| `Transmitter::on_retransmit_timeout` (`transmitter.hpp:1435-1469`): `user_timed_out` first; then `retransmit_count >= (handshake ? retries : data_retries)` is `TimedOut`; `++retransmit_count`; `mark_timeout`; `recovery.on_timeout(..., block.retransmit_count == 1, ...)` off the handshake; the resend of the head; the backoff; `arm_retransmit`. `ZeroWindowProbe` (`connection/detail/zero_window_probe.hpp`) has `start`, `answered`, `deadline`, `expire`, and the private count `out_`. | Task 7 |
| `passive_open` (`receiver.hpp:222-252`) stores `segment.mss.value_or(default_peer_mss)`; `receive_syn_sent` (`:254-294`) stores the same after the ACK check. `TcpConfig::validate(max_l4_size, resolution)` returns the config and throws `std::invalid_argument` naming the rule. `Lifecycle::set_closed` and `raise` are how a connection ends; `peer_reset` counts `connections_reset`. `runtime::Streams` reports a `Reset` before `Connected` as `Refused` (`streams.hpp:343`). | Task 8 |
| `net::ArpCache` (`common/net/ipv4/arp/arp_cache.{hpp,cpp}`): windows of eight, `lookup`, `learn`, `contains`, `confirmed`, `generation`; `insert` evicts in the order expired, lapsed incomplete, oldest reachable, oldest pending. Its tests are `tests/unit_tests/common/net/test_net_arp_cache.cpp`, thirteen cases, four of them about that order. | Task 10 |
| `net::detail::Arp::receive` (`ipv4_arp.hpp:49-106`): a request for our address from a plausible sender calls `learn_from_wire` whether or not the cache holds the sender; a reply for an address the cache does not hold is learned only with `accept_unsolicited_replies` and a target of our address, else `dropped_arp_unsolicited`; `learn_from_wire` reports every mapping in `resolved()`, a refresh with the same MAC included (`test_net_arp.cpp:229`). | Tasks 9 to 11 |
| `net::detail::Routing` has no ARP state; `Output::next_mac` routes and resolves; `Output::prebuild` reads `arp_.confirmed(hop)` for `valid_until` and sets it to `core::TimePoint::max()` for a broadcast or multicast destination (`ipv4_output.hpp:90-116`). `Ipv4::next_hop` has one production caller, `Lifecycle::connect` (`lifecycle.hpp:77`), which returns `ConnectError::NoRoute` when it answers nothing. | Task 9 |
| TCP arms its ARP poll only on `SendError::Unresolved` (`transmitter.hpp:173`); `on_arp_poll` hands any other error to the RTO (`:1574-1577`). | Task 9 |
| `runtime::TcpStack` (`common/runtime/stack/tcp_runtime_stack.hpp`) sets `accept_unsolicited_replies` on a multi-queue device, forwards each `ip_.resolved()` entry to every sibling (`arp_forward.hpp`, one `new (std::nothrow)` node per sibling) and applies a forwarded one with `ip_.learn`. Two tests assert that queue 0 stores a reply it did not ask for: `TcpRuntime.ArpForwardedOnceToSiblings` (`test_tcp_runtime.cpp:258`) and `TcpSteering.GatewayKnownOnlyByQueueZero` (`test_tcp_steering.cpp:184`). | Task 11 |
| About twenty-five files name `Ipv4Config` or build a `net::Ipv4`: `tests/shared/net/net_fixture.hpp` (`stack_config()`, which `tests/shared/tcp/tcp_fixture.hpp` uses and seeds with `ip_.learn(harness_ip, harness_mac, now_)`), `fixtures/scenario/run/harness.cpp:36-45`, `tests/unit_tests/common/{net,tcp,runtime}`, `tests/unit_tests/benchmarks`, `tests/integration_tests/{net,tcp}`, `tests/manual_tests/{net,tcp}`, `examples/{ping_responder,tcp_echo,tcp_echo_tasks}`, `benchmarks/tick_to_trade/{level_bricks.hpp,level_runtime.cpp}`, `common/runtime/stack`. Among the tests that build a brick of their own and seed it: `test_tcp_two_stacks.cpp:59`, `test_tcp_placement.cpp:48,222`, `test_tcp_handshake.cpp:499`, `test_streams.cpp:324`, `test_net_send.cpp:182,257`, `test_net_prebuild.cpp:108`, `test_tcp_ring.cpp:51`, `test_net_ring.cpp:40,119`. | Task 9 |
| The benchmark builds its IP configuration in `stack_ip_config(peer)` (`level_bricks.hpp:16-21`, lifetimes of 24 and 48 hours) and seeds the venue with `learn` unless `--arp-on-wire` (`level_bricks.cpp:41-43`, `level_runtime.cpp:124-126`); `stack_sanity.hpp` treats `tcp.send_refused` and most counters as failures and `tests/unit_tests/benchmarks/test_stack_config.cpp` lists the same counters; both are built only with `-DDO_BENCHMARKS=ON`. `tcp_echo`'s parser finds the optional gateway by testing the fourth argument for a leading `--` (`tcp_echo.cpp:92`). | Tasks 9, 12 |
| The scenario runner: `ScriptTcpOptions` (`fixtures/scenario/script/script.hpp:109-126`), the option names and `set_duration` in `script/parse_script.cpp:690-780`, `configured(base, script)` in `run/harness.cpp:104`. A packet line carries any flags, sequence and acknowledgement numbers, so a RST, a SYN or an ACK at any number needs no new syntax. 93 scripts exist. | Tasks 3, 8 |
| `frames::TcpPeer::segment(flags, payload, sequence, acknowledgement, with_mss = false)` builds any segment at explicit numbers without changing the peer's state; `spec()` reaches its `TcpSpec` (ports, window, `mss`, raw `options`). `TcpHarness` has `receive(frame)`, `advance(by)`, `collect()`, `poll()`, `open_passive(peer)`, `open_passive(peer, round_trip)`, `open_active(peer)`, `configure_tcp(config)`, `force_next_isn(isn)`; `TcpRefusingFixture` has `refuse()` and `allow()`. | every TCP test |

## Read This First

Baseline commands, on a checkout of `main` at or after 7dae733:

```bash
git switch -c tcp/hardening
cmake --preset debug && cmake --build --preset debug && ctest --preset debug      # note the count: the baseline
./scripts/check-format.sh && ./scripts/check-includes.sh
./build/debug/tests/unit_tests/scenarios/Aloe.Tests.Unit.Scenarios               # note the count of script runs
```

**The order of checks on a connection past `SynSent`**, each implemented in exactly one task:

```
0  SynReceived only: ACK set, RST clear, SEG.ACK != SND.NXT        -> RST <SEQ=SEG.ACK>, dropped, slot stays   Task 5
1  PAWS: a SYN that fails is challenged; the answer to any other    -> answer_refused()                         Tasks 3, 4
2  the sequence check: Refused | InOrder | AboveGap | NoRoom                                                    Tasks 3, 4, 6
     Refused + RST   -> silence, except RCV.NXT - 1 after the peer's FIN -> reset                               Task 3
     Refused + SYN   -> challenge, except the duplicate SYN in SynReceived -> SYN-ACK again, limited           Task 4
     Refused + other -> counted as today, answer_refused()                                                      Task 3
3  RST: one of the three positions -> reset; else dropped_reset_challenged, challenge()                         Task 3
4  SYN: past SynReceived -> dropped_syn_challenged, challenge()                                                 Task 4
        in SynReceived   -> RST by reply_reset, slot freed, handshakes_failed
5  ACK: none -> dropped_unexpected; above SND.NXT -> dropped_unexpected, challenge()                            Task 5
        before ack_floor() -> dropped_old_ack, challenge(); else on_ack as today
6  payload: NoRoom -> dropped_out_of_window, ACK at once, never limited                                         Task 6
7  FIN
```

**The two answers** (Task 3), both private members of the `Receiver`:

```
challenge(block)              limiter allows -> pure ACK, edge restated, ++challenge_acks_sent
                              else           -> ++acks_rate_limited
answer_refused(block, seg)    seg.length() == 0 or seg carries SYN -> through the limiter: pure ACK or ++acks_rate_limited
                              else                                 -> pure ACK at once, as ack_now(block, Edge::Restate) today
limiter.allow(now, interval)  interval == 0 -> true; no stamp or now - stamp >= interval -> stamp = now, true; else false
```

A pure ACK the device refuses is queued for the flush as `ack_now` queues it today; the limiter's stamp stands.

**The sequence check's outcomes** (Task 6), with `next` RCV.NXT, `edge` the offered edge, `data_end = SEG.SEQ + payload length` after the old prefix is trimmed, and the FIN's number never held against the edge:

```
zero-length, no SYN, no FIN:   acceptable when !seq.before(next) && !seq.after(edge)      (the edge itself included)
entirely before next:          Refused (dropped_duplicate), except the duplicate SYN in SynReceived
starts above next:             Refused (dropped_out_of_window) when window == 0 or data_end.after(edge); else AboveGap
starts at next after the trim: NoRoom when data_end.after(edge) (at a zero window: any payload); else InOrder
                               a bare FIN at next is InOrder at any window
```

**The numbers of a flow** (Task 2):

```
message = local address (4 bytes, little-endian of to_uint32()) | remote address (4) | local port (2, LE) | remote port (2, LE)
hash    = siphash24(key, message)                                  64 bits
isn     = low32(hash) + uint32((now - epoch) in nanoseconds >> 6)  modulo 2^32
offset  = high32(hash)
```

**ARP's two modes** (Tasks 9 to 11):

```
lookup(address):  static                      -> its MAC, no request, permanent
                  learning off                -> nothing, no request
                  learning on, held           -> as today: fresh, stale with one refresh per interval, or expired
                  learning on, absent         -> a new incomplete entry and a request; table full -> nothing, ++arp_table_full
a frame's sender: static, same MAC            -> nothing
                  static, other MAC           -> ++dropped_arp_static_conflict
                  learning on, held           -> the entry is filled, refreshed or given the new MAC, and reported in resolved()
                  anything else               -> nothing is stored
a request for our address is answered in every case.
full table:       a new address may take the slot of an expired entry, or of an incomplete one whose last request is
                  at least arp_request_interval old; never the slot of a resolved entry that has not expired.
route question:   next_hop(destination) is nothing when routing gives no hop, or, with learning off, when the hop
                  is unicast and not static.
```

**What a script's lines look like for the new answers.** A challenge ACK is an ordinary outbound pure ACK: `> . s:s(0) ack a win w <nop,nop,TS val T ecr E>` with `s` SND.NXT, `a` RCV.NXT and `w` the field the connection's last ACK stated. The limiter is tested by stamps: two inbound lines 100 ms apart draw one outbound line; a third 500 ms after the first draws a second. A script that needs every answer sets `--tcp_challenge_ack_interval_ms=0` and says why in its header.

**Code-base traps for this change** (the rest are in `AGENTS.md`):

- The layouts of `TcpConfig`, `TcpCounters`, `ControlBlock`, `Ipv4Config` and `Ipv4Counters` change; `gcc-debug` is built with `--clean-first`.
- With timestamps on, `mss()` is 1448, and a `frames::TcpPeer` offers none until `set_timestamps(value)`; the same for SACK-permitted and window scaling. A test of the RST position at the end of held data sets SACK-permitted on the peer before the handshake.
- A fixture's handshake has a round trip of zero; the RTO is `rto_min`, 200 ms.
- Each inbound script line is its own tick, and a script whose lines all carry `+0` cannot tell an early answer from a timely one: give the line that must not be answered a later stamp than the one before it.
- `Ipv4Config` gains a `std::vector`, so a function that returns one cannot be `noexcept` or `constexpr`; clang-tidy's `modernize-pass-by-value` wants a constructor that keeps one to take it by value and move it.
- clang-tidy's `readability-convert-member-functions-to-static` fires on a member that ignores `this`; the Receiver's helpers all read the context.
- A test constant named `gamma` collides with glibc; so may `static_mac`-like names with nothing: keep test constants in `aloe::testing`.

## How to Execute and Review

Use superpowers:subagent-driven-development. One task at a time, in order; each ends with every listed test passing and a commit. Implementers and task reviewers run on Opus 5.5 (the author's instruction of 2026-11-21); a small scoped re-review may run on Sonnet. Before writing a part the spec ties to Linux, read the Linux function the task names in the reference folder and stop if it does not say what the spec says.

Per task, a fresh reviewer checks:

1. **Spec conformance.** The task's interfaces match this plan's Interfaces blocks exactly; the behaviour matches the spec sections the task cites; the values of "Global Constraints" appear verbatim; a departure from an RFC or from Linux is one the spec's "Where this departs" lists, with its comment in the code.
2. **The anti-goals.** No reset at an inexact position; no new ACK outside the limiter; no RST limited; nothing from `mt19937` on the wire and no way to read the key; no static entry changed; no request and no learning with learning off; nothing logs, allocates beyond the pools or reads a clock.
3. **Owned state.** Each new field sits in the part that owns its concern with the operations on it; the Receiver gains procedure and no fields; no part reaches into another's members.
4. **Repository rules.** `AGENTS.md` Rules and `codestyle.md`, and the include rules.
5. **Tests.** Each test pins a behaviour that could regress; none needs root, hugepages or a card; every existing test that changed has the spec's sentence that changes it in the commit message or beside the assertion; the scripts binary passes in every mode and policy; every number in a new or changed script is derived, and its header says how.
6. **Build.** `cmake --build --preset debug` is warning-free (clang-tidy runs inside it); both check scripts pass; `asan` and `tsan` pass at the task's last commit.

At the end, a whole-branch review against the spec's "Goals", "Anti-goals", "Where this departs" and "Testing", then the completion gates of Task 12. Pushing the branch, opening the pull request and commenting on #21 wait for the author's go-ahead.

## Review Focus

Five conditions the spec implies that are most likely to bite, each with its pin in the task that owns the code:

1. **Sequence space that wraps inside the new checks.** RCV.NXT - 1, the ACK bound `SND.UNA - min(...)`, the offered edge and the end of held data across 2^32. Pinned by `TheResetPositionsHoldAcrossTheTopOfTheSequenceSpace` in Task 3, `TheAckBoundHoldsAcrossTheTopOfTheSequenceSpace` in Task 5 and `TheEdgeTestsHoldAcrossTheTopOfTheSequenceSpace` in Task 6, with `force_next_isn` or a peer ISN just below the top.
2. **An answer the device refuses.** A challenge ACK or a limited answer that finds no packet or a refusing device must leave an ordinary ACK queued for the flush, must not be retried in a loop, and must leave the limiter stamped. Pinned by `AChallengeTheDeviceRefusesGoesAtTheFlushOnce` in Task 3, on `TcpRefusingFixture`.
3. **A slot used again.** A connection that was limited, refused for its MSS or reset by an exact RST frees its slot; the next connection on it starts with no limiter stamp, a zero acknowledged sum and a fresh pair of numbers. Pinned by `TheNextConnectionOnTheSlotStartsClean` in Tasks 3 and 5.
4. **Every path that could write the static table.** `learn`, `apply_resolution`, a reply, a gratuitous ARP, a request, a lookup at the first stamp and at one a century later, with learning on and off. Pinned by `NothingChangesTheStaticTable` in Task 9.
5. **A learned table with no slot to give.** Full of resolved entries, and full of entries still waiting for their reply inside the request interval: a new address is refused, counted, nothing is evicted, the call returns, and the address is taken once a slot lapses or expires. Pinned by `AFullTableOfResolvedEntriesRefusesAndKeepsEveryOne` and `AFullTableOfWaitingEntriesRefusesUntilOneLapses` in Task 10.

---

## File Structure

| File | Responsibility |
|---|---|
| `common/core/hash/siphash.hpp` (new) | `SipHashKey` and `siphash24`. |
| `common/core/random/random_bytes.{hpp,cpp}` (new) | `random_bytes`, from `getrandom(2)`. |
| `common/core/CMakeLists.txt`, `common/core/export/aloe/core` | The two new libraries and their headers. |
| `common/tcp/stack/detail/isn_generator.hpp` | The key, and the numbers of a flow at a stamp. |
| `common/tcp/connection/detail/answer_limiter.hpp` (new) | `AnswerLimiter`: the stamp of the last limited answer. |
| `common/tcp/connection/detail/control_block.hpp` | The limiter, cleared with the block. |
| `common/tcp/connection/detail/send_sequence.hpp` | The acknowledged sum and the ACK bound. |
| `common/tcp/connection/detail/zero_window_probe.hpp` | An expiry counted without a probe. |
| `common/tcp/stack/detail/receiver.hpp` | The order of checks, the fourth outcome, the two answers, the MSS floor on both opens, the SYN with no next hop. |
| `common/tcp/stack/detail/lifecycle.hpp` | The numbers at `open`; the acknowledged sum zeroed at `establish`; an active open refused. |
| `common/tcp/stack/detail/ack_processor.hpp` | The sum at SND.UNA's write site; the unreachable test in `read_blocks` removed. |
| `common/tcp/stack/detail/transmitter.hpp` | The expiry at a closed window; no reset where there is no next hop. |
| `common/tcp/config/tcp_config.hpp`, `tcp_counters.hpp` | The two settings and their validation; the new counters. |
| `common/tcp/CMakeLists.txt`, `common/tcp/export/aloe/tcp` | The new header. |
| `common/net/ipv4/arp/arp_cache.{hpp,cpp}` | The static array and the learned table. |
| `common/net/ipv4/stack/detail/ipv4_arp.hpp`, `ipv4_output.hpp`, `ipv4.hpp`, `ipv4_config.hpp`, `ipv4_counters.hpp` | What a frame may change; the route question; the configuration and counters. |
| `common/runtime/stack/tcp_runtime_stack.hpp` | The switch under its new name; the sink's call. |
| `fixtures/scenario/script/script.hpp`, `parse_script.cpp`, `run/harness.cpp` | Two option lines; learning on. |
| `tests/shared/net/net_fixture.hpp` and every test, example and benchmark file that builds an IP brick | Learning on, or static entries. |
| `tests/unit_tests/common/core/test_{siphash,random_bytes}.cpp`, `tests/unit_tests/common/tcp/test_tcp_{isn,challenge}.cpp`, `tests/unit_tests/common/net/test_net_static.cpp` (new) and the existing files | The tests. |
| `tests/scenarios/tcp/{reset,handshake,ack,window,rto}/*.pkt` | The scripts. |
| `docs/architecture/{tcp,net,core,runtime,scenario}.md`, `docs/roadmap.md`, `README.md`, `docs/guides/getting-started.md`, `docs/benchmarks/tick-to-trade.md`, `AGENTS.md` | The documentation. |

The names this plan fixes (plan's calls), used from the task that adds each:

| Name | Meaning | Task |
|---|---|---|
| `TcpConfig::challenge_ack_interval` | The limiter's interval. | 3 |
| `TcpCounters::challenge_acks_sent` | Challenge ACKs that left. | 3 |
| `TcpCounters::acks_rate_limited` | Answers the limiter held back: challenges, the ACK for an empty or SYN-bearing refused segment, a duplicate SYN's SYN-ACK. | 3 |
| `TcpCounters::dropped_reset_challenged` | A RST inside the window at no accepted position. | 3 |
| `TcpCounters::dropped_syn_challenged` | A SYN on a connection past `SynReceived`. | 4 |
| `TcpCounters::dropped_old_ack` | A segment whose ACK was below the bound. | 5 |
| `TcpConfig::min_peer_mss`, `TcpCounters::handshakes_refused_mss` | The floor; opens refused under it, passive and active. | 8 |
| `TcpCounters::syn_payload_discarded` | SYNs to a listener that carried payload. | 8 |
| `TcpCounters::dropped_no_route` | A SYN to a listener whose source has no next hop. | 9 |
| `Ipv4Config::static_arp`, `Ipv4Config::arp_learning` | The static table; the switch, `false` by default. | 9 |
| `Ipv4Config::report_unsolicited_replies` | `accept_unsolicited_replies` renamed: it reports and no longer stores. | 11 |
| `Ipv4Counters::dropped_arp_static_conflict`, `Ipv4Counters::arp_table_full` | A frame that contradicted a static entry; a lookup or seed that found the learned table full. | 9, 10 |
| `Ipv4::apply_resolution(address, mac, now)` | A forwarded resolution: fills or updates an entry the brick holds, else nothing. | 11 |
| `--tcp_challenge_ack_interval_ms`, `--tcp_min_peer_mss` | Scenario option lines. | 3, 8 |

---

## Task 1: SipHash-2-4 and random bytes in `core`

Spec: "Initial sequence numbers and timestamp offsets" ("The hash"); "Constraints", first bullet.

**Files:**
- Create: `common/core/hash/siphash.hpp`, `common/core/random/random_bytes.hpp`, `common/core/random/random_bytes.cpp`
- Modify: `common/core/CMakeLists.txt`, `common/core/export/aloe/core`, `tests/unit_tests/common/core/CMakeLists.txt`
- Test: `tests/unit_tests/common/core/test_siphash.cpp`, `tests/unit_tests/common/core/test_random_bytes.cpp` (new)

**Interfaces:**
- Produces, `aloe::core` in `siphash.hpp`:

```cpp
/// A SipHash key: bytes 0 to 7 of the 16-byte key are `k0`, bytes 8 to 15 `k1`, both little-endian.
struct SipHashKey {
    std::uint64_t k0 = 0;
    std::uint64_t k1 = 0;

    friend constexpr bool operator==(const SipHashKey&, const SipHashKey&) noexcept = default;
};

/// SipHash-2-4 of `message` under `key`: two compression rounds per eight-byte word, four finalization rounds.
[[nodiscard]] constexpr std::uint64_t siphash24(const SipHashKey& key, std::span<const std::byte> message) noexcept;
```

- Produces, `aloe::core` in `random_bytes.hpp`:

```cpp
/// Fills `out` from the kernel's random source, `getrandom(2)`. Setup only: it may block until the kernel's pool
/// is ready. Retries on `EINTR` and on a short read; throws `std::system_error` on any other failure.
void random_bytes(std::span<std::byte> out);
```

- CMake: `${CORE}.Hash` as an INTERFACE library over `hash/siphash.hpp`, `${CORE}.Random` as a STATIC library over the two random files, both added to the combined library; the umbrella includes `<siphash.hpp>` and `<random_bytes.hpp>`.

- [ ] **Step 1: Write the failing tests**

`test_siphash.cpp`, with `key = {.k0 = 0x0706050403020100, .k1 = 0x0f0e0d0c0b0a0908}` and `message(n)` the bytes `00 01 .. n-1`:

| Test | Expects |
|---|---|
| `MatchesTheReferenceVectors` | `siphash24(key, message(n))` is `0x726fdb47dd0e0e31`, `0x74f839c593dc67fd`, `0x93f5f5799a932462`, `0xa129ca6149be45e5`, `0x3f2acc7f57c29bdb`, `0x958a324ceb064572` for n = 0, 1, 8, 15, 16, 63 |
| `IsConstexpr` | a `static_assert` on the n = 0 value |
| `AnotherKeyGivesAnotherHash` | the same message under `{.k0 = 1}` differs from the one under `{.k0 = 2}` |

`test_random_bytes.cpp`: `FillsTheSpanAndTwoDrawsDiffer` (two 32-byte draws differ, and neither is all zero); `AnEmptySpanIsFine`.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset debug`
Expected: FAIL to compile, `siphash.hpp` not found.

- [ ] **Step 3: Implement the hash, the random call, the CMake targets and the umbrella lines**

The hash follows the reference algorithm: `v0 = k0 ^ 0x736f6d6570736575`, `v1 = k1 ^ 0x646f72616e646f6d`, `v2 = k0 ^ 0x6c7967656e657261`, `v3 = k1 ^ 0x7465646279746573`; the final word is the remaining bytes little-endian with the message length in its top byte; `v2 ^= 0xff` before the four final rounds. `reference/siphash_probe.py` is a readable copy.

- [ ] **Step 4: Run everything**

Run: `ctest --preset debug`, `./scripts/check-format.sh && ./scripts/check-includes.sh`, then `asan` and `tsan` (`cmake --preset asan && cmake --build --preset asan && ctest --preset asan`, the same for `tsan`).
Expected: PASS, the baseline count plus the new tests.

- [ ] **Step 5: Commit**

```bash
git add common/core tests/unit_tests/common/core
git commit -m "feat(core): SipHash-2-4 and random bytes from the kernel"
```

---

## Task 2: Keyed initial sequence numbers and timestamp offsets

Closes #37. Spec: "Initial sequence numbers and timestamp offsets". Read `secure_seq.c` in the reference folder first.

**Files:**
- Modify: `common/tcp/stack/detail/isn_generator.hpp`, `common/tcp/stack/detail/lifecycle.hpp`
- Test: `tests/unit_tests/common/tcp/test_tcp_isn.cpp` (new, added to `tests/unit_tests/common/tcp/CMakeLists.txt`)

**Interfaces:**
- Consumes: `core::SipHashKey`, `core::siphash24`, `core::random_bytes` (Task 1).
- Produces, `tcp::detail::IsnGenerator`, replacing `next()` and `offset()`:

```cpp
/// The two numbers a connection opens with.
struct Numbers {
    wire::TcpSequence isn{};
    std::uint32_t timestamp_offset = 0;
};

explicit IsnGenerator(core::SipHashKey key) noexcept;

/// A key from the kernel's random source. Setup only; throws what `core::random_bytes` throws.
[[nodiscard]] static core::SipHashKey draw_key();

/// The numbers of the flow `local` to `remote` at `now` ("Read This First"); a forced sequence number replaces
/// the computed one once.
[[nodiscard]] Numbers numbers(Endpoint local, Endpoint remote, core::TimePoint now) noexcept;

void force(wire::TcpSequence isn) noexcept;
```

  `Lifecycle` builds its generator with `IsnGenerator::draw_key()` in its constructor's initialiser list, and `open(block)` calls `numbers(block.local, block.remote, context_.now)` once for both values. The key has no accessor.

- [ ] **Step 1: Write the failing tests**

`test_tcp_isn.cpp`, on a generator built with a fixed key, `flow = {10.0.0.2:40000 -> 10.0.0.1:443}`, `t0 = core::TimePoint{}`:

| Test | Expects |
|---|---|
| `TwoFlowsThatDifferInOneFieldGetUnrelatedNumbers` | changing each of the four fields in turn changes both `isn` and `timestamp_offset` |
| `OneFlowLaterStartsFurtherOnByTheClock` | `numbers(flow, t0 + 64 µs).isn.value - numbers(flow, t0).isn.value == 1000` |
| `TheClockTermWrapsWithTheSequenceSpace` | at `t0 + 275 s` the difference is `uint32((275e9) >> 6)`, computed in the test in 64-bit arithmetic and truncated |
| `TheOffsetIsTheSameForOneFlowAtAnyStamp` | equal at `t0` and `t0 + 1 h` |
| `AnotherKeyGivesUnrelatedNumbers` | two generators, one flow, one stamp: both numbers differ |
| `AForcedNumberIsUsedOnceAndLeavesTheOffset` | `force(x)`: the next `numbers` has `isn == x` and the offset of an unforced call; the call after it is computed |
| `TheNumbersAreTheHashOfTheFlow` | `isn.value == uint32(h)` and `timestamp_offset == uint32(h >> 32)` at `t0`, with `h = core::siphash24(key, the twelve bytes of "Read This First")` built in the test |

And in `test_tcp_handshake.cpp`: `TwoConnectionsOfOneStackStartAtUnrelatedNumbers` (two active opens to two ports of the peer: the SYNs' sequence numbers differ, and so do their first `TS val`s less the stack's clock), and `AnActiveOpenOnTheSameFlowLaterStartsFurtherOn` is not written: `connect` never reuses a port while the first connection holds it.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset debug`
Expected: FAIL to compile, `IsnGenerator::Numbers` undeclared.

- [ ] **Step 3: Implement the generator and the two call sites**

- [ ] **Step 4: Run everything**

Run: `ctest --preset debug`, the scripts binary, both check scripts, then `asan` and `tsan`.
Expected: PASS. No script number moves: the scripts read the stack's numbers from the wire.

- [ ] **Step 5: Commit**

```bash
git add common/tcp tests/unit_tests/common/tcp
git commit -m "feat(tcp): initial sequence numbers and timestamp offsets from a keyed hash"
```

---

## Task 3: The limiter, the challenge ACK and the RST rule

Spec: "The first checks of a segment", steps 1 to 3, "The challenge ACK" and "The limiter". Read `tcp_validate_incoming`, `tcp_reset_check`, `tcp_send_challenge_ack` and `tcp_oow_rate_limited` in `tcp_input.c` first.

**Files:**
- Create: `common/tcp/connection/detail/answer_limiter.hpp`
- Modify: `common/tcp/connection/detail/control_block.hpp`, `common/tcp/stack/detail/receiver.hpp`, `common/tcp/config/tcp_config.hpp`, `tcp_counters.hpp`, `common/tcp/CMakeLists.txt`, `common/tcp/export/aloe/tcp`, `fixtures/scenario/script/script.hpp`, `fixtures/scenario/script/parse_script.cpp`, `fixtures/scenario/run/harness.cpp`
- Test: `tests/unit_tests/common/tcp/test_tcp_challenge.cpp` (new), `tests/unit_tests/common/tcp/test_tcp_handshake.cpp` (`ConfigValidation`), the scenario parser's tests, `tests/scenarios/tcp/reset/*.pkt`

**Interfaces:**
- Produces, `tcp::detail::AnswerLimiter`:

```cpp
/// When the connection last sent an answer that goes through the limit (RFC 5961 section 7, Linux's
/// `last_oow_ack_time`): one stamp, shared by every kind of limited answer.
class AnswerLimiter {
public:
    /// True when an answer may go at `now`, which becomes the stamp; false within `interval` of the last one. A
    /// zero interval always allows.
    [[nodiscard]] bool allow(core::TimePoint now, core::Duration interval) noexcept;
};
```

  `ControlBlock` gains `AnswerLimiter answers{};` behind `sack`, reset in `clear()`.
- Produces, `TcpConfig::challenge_ack_interval` (`core::Duration`, 500 ms; `validate` throws `"TcpConfig: challenge_ack_interval must not be negative"`), the counters `challenge_acks_sent`, `acks_rate_limited`, `dropped_reset_challenged`, and the script option `--tcp_challenge_ack_interval_ms`.
- Produces, private members of `Receiver`, used by Tasks 4 to 6:

```cpp
/// A challenge ACK (RFC 5961): the connection's pure ACK with the edge restated, through the limiter.
void challenge(ControlBlock& block) noexcept;

/// The ACK that answers a refused segment: through the limiter when the segment occupies no sequence space or
/// carries a SYN (Linux's `tcp_oow_rate_limited`), at once otherwise.
void answer_refused(ControlBlock& block, const Segment& segment) noexcept;
```

  Every `ack_now(block, Edge::Restate)` that answers a segment refused by PAWS or by the sequence check becomes `answer_refused`. The others (refused data with no slot or node, data after the peer's FIN, a second FIN) carry payload or a FIN and stay as they are.

- [ ] **Step 1: Write the failing tests**

`test_tcp_challenge.cpp`, on `TcpFixture` unless a row says otherwise; `c` is a connection from `open_passive(peer_)`, `next` the peer's `snd_nxt()`, "RST at x" is `peer_.segment({Rst}, {}, x, peer_.rcv_nxt())`:

| Test | Does | Expects |
|---|---|---|
| `ARstAtRcvNxtResets` | RST at `next` | `Reset` raised, `connections_reset == 1`, nothing sent |
| `ARstInsideTheWindowIsChallenged` | RST at `next + 1`, then at `next + receive_window() - 1` after 500 ms | each time: state unchanged, one pure ACK at SND.NXT acknowledging `next`, `dropped_reset_challenged` and `challenge_acks_sent` up by one |
| `ARstOutsideTheWindowIsIgnored` | RST at `next - 2` and at the edge plus one | nothing sent, no counter but `dropped_duplicate` or `dropped_out_of_window` |
| `ARstOneBackAfterThePeersFinResets` | parametrised over `CloseWait`, `Closing`, `LastAck`: reach the state, RST at `next - 1` | `Reset` |
| `ARstOneBackBeforeThePeersFinIsIgnored` | `Established`, RST at `next - 1` | nothing |
| `ARstAtTheEndOfWhatIsHeldResets` | SACK-permitted peer; a segment of 100 bytes at `next + 1000`; RST at `next + 1100` | `Reset` |
| `ARstAtTheEndOfAHeldFinResets` | the same with a FIN on the held segment; RST at `next + 1101` | `Reset` |
| `ARstAtTheEndOfWhatIsHeldIsChallengedWithoutSack` | the same without SACK-permitted | challenged |
| `TheChallengedPeerThatAnswersWithAnExactRstResets` | RST at `next + 5`; then a RST at the challenge's acknowledgement number | challenged, then `Reset` |
| `ASecondChallengeInsideTheIntervalIsHeldBack` | two RSTs at `next + 1`, 100 ms apart; a third at 500 ms after the first | answers 1, 0, 1; `acks_rate_limited == 1` |
| `AnEmptySegmentOutsideTheWindowSharesTheLimit` | a pure ACK at `next - 1`, then a RST at `next + 1` 100 ms later | one answer, `acks_rate_limited == 1` |
| `RefusedDataIsAnsweredEveryTime` | the same duplicate data segment three times in one tick | three ACKs, `acks_rate_limited == 0` |
| `AFinSentAgainIsAnsweredEveryTime` | the peer's FIN, acknowledged; the same FIN twice more in one tick | two more ACKs |
| `AZeroIntervalSendsEveryAnswer` | `configure_tcp({.challenge_ack_interval = 0})`; three RSTs at `next + 1` in one tick | three challenges |
| `AChallengeTheDeviceRefusesGoesAtTheFlushOnce` | `TcpRefusingFixture`: `refuse()`, RST at `next + 1`, `allow()`, `collect()` | one pure ACK at the flush; a second RST 100 ms later draws none |
| `TheNextConnectionOnTheSlotStartsClean` | challenge, exact RST, `release()`, a new passive open on the slot, RST at its `next + 1` in the same tick | challenged at once |
| `TheResetPositionsHoldAcrossTheTopOfTheSequenceSpace` | peer ISN `0xffffff00`, 0x200 bytes delivered so RCV.NXT wrapped; the RST cases at `next`, `next + 1`, and `next - 1` after a FIN | as above |
| `ANegativeIntervalIsRefused` (in `ConfigValidation`) | `challenge_ack_interval = -1 ms` | `std::invalid_argument` |

Scripts, in `tests/scenarios/tcp/reset/`: `rst-inside-the-window-is-challenged.pkt` (a RST at `101:101`, a challenge `> . 1:1(0) ack 1`, then `R` at `1:1` and `read` reports `ECONNRESET`); `rst-one-back-after-the-fin-resets.pkt`; `rst-at-the-end-of-held-data-resets.pkt` (a `sackOK` peer); `challenges-are-limited.pkt` (three RSTs at `+0`, `+0.1` and `+0.4`: answers after the first and third). The existing `rst-in-established.pkt` stays as it is: its RST is exact.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset debug && ctest --preset debug -R Tcp`
Expected: FAIL to compile, `challenge_ack_interval` undeclared.

- [ ] **Step 3: Implement the limiter, the two answers, the RST rule, the setting and the option line**

The RST at RCV.NXT - 1 is recognised in the sequence check's refused branch, before the silent drop. The end of what is held is read from the reassembly queue's highest range, one further when a FIN is remembered at its end.

- [ ] **Step 4: Run everything and derive again what changed**

Run: `ctest --preset debug`, the scripts binary, both check scripts.
Expected: the new tests pass. Existing tests that assert a RST inside the window resets (`TcpClose.ReceivedResetThenLateSegmentsGetRstAndRstGetsSilence` and the receive side's cases where a RST arrives above a gap are the likely ones), or that count an answer to a second empty out-of-window segment within 500 ms, fail: derive each again from the spec, space its segments or set the interval to zero with a comment that says why. Then `asan` and `tsan`.

- [ ] **Step 5: Commit**

```bash
git add common/tcp fixtures/scenario tests
git commit -m "feat(tcp): challenge ACKs through a limiter, and a RST that resets only at an exact position"
```

---

## Task 4: The SYN rule, in every state

Spec: "The first checks of a segment", steps 1, 2 and 4. Read `tcp_validate_incoming`'s step 4 and `tcp_check_req` (`net_ipv4_tcp_minisocks.c:724-900`) first.

**Files:**
- Modify: `common/tcp/stack/detail/receiver.hpp`, `common/tcp/config/tcp_counters.hpp`
- Test: `tests/unit_tests/common/tcp/test_tcp_challenge.cpp`, `test_tcp_handshake.cpp`, `tests/scenarios/tcp/handshake/*.pkt`

**Interfaces:**
- Consumes: `Receiver::challenge`, `Receiver::answer_refused`, `AnswerLimiter` (Task 3).
- Produces: the counter `dropped_syn_challenged`. A SYN on a connection past `SynReceived` counts it alone, wherever its sequence number lies and whether or not it passed PAWS.

- [ ] **Step 1: Write the failing tests**

| Test | Does | Expects |
|---|---|---|
| `ASynInsideTheWindowIsChallenged` | established; `peer_.segment({Syn}, {}, next + 10, 0)` | state unchanged, one challenge, `dropped_syn_challenged == 1`, `dropped_unexpected == 0` |
| `ASynOutsideTheWindowIsChallenged` | SYN at `next - 100` and, 500 ms later, at the edge plus 100 | challenged each time |
| `ASynThatFailsPawsIsChallenged` | timestamps on; a SYN whose `TS val` is older than TS.Recent | challenged, `dropped_paws == 0` |
| `ASynAckOnAnEstablishedConnectionIsChallenged` | the peer's SYN-ACK again after `Connected` | one challenge: the ACK the peer lost |
| `ADuplicateSynInSynReceivedGetsTheSynAckThroughTheLimit` | the SYN, then the same SYN twice 100 ms apart, then at 500 ms | the SYN-ACK is resent for the second and the fourth; `acks_rate_limited == 1` |
| `ASynAtANewNumberInSynReceivedResetsTheHalfOpenConnection` | the SYN at `irs`, then a SYN at `irs + 1000` | a RST with ACK acknowledging `irs + 1001`, `handshakes_failed == 1`, `table_size() == 0`; a third SYN opens afresh |
| `ASynOutsideTheWindowInSynReceivedIsAnsweredThroughTheLimit` | a SYN at `irs - 1000` | one pure ACK, slot kept |

Scripts: `handshake/syn-on-established-is-challenged.pkt`; `handshake/duplicate-syn-is-answered-through-the-limit.pkt`. The existing `handshake/syn-ack-again-after-a-restating-ack.pkt` and `TcpHandshake.DuplicateSyn` send a duplicate SYN: check their stamps against the limit and derive them again if a resend they expect now falls inside it.

- [ ] **Step 2: Run them to see them fail**

Run: `ctest --preset debug -R Tcp`
Expected: FAIL: the SYN is counted in `dropped_unexpected` and draws nothing.

- [ ] **Step 3: Implement the rule at PAWS, in the refused branch and at step 4**

- [ ] **Step 4: Run everything**

Run: `ctest --preset debug`, the scripts binary, both check scripts, then `asan` and `tsan`.
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add common/tcp tests
git commit -m "feat(tcp): a SYN on a synchronized connection is challenged"
```

---

## Task 5: ACK validation, and the bad ACK in SYN-RECEIVED

Spec: "The first checks of a segment", steps 0 and 5; "The handshake's leftovers", first paragraph. Read `tcp_ack` (`tcp_input.c:4333-4355`) and `tcp_check_req` (`net_ipv4_tcp_minisocks.c:836-839`) first.

**Files:**
- Modify: `common/tcp/connection/detail/send_sequence.hpp`, `common/tcp/stack/detail/receiver.hpp`, `ack_processor.hpp`, `lifecycle.hpp`, `common/tcp/config/tcp_counters.hpp`
- Test: `tests/unit_tests/common/tcp/test_tcp_challenge.cpp`, `test_tcp_handshake.cpp`, `tests/scenarios/tcp/ack/*.pkt`, `handshake/*.pkt`

**Interfaces:**
- Consumes: `Receiver::challenge` (Task 3).
- Produces, `SendSequence`:

```cpp
std::uint64_t acknowledged = 0;  ///< The sum of SND.UNA's advances since the connection became Established.

/// RFC 5961 5.2's lower bound as Linux computes it: SND.UNA less the smaller of the largest window the peer
/// ever offered and what it has acknowledged. An ACK before it is too old to be the peer's.
[[nodiscard]] wire::TcpSequence ack_floor() const noexcept;
```

  The sum is added to where `AckProcessor` writes SND.UNA and set to zero by `Lifecycle::establish`. The counter `dropped_old_ack`. `read_blocks`'s test for an ACK older than the largest window goes, with its comment's sentence, since no such ACK reaches it.

- [ ] **Step 1: Write the failing tests**

| Test | Does | Expects |
|---|---|---|
| `AnAckOfUnsentDataIsChallengedAndItsPayloadNotTaken` | data with `ack = SND.NXT + 1` | `dropped_unexpected == 1`, one challenge, `unread()` empty |
| `AnAckBelowSndUnaBeforeAnythingWasAcknowledgedIsTooOld` | right after the handshake, on a passive and on an active open: data with `ack = SND.UNA - 1` | `dropped_old_ack == 1`, one challenge, payload not taken |
| `AnAckJustInsideTheBoundIsOldAndItsPayloadIsTaken` | the stack sent 3000 bytes, all acknowledged; data with `ack = SND.UNA - 3000` | payload taken, no challenge, `dropped_old_ack == 0` |
| `AnAckOneBelowTheBoundIsChallengedWithItsFin` | the same with `ack = SND.UNA - 3001` and a FIN | `dropped_old_ack == 1`, `peer_closed()` false |
| `TheBoundIsTheLargestWindowOnceMoreWasAcknowledged` | the peer's window 1000 throughout, 3000 bytes acknowledged; `ack = SND.UNA - 1000` taken, `ack = SND.UNA - 1001` challenged | as stated |
| `TheHandshakesOwnAckIsNotCounted` | `c->acknowledged()` is not exposed: assert through the bound, as the second row does for both opens | as stated |
| `ABadAckInSynReceivedIsResetAndTheSlotStays` | the SYN; a segment with ACK and `ack = ISS + 2` | one RST whose sequence number is `ISS + 2`, no ACK flag, `dropped_unexpected == 1`, `table_size() == 1`; the right ACK then completes the handshake |
| `ABadAckInSynReceivedIsResetWhateverItsSequence` | the same at a sequence number far outside the window | the same RST |
| `ARstWithABadAckInSynReceivedDrawsNothing` | RST and ACK set, `ack = ISS + 2`, sequence inside the window but not exact | challenged by Task 3's rule, no RST |
| `TheAckBoundHoldsAcrossTheTopOfTheSequenceSpace` | `force_next_isn(0xfffffff0)`, 3000 bytes acknowledged so SND.UNA wrapped; the bound's two sides | as the bound rows |
| `TheNextConnectionOnTheSlotStartsClean` (extended) | after a connection that acknowledged 3000 bytes, the next on the slot: `ack = SND.UNA - 1` | too old |

Scripts: `ack/ack-of-unsent-data-is-challenged.pkt`, `ack/ack-older-than-what-was-acknowledged-is-challenged.pkt`, `handshake/bad-ack-in-syn-received-is-reset.pkt` (`< . 1:1(0) ack 5` answered by `> R 5:5(0)`, then the right ACK and `accept`).

- [ ] **Step 2: Run them to see them fail**

Run: `ctest --preset debug -R Tcp`
Expected: FAIL: `dropped_old_ack` undeclared.

- [ ] **Step 3: Implement step 0, the bound, the sum and its reset, and remove the unreachable test**

Step 0 runs first in `receive_synchronized`, before PAWS, as Linux checks the ACK of a request before its window; its RST is `transmitter_.reply_reset`.

- [ ] **Step 4: Run everything**

Run: `ctest --preset debug`, the scripts binary, both check scripts, then `asan` and `tsan`.
Expected: PASS. A test that fed `read_blocks` an ACK older than a window (`test_tcp_scoreboard.cpp`, the too-old return) now sees the segment challenged: derive it again.

- [ ] **Step 5: Commit**

```bash
git add common/tcp tests
git commit -m "feat(tcp): validate the ACK against what was sent and acknowledged, and reset a bad ACK in SYN-RECEIVED"
```

---

## Task 6: The sequence check's fourth outcome

Spec: "The first checks of a segment", step 2 and step 6; "The window's leftovers", first two paragraphs. Read `tcp_sequence` (`tcp_input.c:4873-4900`) and `tcp_data_queue` (`:5700-5714`) first.

**Files:**
- Modify: `common/tcp/stack/detail/receiver.hpp`
- Test: `tests/unit_tests/common/tcp/test_tcp_receive_window.cpp`, `test_tcp_window.cpp`, `test_tcp_close.cpp`, `test_tcp_challenge.cpp`, `tests/scenarios/tcp/window/*.pkt`

**Interfaces:**
- Consumes: `Receiver::answer_refused`, the RST rule (Task 3).
- Produces: `Sequenced::NoRoom`, documented `///< At RCV.NXT, with payload the window does not cover: its RST, SYN and ACK are processed and its payload refused.`; the sequence check as "Read This First" states it.

- [ ] **Step 1: Write the failing tests**

A closed window is built as `test_tcp_receive_window.cpp`'s cases build one: a small `receive_segments`, the peer's data left unread.

| Test | Does | Expects |
|---|---|---|
| `TheAckOfASegmentRefusedAtAZeroWindowIsRead` | the stack has 1000 bytes unacknowledged; our window is zero; the peer sends 100 bytes at RCV.NXT with `ack` covering the 1000 | `Acked` raised, `unacknowledged() == 0`, `dropped_out_of_window == 1`, `unread()` unchanged, one ACK at once |
| `TheAckOfASegmentOneByteTooLongIsRead` | 500 bytes of window left; 501 bytes at RCV.NXT with a new ACK | the same, nothing of the 501 taken |
| `TheWindowFieldOfARefusedSegmentIsRead` | the same segment states a window twice the last | `Writable` when it opens room |
| `ARstWithDataAtRcvNxtAtAZeroWindowResets` | RST with 10 bytes at RCV.NXT | `Reset` |
| `ASegmentAboveAGapPastTheEdgeIsStillRefusedWhole` | a new ACK on a segment that starts above RCV.NXT and ends past the edge | `unacknowledged()` unchanged, `dropped_out_of_window == 1` |
| `ABareFinAtAZeroWindowIsTaken` | our window zero; the peer's bare FIN at RCV.NXT | `PeerClosed`, state `CloseWait`, the FIN acknowledged |
| `DataWithAFinAtAZeroWindowTakesNeither` | 10 bytes and a FIN at RCV.NXT | `peer_closed()` false, `dropped_out_of_window == 1` |
| `AFinOnePastTheEdgeIsTaken` | a segment whose payload ends exactly at the edge, with a FIN | payload held, `PeerClosed` |
| `AFinOnePastTheEdgeAboveAGapIsRemembered` | the same above a gap, then the gap filled | `PeerClosed` once the gap fills |
| `AnEmptySegmentAtTheEdgeIsAccepted` | a hole at RCV.NXT, the window filled above it, the peer's pure ACK at the edge with a new acknowledgement | `Acked`, no drop counter, no answer |
| `AnEmptySegmentOnePastTheEdgeIsRefused` | the same one byte further | `dropped_out_of_window == 1`, answered through the limiter |
| `ARstAtTheEdgeThatIsTheEndOfWhatIsHeldResets` | SACK-permitted; the window filled above a hole; RST at the edge | `Reset` |
| `TheEdgeTestsHoldAcrossTheTopOfTheSequenceSpace` | the first, sixth and tenth rows with the edge past 2^32 | as those rows |

Scripts: `window/ack-of-a-segment-refused-at-a-zero-window-is-read.pkt`, `window/bare-fin-at-a-zero-window-is-taken.pkt`.

- [ ] **Step 2: Run them to see them fail**

Run: `ctest --preset debug -R Tcp`
Expected: FAIL: the refused segment's ACK is not read.

- [ ] **Step 3: Implement the outcome, the edge tests and the payload step's refusal**

`on_ack` still receives the segment's real payload length, so an ACK that carries refused data is no duplicate ACK. `timestamps.update` runs for a `NoRoom` segment as for any that passed the check.

- [ ] **Step 4: Run everything**

Run: `ctest --preset debug`, the scripts binary, both check scripts, then `asan` and `tsan`.
Expected: PASS. Existing cases that assert a bare FIN or data is dropped whole at a zero window, with its ACK unread, are derived again; the tap suite's note in `AGENTS.md` on Linux's zero-window probe still holds, since a probe is at RCV.NXT - 1.

- [ ] **Step 5: Commit**

```bash
git add common/tcp tests
git commit -m "fix(tcp): read the ACK of a segment the window refuses, and take a bare FIN at a closed window"
```

---

## Task 7: Resends into the peer's closed window

Spec: "The window's leftovers", third paragraph. Read `tcp_retransmit_timer` (`tcp_timer.c:563-602`) and `tcp_probe_timer` (`:390-439`) first.

**Files:**
- Modify: `common/tcp/connection/detail/zero_window_probe.hpp`, `common/tcp/stack/detail/transmitter.hpp`
- Test: `tests/unit_tests/common/tcp/test_tcp_probes.cpp`, `test_tcp_rto.cpp`, `tests/scenarios/tcp/rto/*.pkt`

**Interfaces:**
- Produces, `ZeroWindowProbe`:

```cpp
/// An expiry of the retransmission timer that found the peer's window zero: counted with the probes, as an
/// unanswered one, with no probe sent and the probes' interval untouched. False when `limit` went unanswered
/// already: the connection is given up.
[[nodiscard]] bool unanswered(std::uint8_t limit) noexcept;

/// Unanswered probes and closed-window expiries since the last acceptable ACK.
[[nodiscard]] std::uint8_t out() const noexcept;
```

  `Transmitter::on_retransmit_timeout`, on a synchronized connection whose `send.wnd` is zero: the user timeout first, as today; then `block.probe.unanswered(config.data_retries)` in place of the `retransmit_count` test, and `retransmit_count` is not incremented; the flag `on_timeout` takes for a run's first expiry is `block.probe.out() == 1`; the marks, the resend of the head, the backoff and the re-arm are as for any expiry. Off a closed window nothing changes.

- [ ] **Step 1: Write the failing tests**

Each test has data in flight and the peer's window at zero: the peer acknowledges nothing new and states a zero window in an ACK at SND.UNA.

| Test | Does | Expects |
|---|---|---|
| `AnsweredResendsIntoAZeroWindowKeepTheConnection` | `data_retries + 5` expiries, the peer answering each resend with an ACK at SND.UNA and window zero | the connection is open, `retransmit_count() == 0`, `retransmits == data_retries + 5` |
| `UnansweredResendsIntoAZeroWindowEndIt` | no answers | `TimedOut` at the expiry after `data_retries` resends, `connections_timed_out == 1` |
| `OneAnswerStartsTheCountAgain` | `data_retries - 1` unanswered, one answer, `data_retries - 1` unanswered | open |
| `TheUserTimeoutEndsItAllTheSame` | `user_timeout = 2 s`, the peer answering | `TimedOut`, `connections_user_timed_out == 1` |
| `AfterTheWindowOpensTheCapStartsFromNothing` | 20 answered closed-window expiries, the window opens with no new ACK, then unanswered expiries | `TimedOut` only after `data_retries` of them |
| `TheBackoffMovesAtAClosedWindow` | three answered closed-window expiries | the intervals between resends double |

A unit test of `ZeroWindowProbe::unanswered` beside the class's existing tests: it counts to `limit`, `answered()` clears it, and it leaves `deadline()` as it was.

Script: `rto/resends-into-a-zero-window-keep-the-connection.pkt`, with `--tcp_data_retries=3` and five answered resends, no SACK-permitted on the peer so that no tail probe comes first.

- [ ] **Step 2: Run them to see them fail**

Run: `ctest --preset debug -R Tcp`
Expected: FAIL: the first test ends `TimedOut`.

- [ ] **Step 3: Implement `unanswered`, `out` and the expiry's branch**

- [ ] **Step 4: Run everything**

Run: `ctest --preset debug`, the scripts binary, both check scripts, then `asan` and `tsan`.
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add common/tcp tests
git commit -m "fix(tcp): resends into a closed window do not end a connection the peer still answers"
```

---

## Task 8: The peer's MSS floor and the SYN payload counter

Spec: "The handshake's leftovers", second and third paragraphs.

**Files:**
- Modify: `common/tcp/config/tcp_config.hpp`, `tcp_counters.hpp`, `common/tcp/stack/detail/receiver.hpp`, `lifecycle.hpp`, `fixtures/scenario/script/script.hpp`, `parse_script.cpp`, `run/harness.cpp`
- Test: `tests/unit_tests/common/tcp/test_tcp_handshake.cpp`, the scenario parser's tests, `tests/scenarios/tcp/handshake/*.pkt`

**Interfaces:**
- Produces: `TcpConfig::min_peer_mss` (`std::uint16_t`, 48), with `inline constexpr std::uint16_t smallest_min_peer_mss = 48;` beside `default_peer_mss`; `validate` throws `"TcpConfig: min_peer_mss must be at least 48 and at most the stack's MSS"` when it is below 48 or above `max_l4_size - wire::TcpHeader::size`. The counters `handshakes_refused_mss` and `syn_payload_discarded`. The script option `--tcp_min_peer_mss`.
- Produces, `Lifecycle`:

```cpp
/// An active open the stack refuses on the peer's SYN-ACK: Closed, with `Reset` raised, as an open the peer
/// refused. Not counted in `connections_reset`, which counts RSTs received.
void refuse_open(ControlBlock& block) noexcept;
```

- [ ] **Step 1: Write the failing tests**

| Test | Does | Expects |
|---|---|---|
| `ASynWhoseMssIsBelowTheFloorIsRefused` | SYNs with MSS 47, 1 and a stated 0 | each: a RST with ACK acknowledging the SYN, no slot (`table_size() == 0`), `handshakes_refused_mss` up by one |
| `ASynAtTheFloorIsAccepted` | MSS 48, timestamps on | SYN-ACK; `mss() == 36` once established |
| `ASynWithoutAnMssOptionIsFiveThirtySix` | no option, default floor | accepted; with `min_peer_mss = 600`, refused |
| `ASynAckWhoseMssIsBelowTheFloorRefusesTheOpen` | `open_active` against a peer whose SYN-ACK states MSS 40 | a RST whose sequence number is the SYN-ACK's acknowledgement number, `Reset` raised, state `Closed`, `handshakes_refused_mss == 1`, `connections_reset == 0`, `connections_opened == 0` |
| `TheFloorsBoundsAreValidated` (in `ConfigValidation`) | 47, and the stack's MSS plus one | `std::invalid_argument`; 48 and the stack's MSS accepted |
| `ASynWithPayloadIsCountedAndItsPayloadNotAcknowledged` | a SYN with 10 bytes | SYN-ACK acknowledges `irs + 1`, `syn_payload_discarded == 1` |

Scripts: `handshake/syn-below-the-mss-floor-is-refused.pkt`, `handshake/syn-ack-below-the-mss-floor-refuses-the-open.pkt` (`read` reports `ECONNREFUSED`), `handshake/syn-payload-is-not-acknowledged.pkt`.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset debug`
Expected: FAIL to compile, `min_peer_mss` undeclared.

- [ ] **Step 3: Implement the floor on both opens, `refuse_open`, the counter and the option line**

The departure from Linux, which raises a small MSS to its floor and goes on (`tcp_output.c:2052-2081`), is commented at both refusals.

- [ ] **Step 4: Run everything**

Run: `ctest --preset debug`, the scripts binary, both check scripts, then `asan` and `tsan`.
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add common/tcp fixtures/scenario tests
git commit -m "feat(tcp): refuse a peer whose MSS is below a floor, and count a SYN's payload"
```

---

## Task 9: The static table, and static as the default

Closes #23. Spec: "Neighbours", from its start to the bullets on a static address, and "Who changes, mechanically". This task flips a default, so its commit carries every mechanical change with it.

**Files:**
- Modify: `common/net/ipv4/arp/arp_cache.{hpp,cpp}`, `common/net/ipv4/stack/ipv4_config.hpp`, `ipv4_counters.hpp`, `ipv4.hpp`, `common/net/ipv4/stack/detail/ipv4_arp.hpp`, `ipv4_output.hpp`, `common/tcp/stack/detail/receiver.hpp`, `transmitter.hpp`, `common/tcp/config/tcp_counters.hpp`, `tests/shared/net/net_fixture.hpp`, `fixtures/scenario/run/harness.cpp`, every test that builds an IP brick, `examples/{ping_responder,tcp_echo,tcp_echo_tasks}`, `benchmarks/tick_to_trade/level_bricks.{hpp,cpp}`, `level_runtime.cpp`, `stack_sanity.hpp`, `tests/unit_tests/benchmarks/test_stack_config.cpp`, `tests/manual_tests/{net,tcp}`
- Test: `tests/unit_tests/common/net/test_net_static.cpp` (new, added to its `CMakeLists.txt`), `test_net_arp.cpp`, `test_net_send.cpp`, `test_net_prebuild.cpp`, `tests/unit_tests/common/tcp/test_tcp_handshake.cpp`

**Interfaces:**
- Produces, `net::Ipv4Config`:

```cpp
/// Neighbours known from the configuration: used without any ARP frame, and never changed by anything.
std::vector<ArpResolution> static_arp{};
/// Resolve neighbours on the wire and keep what is learned until it expires. Off by default: the brick then knows
/// `static_arp` alone, sends no ARP request and learns from no frame.
bool arp_learning = false;
```

  The constructor refuses, each with its own `std::invalid_argument` message: a static address that is not a unicast host on the subnet or is the brick's own; a static MAC that is zero or a group address; a static address listed twice; and, with learning off, a gateway that has no static entry.
- Produces, `net::Ipv4Counters::dropped_arp_static_conflict` and `tcp::TcpCounters::dropped_no_route`.
- Produces, `ArpCache`: the static array, searched first by `lookup`; `Lookup` gains `bool permanent = false;  ///< A static entry: it never ages, and a template built from it never goes stale.`; `[[nodiscard]] bool is_static(wire::Ipv4Address address) const noexcept;`. With learning off `lookup` answers nothing and asks for no request for any other address, and `learn` does nothing. The learned table keeps its windows until Task 10.
- Produces, `Ipv4::next_hop(destination)`: nothing when routing gives no hop, or, with learning off, when the hop is a unicast address with no static entry. `Output`'s resolution fails `NoRoute`, counted in `send_no_route`, in the same case. `Output::prebuild` sets `valid_until` to `core::TimePoint::max()` for a permanent hop.
- Produces, the Receiver and the Transmitter: `passive_open` drops a SYN whose source has no next hop, counted in `dropped_no_route`, before it takes a slot; `reply_reset` sends nothing to a source with no next hop and counts nothing.
- Produces, the examples: after the role and its target and before `--`, any number of `--static <address>=<mac>` and at most one `--learn-arp`. The benchmark: `stack_ip_config(peer, arp_on_wire)`, static venue entry and default lifetimes without `--arp-on-wire`, learning on with the lifetimes of 24 and 48 hours with it.

- [ ] **Step 1: Write the failing tests**

`test_net_static.cpp`, on a fixture like `NetFixture` whose configuration is `{.address = stack_ip, .prefix = 24, .gateway = gateway_ip, .static_arp = {{gateway_ip, gateway_mac}, {harness_ip, harness_mac}}}` with learning off unless a row says otherwise:

| Test | Does | Expects |
|---|---|---|
| `AStaticNeighbourResolvesItsFirstSendWithNoRequest` | `send` to `harness_ip` | the frame leaves to `harness_mac`; `arp_requests_sent == 0` |
| `AStaticNeighbourNeverAges` | sends at the first stamp and at 100 years | resolved both times, no request |
| `ATemplateOfAStaticHopNeverGoesStale` | `prebuild` to `far_ip` | `valid_until == core::TimePoint::max()`, and `generation()` equal before and after every row of this table |
| `AFrameThatContradictsAStaticEntryIsCountedAndChangesNothing` | a reply, a gratuitous ARP and a request for our address, each naming `gateway_ip` at another MAC | `dropped_arp_static_conflict == 3`; sends still go to `gateway_mac`; the request is answered; `resolved()` empty |
| `AFrameThatAgreesWithAStaticEntryTeachesNothing` | a reply naming `gateway_ip` at `gateway_mac` | no counter but `arp_replies_received`; `resolved()` empty |
| `NothingChangesTheStaticTable` | `learn(gateway_ip, other_mac)`, the three frames, lookups at both stamps, with learning off and with it on | sends go to `gateway_mac` throughout |
| `WithLearningOffNoRequestIsEverSent` | `send`, `prebuild` and `resolve` to an on-subnet address outside the table | `NoRoute`, `NoRoute`, nothing; `arp_requests_sent == 0`, `send_no_route == 2`, `send_unresolved == 0` |
| `WithLearningOffAReplyTeachesNothing` | a reply for an address outside the table | `resolved()` empty; a send to it is still `NoRoute` |
| `WithLearningOffARequestForOurAddressIsAnswered` | a request from an address outside the table | one reply to the sender's MAC |
| `WithLearningOffLearnDoesNothing` | `learn(x, mac)`, then `send` to `x` | `NoRoute` |
| `TheRouteQuestionKnowsTheStaticTable` | `next_hop(harness_ip)`, `next_hop(far_ip)`, `next_hop` of an on-subnet stranger, of the subnet broadcast | `harness_ip`, `gateway_ip`, nothing, the broadcast |
| `ABadStaticTableIsRefused` | our own address; an off-subnet address; the subnet broadcast; a zero MAC; a group MAC; an address twice; learning off with a gateway outside the table | `std::invalid_argument` each; the last accepted with learning on |

`test_tcp_handshake.cpp`, on a stack whose IP brick has learning off and a static entry for the harness alone:

| Test | Does | Expects |
|---|---|---|
| `ConnectToANeighbourOutsideTheTableIsNoRouteWithNoSlot` | `connect` to an on-subnet stranger | `ConnectError::NoRoute`, `table_size() == 0`, nothing armed, nothing sent |
| `ASynFromANeighbourOutsideTheTableTakesNoSlot` | a SYN to a listener from a stranger's address | `dropped_no_route == 1`, `table_size() == 0`, nothing sent |
| `AStraySegmentFromANeighbourOutsideTheTableDrawsNoReset` | an ACK to a closed port from a stranger | `dropped_no_connection == 1`, `resets_sent == 0`, `send_refused == 0` |
| `ASynFromOffTheSubnetWithNoGatewayTakesNoSlot` | learning on, no gateway, a SYN from `far_ip` | `dropped_no_route == 1`, no slot |

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset debug`
Expected: FAIL to compile, `static_arp` undeclared.

- [ ] **Step 3: Implement the static array, the switch, the route question, the two TCP checks and the validation**

- [ ] **Step 4: Switch learning on wherever a brick relied on it**

Search: `grep -rnE 'Ipv4Config|net::Ipv4<' tests fixtures examples benchmarks common/runtime --include='*.cpp' --include='*.hpp'`. `stack_config()` in the net fixture and the scenario harness's configuration set `.arp_learning = true` and keep their `learn` seeds; so does every test that builds a brick of its own ("Verified Beforehand" lists the known ones). The tap tests set it too. The examples parse the two new arguments, with a parser that no longer finds the gateway by position alone, and their documented tap command lines gain `--learn-arp`. The benchmark takes `arp_on_wire` into `stack_ip_config`; `dropped_arp_static_conflict` and `dropped_no_route` join the sanity list and `test_stack_config.cpp`'s table.

- [ ] **Step 5: Run everything**

Run: `cmake --preset debug -DDO_BENCHMARKS=ON && cmake --build --preset debug && ctest --preset debug`, the scripts binary, both check scripts, then `asan` and `tsan`.
Expected: PASS with the baseline's count plus the new tests; no existing test changed but for its configuration.

- [ ] **Step 6: Commit**

```bash
git add common fixtures tests examples benchmarks
git commit -m "feat(net): a static ARP table, and no resolution on the wire unless learning is switched on"
```

---

## Task 10: The learned table

Spec: "Neighbours", "With learning on", rules 1 to 3. Read `arp_process` (`net_ipv4_arp.c:843-947`) for what this departs from.

**Files:**
- Modify: `common/net/ipv4/arp/arp_cache.{hpp,cpp}`, `common/net/ipv4/stack/detail/ipv4_arp.hpp`, `ipv4_output.hpp`, `ipv4.hpp`, `ipv4_counters.hpp`, `tests/manual_tests/tcp/test_tcp_tap.cpp`, `benchmarks/tick_to_trade/stack_sanity.hpp`, `tests/unit_tests/benchmarks/test_stack_config.cpp`
- Test: `tests/unit_tests/common/net/test_net_arp_cache.cpp`, `test_net_arp.cpp`

**Interfaces:**
- Consumes: the static array and `arp_learning` (Task 9).
- Produces, `ArpCache`, replacing `learn` and the windows:

```cpp
/// What a frame's or the program's mapping did to the table.
enum class Taught : std::uint8_t {
    Updated,  ///< A learned entry was filled, refreshed or given another MAC.
    Created,  ///< `seed` made a new entry.
    Unknown,  ///< `update`: the table holds no entry for the address; nothing was stored.
    Static,   ///< The address is static and the MAC agrees; nothing changed.
    Conflict, ///< The address is static and the MAC differs; nothing changed.
    Full,     ///< `seed`: no free slot, no expired entry and no lapsed request; nothing was stored.
    Off,      ///< Learning is off; nothing was stored.
};

/// A frame's mapping: fills, refreshes or changes the entry the table holds for `address`, and creates none.
[[nodiscard]] Taught update(wire::Ipv4Address address, wire::MacAddress mac, core::TimePoint now) noexcept;
/// The program's mapping: as `update`, and creates the entry when the table does not hold it.
[[nodiscard]] Taught seed(wire::Ipv4Address address, wire::MacAddress mac, core::TimePoint now) noexcept;
```

  `Lookup` gains `bool full = false;  ///< The address is absent and the table had no slot to give it.` The table is a fixed open-addressing table with linear probing and backward-shift deletion, as `tcp::FlowTable` is, sized for `capacity` entries at no more than half full; `capacity` need only be positive. A new address takes a free slot, or, when `capacity` entries are held, the slot of an expired entry or of an incomplete one whose last request is at least `request_interval` old. A resolved entry that has not expired is never removed.
- Produces, `Ipv4Counters::arp_table_full`, counted for a lookup or a seed that found the table full. `Output`'s resolution fails `Unresolved` for a full table, as for any address not resolved yet. `Arp::receive` calls `update` for a plausible sender in every case where it calls `learn_from_wire` today and reports the mapping in `resolved()` when the answer is `Updated`; `Ipv4::learn` calls `seed`.

- [ ] **Step 1: Write the failing tests**

`test_net_arp_cache.cpp` keeps its cases on a miss, a learn, a stale entry, an expired entry, a bad configuration, `confirmed` and the generation, with `learn` renamed, and loses the four on the eviction order. New, with `capacity = 8`:

| Test | Does | Expects |
|---|---|---|
| `AnUpdateForAnAddressNotHeldStoresNothing` | `update(x, mac)` | `Taught::Unknown`; `contains(x)` false; `size() == 0` |
| `AnUpdateFillsAnEntryALookupMade` | `lookup(x)`, `update(x, mac)` | `Updated`; the next lookup has the MAC |
| `ASeedCreatesAndALaterUpdateChangesIt` | `seed(x, a)`, `update(x, b)` | `Created`, `Updated`; the generation moved twice |
| `AnyFreeSlotServesAnyAddress` | eight addresses that share one home slot | all eight held |
| `AFullTableOfResolvedEntriesRefusesAndKeepsEveryOne` | eight seeded and looked up; a ninth looked up, 100 times | `full` each time, no request asked for, the eight still resolve, `size() == 8` |
| `AFullTableOfWaitingEntriesRefusesUntilOneLapses` | eight lookups never answered; a ninth at the same stamp, then one request interval later | `full`, then held with a request due |
| `AnExpiredEntrysSlotIsUsedAgain` | eight resolved; past `expire` a ninth lookup | held; `size() == 8` |
| `AResolvedEntryIsNeverRemovedBeforeItExpires` | eight resolved at staggered stamps; ninth lookups at every stamp short of the first expiry | `full` each time |
| `RemovingAnEntryKeepsTheChainsOfTheOthers` | fill, expire one in the middle of a probe chain, reuse its slot, look every other address up | all found |
| `WithLearningOffTheTableIsNeverUsed` | `seed` and `update` with learning off | `Taught::Off` |

`test_net_arp.cpp`, with learning on:

| Test | Does | Expects |
|---|---|---|
| `ARequestForOurAddressFromAnUnknownSenderIsAnsweredAndTeachesNothing` (replaces `...TeachesTheSender`) | the request | one reply; `resolved()` empty; a later `send` to the sender asks for it |
| `ARequestFromASenderWeHoldUpdatesIt` | `resolve(sender)`, then its request | `resolved()` reports it; sends go to its MAC |
| `AReplyForAnAddressWeDidNotAskAboutIsDropped` | as `AnUnsolicitedReplyIsDroppedAndTeachesNothing` today | `dropped_arp_unsolicited == 1` |
| `ALookupAtAFullTableIsCountedAndTheSendIsUnresolved` | `arp_capacity = 8`, eight resolved, a ninth send | `Unresolved`, `arp_table_full == 1`, no request |

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset debug`
Expected: FAIL to compile, `ArpCache::update` undeclared.

- [ ] **Step 3: Implement the table, `update`, `seed`, the counter and the callers**

The departures are commented where they are made: RFC 826's merge adds the sender when we are the target, and Linux's `arp_process` creates the entry at once; here a frame creates none. Linux makes room by a forced collection; here a full table refuses.

- [ ] **Step 4: Run everything**

Run: `ctest --preset debug`, the scripts binary, both check scripts, then `asan` and `tsan`.
Expected: PASS. A TCP or runtime test whose stack answered a host only because that host's ARP request had taught it the MAC now sees one ARP exchange first: seed the host, or resolve it, in the test's setup and say so. The tap fixture does the same for the kernel's address, once, before any case connects (spec, "The tap suite under rule 1"); it cannot be run here, so the change is read by the reviewer and the run stays owed. `arp_table_full` joins the benchmark's sanity list and `test_stack_config.cpp`'s table, built with `-DDO_BENCHMARKS=ON`.

- [ ] **Step 5: Commit**

```bash
git add common/net tests
git commit -m "feat(net): a learned ARP table that no frame fills and no address evicts"
```

---

## Task 11: Several queues

Spec: "Neighbours", "Several queues".

**Files:**
- Modify: `common/net/ipv4/stack/ipv4_config.hpp`, `ipv4.hpp`, `common/net/ipv4/stack/detail/ipv4_arp.hpp`, `common/runtime/stack/tcp_runtime_stack.hpp`, and every user of the old name
- Test: `tests/unit_tests/common/net/test_net_arp.cpp`, `tests/unit_tests/common/runtime/test_tcp_runtime.cpp`, `test_tcp_steering.cpp`

**Interfaces:**
- Consumes: `ArpCache::update` (Task 10).
- Produces: `Ipv4Config::report_unsolicited_replies`, the old `accept_unsolicited_replies` renamed, documented `/// With learning on: a reply addressed to us about an address this brick does not hold is reported in `resolved()` and not stored, so that a loop can forward it to the shard that asked.`
- Produces, `net::Ipv4`:

```cpp
/// A resolution another shard reported: fills, refreshes or changes an entry this brick holds, and otherwise does
/// nothing. Reports nothing in `resolved()`, so shards never echo each other.
void apply_resolution(wire::Ipv4Address address, wire::MacAddress mac, core::TimePoint now) noexcept;
```

  `resolved()` reports what it reports today for an entry the brick holds, a refresh included; with the switch on it also reports a reply whose target is our address and whose sender the brick does not hold, without storing it; such a reply is counted in `dropped_arp_unsolicited` only with the switch off. `runtime::TcpStack` sets the switch under its new name on a multi-queue device, and its sink calls `apply_resolution`.

- [ ] **Step 1: Write the failing tests**

| Test | Does | Expects |
|---|---|---|
| `AReplyAboutAnAddressNotHeldIsReportedAndNotStored` (replaces `WithAcceptUnsolicitedRepliesAReplyAddressedToUsIsLearned`) | the switch on; a reply to our address about `x` | `resolved() == {{x, mac}}`; a `send` to `x` still asks |
| `AReplyNotAddressedToUsIsNotReported` | the same with another target address | `resolved()` empty |
| `ARefreshOfAHeldEntryIsReportedAsToday` | `ARefreshLearnedFromTheWireIsReportedAgain` unchanged | passes |
| `AppliedResolutionFillsAnEntryThatWasAskedFor` | `resolve(x)`, `apply_resolution(x, mac)` | the next send goes to `mac`; `resolved()` empty |
| `AppliedResolutionDoesNothingOnABrickThatDidNotAsk` | `apply_resolution(x, mac)` | a `send` to `x` asks for it |
| `AppliedResolutionCannotChangeAStaticEntry` | on a static address with another MAC | sends unchanged |

The runtime's `ArpForwardedOnceToSiblings` and `GatewayKnownOnlyByQueueZero` are derived again: a shard that asked gets the resolution through the forward and resolves; queue 0, which did not ask, stores nothing; a shard that did not ask stores nothing.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset debug`
Expected: FAIL to compile, `apply_resolution` undeclared.

- [ ] **Step 3: Implement the report, the call, the rename and the sink**

- [ ] **Step 4: Run everything**

Run: `ctest --preset debug`, the scripts binary, both check scripts, then `asan` and `tsan`.
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add common tests
git commit -m "feat(runtime): a forwarded ARP resolution is used only by a shard that asked"
```

---

## Task 12: Documentation, the benchmark, the gates

Spec: "Documentation", "The tick path", "Delivery", "Where this departs".

**Files:**
- Modify: `docs/architecture/{tcp,net,core,runtime,scenario}.md`, `docs/roadmap.md`, `README.md`, `docs/guides/getting-started.md`, `docs/benchmarks/tick-to-trade.md` and a result file under `docs/benchmarks/results/`, `AGENTS.md`

- [ ] **Step 1: The public pages**

- `tcp.md`: `TcpConfig`'s and `TcpCounters`' tables and prose with the two settings and the eight counters; the design note on the receive path with the order of "Read This First"; "Hardening is deferred too" rewritten to what is left, SYN cookies and the keyed table hash; the entries of "What is still left out" this closes (the peer's MSS, the blind RST and FIN); the usage section's examples with their new arguments; and in "Where the brick departs from the RFCs and from Linux" every line of the spec's "Where this departs" that is TCP's, each with who decided it.
- `net.md`: the static table and the operating rule; the default, in the words of the spec's paragraph on what learning gives and does not; the learned table's three rules; several queues; the ARP departures; "Deferred" without the rate limit; the key types and configuration.
- `core.md`: the hash and the random bytes. `runtime.md`: the forwarding as it now works, and the cost of a forward under a flood. `scenario.md`: the two option lines and a row per new script. `getting-started.md`: the examples' arguments, with `--learn-arp` in every tap command line.
- `roadmap.md`: step 6 says the hardening has landed and what remains. `README.md`: the sentence on hardening says what is and is not hardened yet.
- `AGENTS.md`: the layout row's description of `core`, `net` and `tcp`; a trap for each thing the build taught.

No tracked file links into `docs/superpowers/`.

- [ ] **Step 2: The benchmark**

Build `release` with `-DDO_BENCHMARKS=ON` for the branch and for `main` at the branch point, as the benchmark page's "Tracking regressions" says, the binaries under `~/.cache/aloe-bench/hardening/`. Run the 64-byte tick at level 0, cross-core, three runs per side, alternating, base first, on hugepages. Record the medians and the result files on the page, whether or not anything moved, with a sentence that the branch's venue entry is static and `main`'s a seed that never refreshes within a run. The bar is no change beyond 10 to 20 ns; a miss is investigated before it is recorded.

- [ ] **Step 3: The gates**

```bash
cmake --preset debug -DDO_BENCHMARKS=ON && cmake --build --preset debug && ctest --preset debug
cmake --preset gcc-debug && cmake --build --preset gcc-debug --clean-first && ctest --preset gcc-debug
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
./scripts/check-format.sh && ./scripts/check-includes.sh
```

Expected: every preset passes. Owed to a machine with root and recorded as owed: the tap suite, `sudo ./build/debug/tests/manual_tests/tcp/Aloe.Tests.Manual.Tcp.Tap`, with the fixture resolving the kernel's address at setup.

- [ ] **Step 4: Commit, and stop before anything leaves the machine**

```bash
git add docs README.md AGENTS.md
git commit -m "docs: hardening, the departures, and ARP that is static by default"
git add -f docs/superpowers/specs/2026-11-28-hardening-design.md docs/superpowers/plans/2026-11-28-hardening.md
git commit -m "docs(tcp): add the hardening spec and plan"
```

The pull request is titled `[TCP][NET][CORE][RUNTIME][FIXTURES] Hardening: keyed sequence numbers, RFC 5961, static ARP by default and the edge cases`; it closes #37 and #23, and a comment on #21 says what landed of step 6 and what is owed. The push, the pull request, the comment and the follow-up issue on forwards under a flood wait for the author's go-ahead.

---

## Self-Review Notes

- **Spec coverage.** "Initial sequence numbers and timestamp offsets": Tasks 1 and 2. "The first checks of a segment": steps 0 and 5 in Task 5, 1 to 3 in Task 3, 4 in Task 4, 2's outcomes and 6 in Task 6. "The handshake's leftovers": Tasks 5 and 8. "The window's leftovers": Tasks 6 and 7. "Neighbours": Tasks 9 to 11. "Where this departs": comments in each task, the pages in Task 12. "Configuration, counters": the names table. "The tick path": Task 12's benchmark. "Fixtures and the scenario runner": Tasks 3, 8 and 9. "Documentation" and "Delivery": Task 12.
- **Left out on purpose.** No tap case (spec, "Testing"). No per-hop generation (spec, "Neighbours"). No limit on RSTs.
- **Names.** `challenge`, `answer_refused`, `AnswerLimiter::allow`, `SendSequence::ack_floor`, `ZeroWindowProbe::unanswered`, `Lifecycle::refuse_open`, `ArpCache::update` and `seed`, `Ipv4::apply_resolution` are used with one spelling throughout.
