# Receive Side Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build roadmap phase 2, step 4 (GitHub issue #19): out-of-order segments held as the packets they arrived in and joined to the in-order chain without a copy, SACK blocks (RFC 2018) and duplicate SACK (RFC 2883) on pure ACKs, window scaling (RFC 7323) with a configured window, receiver silly-window avoidance (RFC 1122 4.2.3.3), Linux's doubling rule for a window update on `consume`, and an optional delayed ACK; with the fixtures, the scenario runner's lines and scripts, the two-stack and tap tests, the benchmark rerun and the docs brought current.

**Architecture:** The control block's receive side becomes five parts that each own their state: `ReceiveQueue` (RCV.NXT and the in-order chain), `ReceiveWindow` (the byte budget, our scale and the offered edge), `ReassemblyQueue` (the out-of-order chain and a remembered FIN), `SackBlocks` (whether SACK is in use, the four blocks and the pending duplicate) and `AckPolicy` (every decision of when an ACK goes). The Receiver's sequence check gets a third outcome, "above a gap, inside the window", whose segment runs the rest of RFC 9293's order and is then held on the reassembly chain; an in-order segment asks one new question, whether anything is held above, and when something is, links the run that now follows RCV.NXT onto the in-order chain. The Transmitter's one send function writes the handshake's options in Linux's layout, the scaled window field rounded up with the reconstructed edge recorded, and the blocks a pure ACK carries. Both chains are nodes of the shard's one `HeldSegmentPool`, so a join takes no node and cannot fail.

**Tech Stack:** C++26 on libstdc++, clang 22 and GCC 16, CMake and Ninja, GoogleTest, the existing vcpkg dependencies, DPDK 26.03 through `Aloe::Dpdk` inside `ethdev` only. No new dependency.

**Spec:** [The receive side: out-of-order segments held as packets, SACK blocks, window scaling, delayed ACK and silly-window avoidance](../specs/2026-11-12-receive-side-design.md). Read the spec, `AGENTS.md` and `codestyle.md` before implementation. Both documents live in the git-ignored `docs/superpowers/`; nothing tracked may link to them, and both are force-added to the branch with the pull request. The spec's "Decisions made in the brainstorm" records who decided what; this plan's own choices are marked "plan's call" where the spec's "Left to the builder" left them open.

## Global Constraints

- "No copy on the receive path, with or without loss, with or without memory pressure." An out-of-order segment is the packet it arrived in; an overlap is trimmed by moving an offset and a length. No task adds a `memcpy`, `std::ranges::copy` or a fresh packet on any receive path.
- "A SACK block that reports data the connection does not hold" is forbidden. The number of a remembered FIN is not data and may be reported. A block is cut back in the same step that releases its packet.
- "An offered right edge that moves left, as the peer reconstructs it from the scaled field" is forbidden. The connection records the reconstructed edge, never the computed one.
- "Out-of-order data that can pin more packets than the cap and the pool allow, or that survives the connection" is forbidden. Both chains count against `receive_segments`; a terminal event returns the out-of-order packets at once.
- Hot paths: every member of `tcp::Stack` and `tcp::Connection` reached after construction is `noexcept`, allocates nothing but packets from IP's pool and nodes from the fixed pools, logs nothing, reads no clock. Errors on hot paths are `std::expected`; constructors throw `std::invalid_argument`.
- The values the spec fixes, verbatim: `receive_segments` 512; `receive_window` 0 (half of what the slots hold); `receive_pool` 2048; `sack` true; `window_scaling` true; `delayed_ack` 0 (off), at most 500 ms; four SACK blocks kept (Linux's `TCP_NUM_SACKS`), three sent beside timestamps and four without; the largest shift 14; the shift is the smallest that states the largest budget, 3 with the defaults; the silly-window threshold `min(budget / 2, mss())`; a `consume` sends an update only when the window it could offer is at least twice the one on offer; the first three segments above a gap since RCV.NXT last moved are answered at once on a connection that uses SACK and every one on a connection that does not; a full-sized segment on the receive side is `mss()`.
- The handshake's options, in Linux's layout: with timestamps `mss, sackOK, TS val ecr, nop, wscale` (20 bytes), without `mss, nop, nop, sackOK, nop, wscale` (12 bytes); an absent `sackOK` beside timestamps leaves `nop, nop`. A pure ACK carries `nop, nop, TS` then `nop, nop, sack` and eight bytes per block. A data segment never carries blocks.
- What happens when a connection is at its packet cap is the spec's "Room" rule and nothing more: the lower sequence wins, one node for one segment, and with nothing held out of order an in-order segment is dropped as today. Whether to close the window or copy at the cap is issue #39, held by the author; Task 6 keeps the rule in one function (`Receiver::make_room`) so that decision changes one place.
- Scripts are Aloe's own, under `tests/scenarios/tcp/<area>/<name>.pkt`; each opens with a comment stating what it pins and where Linux differs. Every number in a script is derived from the rules under "Read This First", never pasted from a failing run.
- Every test runs without root, hugepages or a card; the tap cases and the benchmark are manual.
- `AGENTS.md` rules apply unchanged: `tcp` links neither `execution` nor `log`; `core::TimePoint` and `core::Duration` for every stamp and interval; one class in one place; a header whose names are all in `detail` sits in the `detail/` folder of the folder it belongs to; public headers define no macros; another module's names qualified with its namespace; test helpers in `aloe::testing`; clang-tidy clean inside the clang builds; the include rules of `./scripts/check-includes.sh`.
- `codestyle.md` applies: by-value parameters `const` in definitions and never in bodiless declarations; `[[nodiscard]]` on every value-returning function; a default member initialiser on every struct member; no integer literal suffixes beyond `U`; concepts over SFINAE; a class that models a concept asserts it at the bottom of its header.
- Completion gates: `debug`, `gcc-debug`, `asan` and `tsan` build and pass; `./scripts/check-format.sh` and `./scripts/check-includes.sh` pass; `-DDO_BENCHMARKS=ON` builds and its tests pass; the tap cases and the benchmark rerun are run by hand or recorded as owed.
- Conventional commit subjects with the module as scope: `feat(wire)`, `feat(tcp)`, `feat(frames)`, `feat(scenario)`, `test(tcp)`, `bench(tick-to-trade)`, `docs:`.

## Verified Beforehand

Nothing in this plan was built; the author asks for plans without a prototype. These facts were read from the code on `main` at 6a779fb on 2026-11-12, and from the Linux kernel at torvalds/linux 9d66f26 (7.4-rc2) the same day (`net/ipv4/tcp_input.c`, `tcp_output.c`, `tcp.c`, `include/net/tcp.h`).

| Fact | Where it matters |
|---|---|
| `Receiver::accept_sequence` (`common/tcp/stack/detail/receiver.hpp:298-347`) returns `bool`. A segment with `length() == 0` is acceptable at RCV.NXT, or above it inside the window, and is not answered. `sequence.after(next)` with length calls `block.ack_policy.gap(end)` and drops with `dropped_out_of_order` and an immediate ACK. An overlap is trimmed by offset and `header.sequence = next`. `window == 0 \|\| end.after(edge)` drops with `dropped_out_of_window`. | Tasks 3, 6 |
| `Receiver::receive_synchronized` (`:241-290`) runs PAWS, the sequence check, `timestamps.update(ts, arrived, now)`, RST, SYN, `receive_ack`, `receive_payload`, the FIN. `receive_payload` (`:397-425`) refuses with `ack_now` past the peer's FIN, at `receive.segments() >= config.receive_segments` (`dropped_no_slot`) and when `ReceiveQueue::hold` finds no node (`dropped_no_node`); then raises `Readable` and asks `ack_policy.on_data(payload_length, context_.mss - timestamps.option_size(), next)`. | Tasks 3, 6, 8 |
| Both opens open the receive side too early for `mss()`. `passive_open` (`:187-192`) calls `receive.open(seq + 1, context_.byte_budget)` before `send.peer_mss` and `timestamps.answer`. `receive_syn_sent` (`:220-228`) calls `timestamps.answer`, `handshake_acknowledged`, `receive.open`, then stores `peer_mss`, `send.synchronize(header)` and only then `lifecycle_.connected(block)`. `ControlBlock::mss()` (`control_block.hpp:83-93`) is `min(send.peer_mss, local_mss)` less 12 when `timestamps.enabled() && state != State::SynSent`. | Task 3 |
| `ReceiveQueue` (`connection/detail/receive_queue.hpp`) owns `next_`, `offered_`, `unread_`, `chain_`, `closed_`: `open(next, budget)`, `offered()`, `window()`, `hold(pool, packet, offset, length)`, `take_fin()`, `consume(pool, count)`, `offer(budget, cap)` (the candidate when it is later than the offered edge and `chain_.count < cap`), `record_offer(edge)`, `release(pool)`, `view(pool)`. | Tasks 3, 6 |
| `HeldSegment{packet, offset, length, next}` on `NodePool<N>` (`pool/detail/node_pool.hpp`): `acquire`, `release(index)`, `push_back(chain, index)`, `release_front(chain)`, `release_back(chain, previous)`, `release_all(chain)`, `node(index)`, `available()`; `NodeChain{head, tail, count}`; `no_node`. The chain is singly linked. | Task 5 |
| `AckPolicy` (`connection/detail/ack_policy.hpp`): `enum Ack { Delayed, Now }` where `Delayed` means "by the next flush"; `gap(end)` remembers the furthest refused end; `on_data(accepted, full_size, next)` returns `Now` until RCV.NXT reaches that end and for the second full-sized segment; `acknowledged()` clears the count. `Lifecycle::peer_closed` returns `AckPolicy::Ack`. | Tasks 7, 8 |
| `Transmitter` (`stack/detail/transmitter.hpp`): `emit(block, packet, framed, sequence, flags, handshake)` writes `window = offer.value - next.value` cast to 16 bits for a segment with ACK and `context_.byte_budget` for one without; `record_offer` counts `window_updates`, removes the block from the ACK list and calls `ack_policy.acknowledged()`; `send_bare_ack` is the pure ACK and the probe; `send_reset` (`:247-265`) builds its own `OutgoingSegment` and casts the window itself; `offer_of(block)` is `block.receive.offer(context_.byte_budget, config.receive_segments)`. `Stack::flush` walks the ACK list through `ack_front` and `ack_next`. | Tasks 3, 4, 7, 8 |
| `OutgoingSegment{header, mss, timestamps}` with `size()`; `SegmentWriter::write` writes the MSS then `nop, nop, TS` (`stack/detail/segment_writer.hpp`). `tcp::Stack` asserts `net::Prebuilt::size + TcpHeader::size + mss_size + timestamps_size <= device::packet_headroom`; `Prebuilt::size` is 34, `packet_headroom` 128, `TcpHeader::max_size` 60. | Tasks 4, 7 |
| `AckProcessor::on_ack` (`stack/detail/ack_processor.hpp:69-71`) calls an ACK a duplicate when it has no payload, SYN or FIN, `ack == send.una`, `header.window == send.wnd` and something is in flight. `SendSequence::synchronize(header)` and `update_window(header)` take `header.window` as bytes; `wnd` is 32 bits. | Task 4 |
| `ControlBlock` has one `TimerNode timer` built from the stack's trampoline (`Stack::on_timer_fired`, which calls `lifecycle_.on_timer`), and `clear()` asserts it is not armed. `StackContext` builds the blocks and holds `mss`, `byte_budget = min(receive_segments * mss, 65535)`, `pool`. `Lifecycle::set_closed` disarms, forgets and drops the queue; `free_slot` calls `return_chain`. | Tasks 3, 6, 8 |
| `wire::TcpOptions::parse` reads kinds 2 and 8 and skips every other kind by its length byte; `timestamp_at` is where kind 8 starts. `write_mss`, `write_timestamps`. `wire::TcpSequence{value}` has `+ uint32_t`, `- TcpSequence` (signed distance), `before`, `after`, `==`. | Task 1 |
| `frames::TcpPeer` (`fixtures/frames/frames/tcp_peer.hpp`): `segment(flags, payload, sequence, acknowledgement, with_mss)` builds the option block as `mss` then `timestamps_option` when `set_timestamps` is engaged; `see(frame)` tracks `stack_window_` (the raw field) and `stack_mss_`; `set_window(field)`. `frames::tcp_segment` asserts `options.empty() \|\| !mss` and that option bytes are a multiple of four within 40. | Tasks 1, 4 |
| The scenario runner: `ScriptTcpOptions` (`fixtures/scenario/script/script.hpp:109-120`) and `apply_option` (`parse_script.cpp`) carry `--tcp_rto_min_ms`, `--tcp_rto_max_ms`, `--tcp_rto_initial_ms`, `--tcp_user_timeout_ms`, `--tcp_retries`, `--tcp_data_retries`, `--tcp_retransmit_defers`, `--tcp_send_segments`, `--tcp_timestamps`; `configured(base, script)` (`run/harness.cpp`) applies them. The parser, `encode_options`, `describe_options` and `match` already handle `sackOK`, `wscale n` and `sack a:b c:d`; an outbound line's blocks are relative to the peer's ISN. A script's `win` is the raw 16-bit field in both directions. `HarnessConfig::pool_size` is 256. | Tasks 2, 3, 4 |
| The runner flushes after every inbound line: it "injects or calls, then drains, flushes and collects at the same stamp" (`docs/architecture/scenario.md`, "Time and matching"). Two inbound lines are never one `process` call, so a script cannot show three ACKs at once and one at the flush; that is a brick test. | Task 7 |
| 40 of the 42 scripts show `46720`; three scripts open actively (`handshake/active-open`, and two more found with `grep -l connect tests/scenarios -r`). `tests/shared/tcp/tcp_fixture.hpp` defines `tcp_fabric_budget = 32U * tcp_fabric_mss`, read in 58 places. `TcpFixture`'s stack port has 64 packets, the two-stack fixture's ports 256, the tap port 1024. | Tasks 3, 4 |
| `benchmarks/tick_to_trade/venue_peer.cpp` is a `frames::TcpPeer` (`script_`) with `set_timestamps(++ts_)`; `stack_sanity.hpp` expects every `dropped_*` counter to be zero, `dropped_out_of_order` among them. | Task 11 |
| Linux, `__tcp_ack_snd_check` (`tcp_input.c:6169-6245`): with data in the out-of-order queue, `if (!tcp_is_sack(tp) \|\| tp->compressed_ack >= sysctl_tcp_comp_sack_nr) goto send_now;` then `if (tp->compressed_ack_rcv_nxt != tp->rcv_nxt) { ...; tp->dup_ack_counter = 0; } if (tp->dup_ack_counter < TCP_FASTRETRANS_THRESH) { tp->dup_ack_counter++; goto send_now; }`. `TCP_FASTRETRANS_THRESH` is 3. | Task 7 |
| Linux, `tcp_data_queue` (`tcp_input.c:5644`) sets `tp->rx_opt.dsack = 0` on entry. `tcp_data_queue_ofo` (`:5416-5560`): a newcomer inside a held buffer is dropped with `tcp_dsack_set(seq, end_seq)`; one that starts at a held buffer's start and covers it replaces it with `tcp_dsack_extend` over the held range; `merge_right` removes every later buffer the newcomer covers and stops at one it covers in part. `tcp_ofo_queue` (`:5339-5372`) reports with `tcp_dsack_extend` the part of a held buffer that RCV.NXT had passed. | Tasks 5, 6, 7 |
| Linux, `tcp_select_window` (`tcp_output.c:275-330`): `if (new_win < cur_win) new_win = ALIGN(cur_win, 1 << rcv_wscale)` ("Never shrink the offered window"), then `rcv_wnd = new_win; rcv_wup = rcv_nxt` and the field is `new_win >> rcv_wscale`. `__tcp_select_window` (`:3378`): with scaling, `window = ALIGN(free_space, 1 << rcv_wscale)`. Both round up, so the recorded edge creeps as the spec says. `TCP_MAX_WSCALE` is 14. | Tasks 3, 4 |
| Linux, `tcp_options_write` (`tcp_output.c:711`): the MSS; then with timestamps `sackOK, TS` in one word when SACK is advertised, else `nop, nop, TS`; a SACK-permitted without timestamps as `nop, nop, sackOK`; the window scale as `nop, wscale`. `tcp_established_options` (`:1209`): `eff_sacks = num_sacks + dsack`, as many blocks as the remaining option space holds. | Tasks 4, 7 |
| Linux, `__tcp_cleanup_rbuf` (`tcp.c:1562`): `if (2 * rcv_window_now <= tp->window_clamp) { new_window = __tcp_select_window(sk); if (new_window && new_window >= 2 * rcv_window_now) time_to_ack = true; }`. | Task 3 |

## Read This First

Baseline commands, on a checkout of `main` at or after 6a779fb:

```bash
git switch -c tcp/receive-side
cmake --preset debug && cmake --build --preset debug && ctest --preset debug      # 60 tests pass today
./scripts/check-format.sh && ./scripts/check-includes.sh
./build/debug/tests/unit_tests/scenarios/Aloe.Tests.Unit.Scenarios               # 150 script runs pass today
```

**The window arithmetic every script and test number comes from.** `ReceiveWindow` (Task 3) is the only place it is written; this is its statement.

```
held       = receive_segments * mss()                     what the slots hold, in bytes
budget     = min(receive_window != 0 ? receive_window : held / 2, held, 65535 << shift)
shift      = the stack's shift when both ends offered window scaling, else 0
unit       = 1 << shift
threshold  = max(1, min(budget / 2, mss()))
edge       = RCV.NXT + min(budget, 65535) when the handshake ends (the SYN-ACK's unscaled field)
candidate  = RCV.NXT + (budget > unread ? budget - unread : 0)
target     = candidate when a slot is free and candidate is at least `threshold` beyond edge, else edge
field      = min(65535, (target - RCV.NXT + unit - 1) >> shift)
edge       = RCV.NXT + (field << shift)                   recorded when the segment leaves
```

- The stack's shift is the smallest `s` in 0..14 with `65535 << s` at least the largest budget the configuration allows, which is computed with the stack's own MSS and no timestamps. With the defaults that budget is `512 * 1460 / 2 = 373760` and the shift is 3.
- With the defaults and a peer that offers window scaling: `mss()` is 1448 with timestamps, the budget `512 * 1448 / 2 = 370688`, and the first ACK after the handshake states `370688 >> 3 = 46336`. Without timestamps the budget is 373760 and the field 46720.
- With the defaults and a peer that does not offer window scaling: the budget is 65535 and the field counts bytes.
- A SYN states `min(largest budget, 65535)`, 65535 with the defaults; a SYN-ACK states `min(budget, 65535)`. Neither is scaled.
- An ACK sent at once for a duplicate, a segment above a gap or a refused segment uses `target = edge`: it restates the edge.
- A `consume` queues an update only when a slot is free, the candidate is at least `threshold` beyond the edge, and `candidate - RCV.NXT >= 2 * (edge - RCV.NXT)`.

**How the existing scripts' numbers move, by task.** Task 3 (no scaling yet): the stack's SYN and SYN-ACK state `win 65535`; every later `win` is `65535` less the unread bytes while the edge stands, and the edge moves only by the rule above; a `read` no longer produces a window-update line unless it doubles the window. Task 4: a SYN the stack sends gains `sackOK` and `nop,wscale 3` in the layout of "Global Constraints"; a SYN-ACK echoes what the script's SYN offered, which in every existing script is neither, so only the three active-open scripts change. Scripts that pin `--tcp_receive_segments` compute their budget from it.

**The order of the work, and why.** The configuration and the runner's lines come first so every later task has the names. The window lands before scaling (Task 3 then Task 4) so the 40 scripts' numbers are re-derived once, against an unscaled window, and only the active-open scripts change again. The reassembly queue is a part with its own tests (Task 5) before the receive path uses it (Task 6). In Task 6 every segment above a gap is answered at once, which is the final rule for a connection without SACK; Task 7 adds the blocks and, with them, the three-then-flush rule for a connection that uses SACK.

Code-base traps that bite this plan in particular; the full list is in `AGENTS.md`:

- A member of a class template that is declared but never defined fails at link time only once something instantiates it; `tcp::Stack::process` reaches the whole receive path, so every function a task declares is defined in the same task.
- After a change to the layout of a struct the scenario binaries compile in (`TcpConfig`, `TcpCounters`, `ControlBlock`), an incremental `gcc-debug` build has left stale objects before. If `Aloe.Tests.Unit.Scenario` or `Aloe.Tests.Unit.Scenarios` aborts after such a change, rebuild with `cmake --build --preset gcc-debug --clean-first` before chasing it.
- `TcpFixture`'s stack port holds 64 packets and the scenario harness's 256, both below the new default cap of 512. A test or script of the cap pins `receive_segments` (`configure_tcp({.receive_segments = 4})`, `--tcp_receive_segments=4`); none grows a pool.
- `TcpConfig::validate` gains `receive_segments <= receive_pool`: a test that shrinks `receive_pool` pins `receive_segments` too.
- A `frames::TcpPeer` offers no timestamps until `set_timestamps(value)`, and after Task 4 no window scaling or SACK until `set_window_scale` and `set_sack_permitted`: without them the connection's `mss()` is 1460 and its budget 65535.
- With timestamps on, a full segment carries 1448 bytes; a test of full-sized segments reads `mss()`.
- A `loop::TimerWheel` advanced once past several deadlines fires a re-arming timer once; the delayed-ACK tests step one millisecond per `advance`.
- A local class cannot declare member templates, and GCC rejects `for (const auto [a, b] : {...})`; write `const auto& [a, b]`.
- clang-tidy's `cert-msc51-cpp` fires on a fixed random seed: the randomized tests keep theirs with a one-line `NOLINT` and its reason. `std::uniform_real_distribution` differs between libstdc++ and libc++; the randomized tests draw integers with `std::uniform_int_distribution`, whose results they do not compare across libraries.
- clang-tidy's `readability-convert-member-functions-to-static` fires on a member that never touches `this`; `ReceiveWindow::budget_of` and `shift_for` are `static`.
- A gtest filter: `-` starts the excluded patterns, so a script name with a dash is matched with `?`.
- A script whose lines all carry `+0` cannot tell a segment sent early from one sent on time; the delayed-ACK scripts give the expected ACK a later stamp.

## How to Execute and Review

Use superpowers:subagent-driven-development (preferred) or superpowers:executing-plans. One task at a time, in order; each ends with every listed test passing on `debug` and a commit. Before writing a part that the spec ties to Linux, read the Linux function the task names (the table above gives the file and what was relied on) and stop if it no longer says what the table says.

Per task, a fresh reviewer checks:

1. **Spec conformance.** The task's interfaces match this plan's Interfaces blocks exactly; the behaviour matches the spec sections the task cites; the values of "Global Constraints" appear verbatim; a departure from Linux is one the spec names.
2. **The anti-goals.** No copy on a receive path; no block that reports data not held; no edge that moves left; both chains inside the cap and returned at a terminal event; nothing logs, allocates beyond the pools or reads a clock.
3. **Owned state.** Each new field sits in the part that owns its concern, with the operations on it; the Receiver, the Transmitter and the Lifecycle gain procedure and no fields; no part reaches into another's members.
4. **Repository rules.** `AGENTS.md` Rules and `codestyle.md`, and the include rules.
5. **Tests.** Each test pins a behaviour that could regress; none needs root, hugepages or a card; `ctest --preset debug` passes with the new tests; the scripts binary passes in every mode and policy it registers; every number in a changed script is the one the arithmetic above gives.
6. **Build.** `cmake --build --preset debug` is warning-free (clang-tidy runs inside it); `./scripts/check-format.sh` and `./scripts/check-includes.sh` pass; Tasks 3, 6 and 7 also run `gcc-debug`, `asan` and `tsan` before their commit, since they change what every TCP test does.

At the end, a whole-branch review against the spec's "Goals", "Anti-goals" and "Testing", then the completion gates of Task 12, then the pull request titled `[TCP][WIRE][FRAMES][SCENARIO] Receive side: out-of-order segments held as packets, SACK blocks, window scaling and delayed ACK`.

## Review Focus

Five conditions the spec implies that are most likely to bite, each with its pin in the task that owns the code:

1. **A sequence space that wraps inside a held run.** A reassembly chain, a block list and an offered edge that cross 2^32, where a comparison by raw value sorts wrongly. Pinned by `WrapsAcrossTheTopOfTheSequenceSpace` in Tasks 3, 5 and 7.
2. **An active open's budget.** A budget computed while the state is still SynSent, or before the peer's MSS is stored, counts 1460 or 536 bytes per slot. Pinned by `AnActiveOpenTakesItsBudgetAfterTheHandshake` in Task 3.
3. **The peer's pure ACKs during a hole.** They arrive above RCV.NXT, carry no data, and must be processed and not answered; an answer is a duplicate ACK for a segment the peer never sent. Pinned by `APureAckAboveRcvNxtIsProcessedAndNotAnswered` in Task 6.
4. **A connection that ends while it holds out-of-order packets or a pending delayed ACK.** Neither may outlive it: no node left out of the pool, no armed timer on a freed block. Pinned by `ATerminalEventReturnsTheOutOfOrderPackets` and `ReleaseReturnsBothChains` in Task 6 and `ReleaseWithADelayedAckPendingCancelsTheTimer` in Task 8.
5. **An ACK with blocks that the device refuses.** The report must still be owed: the duplicate stays pending and the blocks go with the ACK the flush retries. Pinned by `ARefusedAckKeepsItsReportForTheFlush` in Task 7.

---

## File Structure

| File | Responsibility |
|---|---|
| `common/wire/tcp/tcp_options.hpp` | `TcpSackBlock`; `TcpOptions` reads kinds 3 and 4 and writes the window scale, SACK-permitted and a SACK option. |
| `common/tcp/config/tcp_config.hpp`, `tcp_counters.hpp` | `receive_window`, `sack`, `window_scaling`, `delayed_ack`, the new default cap and their validation; the five new counters. |
| `common/tcp/connection/detail/receive_window.hpp` (new) | `ReceiveWindow`: the budget, our scale, the offered edge, the offer with its three conditions and its rounding, the consume rule. |
| `common/tcp/connection/detail/receive_queue.hpp` | `ReceiveQueue` gives up the offered edge and gains `append` for a joined run. |
| `common/tcp/pool/detail/node_pool.hpp`, `held_segment_pool.hpp` | `insert_after`, `unlink_after`, `splice_back`; `HeldSegment::first`. |
| `common/tcp/connection/detail/reassembly_queue.hpp` (new) | `ReassemblyQueue`: the out-of-order chain, the three insert rules, the join's run, the highest node, the remembered FIN. |
| `common/tcp/connection/detail/sack_blocks.hpp` (new) | `SackBlocks`: whether SACK is in use, the four blocks in report order, the pending duplicate. |
| `common/tcp/connection/detail/ack_policy.hpp` | `AckPolicy`: at once, at the flush or after the delay; the count above a gap; the bytes a delayed ACK covers. |
| `common/tcp/connection/detail/send_sequence.hpp` | The peer's shift, applied wherever a window field is read. |
| `common/tcp/connection/detail/control_block.hpp` | The three new parts, the ACK timer, `held_segments()`. |
| `common/tcp/stack/detail/stack_context.hpp` | The stack's shift, the largest budget, the SYN's window. |
| `common/tcp/stack/detail/segment_writer.hpp` | `OutgoingSegment` and the writer learn the handshake layout and the SACK option. |
| `common/tcp/stack/detail/transmitter.hpp` | The offer or the restated edge per segment, the scaled field, the blocks on a pure ACK, the ACK timer's arming. |
| `common/tcp/stack/detail/receiver.hpp` | The three-way sequence check, the above-a-gap path, the join, `make_room`, the reordered opens, the consume rule. |
| `common/tcp/stack/detail/lifecycle.hpp`, `ack_processor.hpp`, `tcp_stack.hpp`, `connection/tcp_connection.hpp` | The offers at `open`, the out-of-order chain returned at a terminal event; the duplicate test on the scaled window; the ACK timer's trampoline and the headroom assertion; the new accessors. |
| `common/tcp/CMakeLists.txt`, `common/tcp/export/aloe/tcp` | The three new headers. |
| `fixtures/frames/frames/tcp_frames.hpp`, `tcp_peer.hpp` | Option builders, `sack_blocks_of`; `TcpPeer` offers both options and reads the stack's. |
| `fixtures/scenario/script/script.hpp`, `parse_script.cpp`, `run/harness.cpp` | The five new `--tcp_*` lines. |
| `tests/unit_tests/common/tcp/test_tcp_{receive_window,reassembly,sack_blocks,ack_policy,sack}.cpp` (new) and the existing files | The parts' tests and the brick's. |
| `tests/scenarios/tcp/{reassembly,sack,ack}/*.pkt` (new areas) and every existing script | The scripts. |
| `tests/manual_tests/tcp/test_tcp_tap.cpp`, `benchmarks/tick_to_trade/venue_peer.cpp`, `stack_sanity.hpp` | The tap cases; the venue's options and the sanity counters. |
| `docs/architecture/{tcp,scenario,fixtures}.md`, `docs/roadmap.md`, `docs/benchmarks/tick-to-trade.md`, `AGENTS.md` | The documentation. |

---

## Task 1: The wire options: window scale, SACK-permitted and SACK blocks

Nothing changes in the brick: the parser reads two more kinds, which the Receiver does not look at yet, and the writers exist for Tasks 4 and 7.

**Files:**
- Modify: `common/wire/tcp/tcp_options.hpp`
- Modify: `fixtures/frames/frames/tcp_frames.hpp`
- Test: `tests/unit_tests/common/wire/test_wire_tcp.cpp`, `tests/unit_tests/fixtures/frames/test_tcp_frames.cpp`

**Interfaces:**
- Produces, `common/wire/tcp/tcp_options.hpp`, namespace `aloe::wire`:

```cpp
/// One SACK block (RFC 2018): the sequence of its first byte and the sequence after its last.
struct TcpSackBlock {
    TcpSequence first{};
    TcpSequence end{};
    friend constexpr bool operator==(const TcpSackBlock&, const TcpSackBlock&) noexcept = default;
};

struct TcpOptions {
    static constexpr std::size_t mss_size            = 4;
    static constexpr std::size_t timestamps_size     = 12;
    static constexpr std::size_t window_scale_size   = 4;   ///< The aligned block: nop, kind 3, length 3, shift.
    static constexpr std::size_t sack_permitted_size = 4;   ///< The aligned block: nop, nop, kind 4, length 2.
    static constexpr std::size_t sack_block_size     = 8;
    static constexpr std::size_t max_sack_blocks     = 4;   ///< Linux's TCP_NUM_SACKS.
    static constexpr std::uint8_t max_window_scale   = 14;  ///< RFC 7323; Linux's TCP_MAX_WSCALE.

    std::optional<std::uint16_t> mss;
    std::optional<std::uint32_t> timestamp_value;
    std::optional<std::uint32_t> timestamp_echo;
    std::optional<std::size_t> timestamp_at;
    std::optional<std::uint8_t> window_scale;  ///< Kind 3 as written, above 14 included; it means something only on a SYN.
    bool sack_permitted = false;               ///< Kind 4; it means something only on a SYN.

    [[nodiscard]] static constexpr std::optional<TcpOptions> parse(std::span<const std::byte> options) noexcept;
    static constexpr void write_mss(std::span<std::byte> out, std::uint16_t mss) noexcept;
    static constexpr void write_timestamps(std::span<std::byte> out, std::uint32_t value, std::uint32_t echo) noexcept;
    /// The twelve bytes Linux writes when it offers both: kind 4, length 2, kind 8, length 10, value, echo.
    static constexpr void write_sack_permitted_timestamps(std::span<std::byte> out, std::uint32_t value, std::uint32_t echo) noexcept;
    static constexpr void write_sack_permitted(std::span<std::byte> out) noexcept;                  // 1, 1, 4, 2
    static constexpr void write_window_scale(std::span<std::byte> out, std::uint8_t shift) noexcept;  // 1, 3, 3, shift
    /// The bytes a SACK option of `blocks` blocks takes: nop, nop, kind 5, length, then eight per block.
    [[nodiscard]] static constexpr std::size_t sack_size(std::size_t blocks) noexcept;              // 4 + 8 * blocks
    /// One to four blocks, in the order given.
    static constexpr void write_sack(std::span<std::byte> out, std::span<const TcpSackBlock> blocks) noexcept;
};
```

  `parse` refuses kind 3 whose length is not 3 and kind 4 whose length is not 2, as it refuses a wrong MSS or timestamp length; kind 5 stays skipped by its length. The two new kinds are tested after kinds 2 and 8 in the chain, so the walk over the usual `nop, nop, TS` is unchanged. The class comment names the new kinds.

- Produces, `fixtures/frames/frames/tcp_frames.hpp`, namespace `aloe::frames`, each a `std::vector<std::byte>` over the wire writer of the same name: `window_scale_option(std::uint8_t shift)`, `sack_permitted_option()`, `sack_permitted_timestamps_option(std::uint32_t value, std::uint32_t echo)`, `sack_option(std::span<const wire::TcpSackBlock> blocks)`; and

```cpp
/// The SACK blocks of a parsed frame, in the order written; empty when it carries none or is not TCP.
[[nodiscard]] inline std::vector<wire::TcpSackBlock> sack_blocks_of(const ParsedFrame& frame);
```

  `sack_blocks_of` walks `tcp_options_of(frame)` by kind and length as `TcpOptions::parse` does; the brick's parser does not decode blocks until step 5.

- [ ] **Step 1: Write the failing tests**

In `test_wire_tcp.cpp`, with a local `bytes(std::initializer_list<int>)` helper if the file has none:

```cpp
TEST(TcpOptions, ParsesWindowScaleAndSackPermitted) {
    // A Linux SYN: mss 1460, sackOK, TS val 100 ecr 0, nop, wscale 7.
    const auto options = bytes({2, 4, 0x05, 0xb4, 4, 2, 8, 10, 0, 0, 0, 100, 0, 0, 0, 0, 1, 3, 3, 7});
    const auto parsed  = TcpOptions::parse(options);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->mss, 1460);
    EXPECT_TRUE(parsed->sack_permitted);
    EXPECT_EQ(parsed->timestamp_value, 100U);
    EXPECT_EQ(parsed->timestamp_at, 6U);
    EXPECT_EQ(parsed->window_scale, 7);
}
TEST(TcpOptions, RejectsAWrongLengthForWindowScaleAndSackPermitted) {
    EXPECT_FALSE(TcpOptions::parse(bytes({3, 4, 7, 0})).has_value());
    EXPECT_FALSE(TcpOptions::parse(bytes({4, 3, 0, 0})).has_value());
}
TEST(TcpOptions, AShiftAboveFourteenIsReportedAsWritten) {
    EXPECT_EQ(TcpOptions::parse(bytes({1, 3, 3, 15}))->window_scale, 15);
}
TEST(TcpOptions, SackBlocksAreSkippedByTheirLength) {
    const auto parsed = TcpOptions::parse(bytes({1, 1, 5, 10, 0, 0, 0, 1, 0, 0, 0, 2}));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->sack_permitted);
    EXPECT_FALSE(parsed->window_scale.has_value());
}
TEST(TcpOptions, WritesTheHandshakeBlocksInLinuxLayout) {
    std::array<std::byte, 20> with{};
    TcpOptions::write_mss(with, 1460);
    TcpOptions::write_sack_permitted_timestamps(std::span{with}.subspan(4), 100, 0);
    TcpOptions::write_window_scale(std::span{with}.subspan(16), 3);
    EXPECT_TRUE(std::ranges::equal(with, bytes({2, 4, 0x05, 0xb4, 4, 2, 8, 10, 0, 0, 0, 100, 0, 0, 0, 0, 1, 3, 3, 3})));
    std::array<std::byte, 12> without{};
    TcpOptions::write_mss(without, 1460);
    TcpOptions::write_sack_permitted(std::span{without}.subspan(4));
    TcpOptions::write_window_scale(std::span{without}.subspan(8), 3);
    EXPECT_TRUE(std::ranges::equal(without, bytes({2, 4, 0x05, 0xb4, 1, 1, 4, 2, 1, 3, 3, 3})));
}
TEST(TcpOptions, WritesOneToFourSackBlocks) {
    EXPECT_EQ(TcpOptions::sack_size(1), 12U);
    EXPECT_EQ(TcpOptions::sack_size(4), 36U);
    const std::array blocks{TcpSackBlock{TcpSequence{0x01020304}, TcpSequence{0x01020404}},
                            TcpSackBlock{TcpSequence{0xfffffff0}, TcpSequence{0x00000010}}};
    std::array<std::byte, 20> out{};
    TcpOptions::write_sack(out, blocks);
    EXPECT_TRUE(std::ranges::equal(
        out, bytes({1, 1, 5, 18, 1, 2, 3, 4, 1, 2, 4, 4, 0xff, 0xff, 0xff, 0xf0, 0, 0, 0, 0x10})));
}
```

In `test_tcp_frames.cpp`: `SackBlocksOfReadsWhatSackOptionWrote` builds a `TcpSpec` whose `options` are `joined({timestamps_option(1, 2), sack_option(blocks)})`, runs it through `tcp_frame` and `parse_frame`, and expects `sack_blocks_of(frame)` to equal the two blocks above; and to be empty for a frame with timestamps alone.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset debug 2>&1 | tail -5`
Expected: a compile error naming `window_scale`, `write_sack` or `sack_blocks_of`.

- [ ] **Step 3: Implement the option kinds, the writers, the frames builders and `sack_blocks_of`**

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset debug && ctest --preset debug -R 'Wire|Frames'`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add common/wire fixtures/frames tests/unit_tests/common/wire tests/unit_tests/fixtures/frames
git commit -m "feat(wire): read and write the window scale, SACK-permitted and SACK options"
```

---

## Task 2: The configuration, the counters and the runner's lines

The fields exist and are validated; nothing reads them yet, and `receive_segments` keeps its default of 32 until Task 3 gives the budget its new rule.

**Files:**
- Modify: `common/tcp/config/tcp_config.hpp`, `common/tcp/config/tcp_counters.hpp`
- Modify: `fixtures/scenario/script/script.hpp`, `fixtures/scenario/script/parse_script.cpp`, `fixtures/scenario/run/harness.cpp`
- Modify: `docs/architecture/tcp.md` (the configuration and counter tables), `docs/architecture/scenario.md` (the option lines)
- Test: `tests/unit_tests/common/tcp/test_tcp_handshake.cpp` (`ConfigValidation`), `tests/unit_tests/fixtures/scenario/test_parse_script.cpp`, `test_harness.cpp`

**Interfaces:**
- Produces, `TcpConfig`, each with the doc line shown, in this position among the existing fields:

```cpp
std::size_t receive_segments  = 32;    ///< Packets one connection may hold, in order and out of order together; with `mss()`, the default window.
std::uint32_t receive_window  = 0;     ///< The byte window when set, capped at what the slots hold; zero is half of what they hold.
std::size_t receive_pool      = 2048;
// ... the existing fields ...
bool timestamps               = true;
bool sack                     = true;  ///< Offer and use SACK-permitted: report what is held, and what arrived twice.
bool window_scaling           = true;  ///< Offer and use RFC 7323 window scaling.
core::Duration delayed_ack    = core::Duration::zero();  ///< How long an ACK for in-order data may wait; zero sends it at the flush of the same tick.
```

  `validate` adds, each with a message that names the field as the existing ones do: `receive_segments > receive_pool`; `receive_window > (65535U << 14U)`; `delayed_ack` negative, above 500 ms, or positive and below `resolution`.

- Produces, `TcpCounters`, after `window_updates`:

```cpp
std::uint64_t out_of_order_held     = 0;  ///< Segments held above a gap.
std::uint64_t out_of_order_released = 0;  ///< Held out-of-order segments released to make room for a lower sequence.
std::uint64_t sack_acks_sent        = 0;  ///< Pure ACKs that carried SACK blocks.
std::uint64_t duplicate_sacks_sent  = 0;  ///< Of them, those whose first block reported data that arrived twice.
std::uint64_t delayed_acks_sent     = 0;  ///< ACKs the delay timer sent.
```

- Produces, `ScriptTcpOptions`: `std::optional<std::size_t> receive_segments`, `std::optional<std::uint32_t> receive_window`, `std::optional<bool> sack`, `std::optional<bool> window_scaling`, `std::optional<core::Duration> delayed_ack`; `apply_option` takes `tcp_receive_segments` and `tcp_receive_window` (`set_count`), `tcp_sack` and `tcp_window_scaling` (`take_flag`, as `tcp_timestamps`), `tcp_delayed_ack_ms` (`set_duration`); `configured` applies each when set.

- [ ] **Step 1: Write the failing tests**

`ConfigValidation` gains, in the style of its neighbours:

```cpp
EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, {.receive_segments = 2049}}), std::invalid_argument);  // above the pool
EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, {.receive_window = (65535U << 14U) + 1}}), std::invalid_argument);
EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, {.delayed_ack = -1ms}}), std::invalid_argument);
EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, {.delayed_ack = 501ms}}), std::invalid_argument);
EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, {.delayed_ack = 500us}}), std::invalid_argument);  // below the 1 ms wheel
EXPECT_NO_THROW((std::ignore = Tcp{ip_, wheel_, {.receive_window = 65535U << 14U, .delayed_ack = 500ms}}));
```

`test_parse_script.cpp`: `ParsesTheReceiveSideOptionLines` parses the five lines `--tcp_receive_segments=64`, `--tcp_receive_window=100000`, `--tcp_sack=0`, `--tcp_window_scaling=0`, `--tcp_delayed_ack_ms=40` and expects `64`, `100000`, `false`, `false`, `40ms`; `ARepeatedReceiveSideLineIsAnError` expects "given twice" for a second `--tcp_sack=1`; `--tcp_sack=2` fails with "expected 0 or 1". `test_harness.cpp`: `ConfiguredAppliesTheReceiveSideLines` expects each of the five in `configured(base, script).tcp`.

- [ ] **Step 2: Run them to see them fail** (`cmake --build --preset debug`; a compile error on the first new field)

- [ ] **Step 3: Add the fields, the validation, the counters and the lines; add the rows to the two tables of `tcp.md` and the lines to `scenario.md`'s list**

- [ ] **Step 4: Run** `cmake --build --preset debug && ctest --preset debug` — Expected: PASS, 60 tests.

- [ ] **Step 5: Commit**

```bash
git add common/tcp/config fixtures/scenario tests/unit_tests docs/architecture
git commit -m "feat(tcp): configuration and counters for the receive side, and the runner's lines for them"
```

---

## Task 3: `ReceiveWindow`: the budget in `mss()`, 512 slots, the offer and the consume rule

The offered edge leaves `ReceiveQueue` for a part of its own, and the three rules that govern it land together: the budget, the silly-window threshold, and the doubling rule on `consume`. Window scaling is not negotiated yet, so `scale` is always empty here and every budget is at most 65535. Spec: "The receive window" (all of it but "The scale"), "Acknowledgement timing" ("A duplicate ACK stays a duplicate").

**Files:**
- Create: `common/tcp/connection/detail/receive_window.hpp`
- Modify: `common/tcp/connection/detail/receive_queue.hpp`, `control_block.hpp`, `common/tcp/stack/detail/stack_context.hpp`, `transmitter.hpp`, `receiver.hpp`, `common/tcp/stack/tcp_stack.hpp`, `common/tcp/connection/tcp_connection.hpp`, `common/tcp/config/tcp_config.hpp` (the default), `common/tcp/CMakeLists.txt`, `common/tcp/export/aloe/tcp`
- Modify: `tests/shared/tcp/tcp_fixture.hpp` (`tcp_fabric_budget`), every script under `tests/scenarios/tcp/`, `docs/architecture/tcp.md` (the `receive_segments` row), `docs/architecture/scenario.md` (the failure example's numbers)
- Test: `tests/unit_tests/common/tcp/test_tcp_receive_window.cpp` (new, added to `tests/unit_tests/common/tcp/CMakeLists.txt`), `test_tcp_window.cpp`, `test_tcp_acks.cpp`, `test_tcp_data.cpp`, `test_tcp_timestamps.cpp`, and whichever tests read a window number

**Interfaces:**
- Produces, `common/tcp/connection/detail/receive_window.hpp`, namespace `aloe::tcp::detail`:

```cpp
/**
 * @brief A connection's receive window: the byte budget, our scale, and the right edge already offered to the
 * peer, as the peer reconstructs it from the scaled field.
 *
 * The edge never moves left. It moves right by full segments only (RFC 1122 4.2.3.3), and a field is rounded up
 * to the scale's unit, so the edge can stand ahead of the budget; the packet cap, not the budget, bounds memory.
 */
class ReceiveWindow {
public:
    /// What a segment that carries an ACK states: the window field, and the edge the peer reconstructs from it.
    struct Offer {
        std::uint16_t field = 0;
        wire::TcpSequence edge{};
    };

    static constexpr std::uint32_t unscaled_max = 65535;

    /// Half of what `segments` full segments hold, or `configured` when set, never more than they hold nor more
    /// than the field states under `shift`.
    [[nodiscard]] static constexpr std::uint32_t budget_of(std::size_t segments, std::uint16_t full_segment,
                                                           std::uint32_t configured, std::uint8_t shift) noexcept;
    /// The smallest shift, at most 14, under which the field states `budget`.
    [[nodiscard]] static constexpr std::uint8_t shift_for(std::uint32_t budget) noexcept;

    /// The handshake settled the options: RCV.NXT is `next`, a full segment carries `full_segment` bytes, and
    /// `scale` is our shift when both ends offered window scaling. The handshake's own segment stated
    /// `min(budget, 65535)`, unscaled, which is where the edge starts.
    void open(wire::TcpSequence next, std::uint32_t budget, std::uint16_t full_segment,
              std::optional<std::uint8_t> scale) noexcept;

    [[nodiscard]] std::uint32_t budget() const noexcept;
    [[nodiscard]] std::optional<std::uint8_t> scale() const noexcept;  ///< What a SYN-ACK's option states.
    [[nodiscard]] std::uint8_t shift() const noexcept;                 ///< `scale()`, or zero.
    [[nodiscard]] wire::TcpSequence edge() const noexcept;
    [[nodiscard]] std::uint32_t window(wire::TcpSequence next) const noexcept;  ///< `edge - next`.

    /// What a SYN-ACK states: the field unscaled, `min(budget, 65535)`.
    [[nodiscard]] Offer handshake(wire::TcpSequence next) const noexcept;
    /// What an ordinary segment with an ACK states now.
    [[nodiscard]] Offer offer(wire::TcpSequence next, std::size_t unread, bool slot_free) const noexcept;
    /// What a duplicate ACK states: the edge already offered, whatever was consumed since.
    [[nodiscard]] Offer restate(wire::TcpSequence next) const noexcept;
    /// A segment left stating `offer`; true when that moved the edge.
    [[nodiscard]] bool record(const Offer& offer) noexcept;
    /// Whether a `consume` owes a window update of its own: Linux's doubling rule (`__tcp_cleanup_rbuf`).
    [[nodiscard]] bool update_due(wire::TcpSequence next, std::size_t unread, bool slot_free) const noexcept;

private:
    std::uint32_t budget_       = 0;
    std::uint16_t full_segment_ = 0;
    std::optional<std::uint8_t> scale_{};
    wire::TcpSequence edge_{};
};
```

  The arithmetic is the block under "Read This First", in 64 bits where a product can pass 32. `offer`, `restate` and `handshake` do not change the object; only `record` does. Distances are `wire::TcpSequence`'s signed `operator-`, never a comparison of `.value`.

- Produces, `ReceiveQueue`: `void open(wire::TcpSequence next) noexcept`; `offered()`, `window()`, `offer()`, `record_offer()` and the `offered_` member are removed. The class comment loses the offered edge.
- Produces, `ControlBlock`: the member `ReceiveWindow window{};` after `receive`, reset in `clear()`; and

```cpp
/// Packets the connection holds, on both chains (the out-of-order chain joins in Task 6).
[[nodiscard]] std::uint32_t held_segments() const noexcept;
```

- Produces, `StackContext`: `byte_budget` is replaced by

```cpp
std::uint8_t window_shift    = 0;  ///< Ours, offered on every SYN and SYN-ACK when window scaling is on; Task 4 computes it.
std::uint32_t largest_budget = 0;  ///< `ReceiveWindow::budget_of(receive_segments, mss, receive_window, window_shift)`.
std::uint16_t syn_window     = 0;  ///< `min(largest_budget, 65535)`: what a SYN states, before the connection has a budget.
```

  `Stack::byte_budget()` returns `largest_budget`, documented as "the largest budget the configuration allows; a connection's own is `Connection::receive_budget()`".
- Produces, `transmitter.hpp`:

```cpp
/// Which edge a segment with an ACK states.
enum class Edge : std::uint8_t {
    Offer,    ///< What the window offers now (`ReceiveWindow::offer`).
    Restate,  ///< The edge already offered: a duplicate ACK stays a duplicate (RFC 5681).
};

[[nodiscard]] bool send_pure_ack(ControlBlock& block, Edge edge = Edge::Offer) noexcept;
/// What a segment with an ACK states now.
[[nodiscard]] ReceiveWindow::Offer offer_of(const ControlBlock& block, Edge edge) const noexcept;
```

  `emit` gains a last parameter `Edge edge = Edge::Offer`. Its window: a segment without ACK (the SYN) states `context_.syn_window` and records nothing; a handshake segment with ACK (the SYN-ACK) states `block.window.handshake(next)`; every other takes `offer_of(block, edge)`. `record_offer(block, const ReceiveWindow::Offer&)` calls `block.window.record`. `send_reset` takes `offer_of(block, Edge::Offer)` and writes its `field`. `slot_free` is `block.held_segments() < context_.config.receive_segments`.
- Produces, `receiver.hpp`:

```cpp
/// Opens the receive side once the handshake settled the peer's MSS and the options: RCV.NXT, then the window
/// with the budget counted in `mss()`. `scaling` is whether both ends offered window scaling (Task 4).
void open_receive(ControlBlock& block, Sequence next, bool scaling) noexcept;
void ack_now(ControlBlock& block, Edge edge = Edge::Offer) noexcept;
```

  The opens are reordered so `block.mss()` is final when `open_receive` runs. `passive_open`: `send.peer_mss`, `send.wnd`/`wl1`/`wl2`, `timestamps.answer`, then `open_receive(block, sequence + 1, false)`, then `timestamps.sent_ack`. `receive_syn_sent`: `timestamps.answer`, `handshake_acknowledged`, `send.peer_mss`, `send.synchronize`, `lifecycle_.connected(block)`, then `open_receive(block, sequence + 1, false)`, then `timestamps.sent_ack`, `update_probing`, `queue_ack`. `accept_sequence` reads `block.window.edge()` and `block.window.window(next)`. `receive_payload` asks the ACK policy with `block.mss()` as the full size. `consume` queues an ACK when `block.window.update_due(block.receive.next(), block.receive.unread(), slot_free)`.
  `Edge::Restate` is passed by: the PAWS rejection; every `drop` in `accept_sequence`; `receive_ack`'s ACK of what was never sent; `receive_payload`'s three refusals; the second FIN. `Edge::Offer` stays for accepted payload and the FIN.
- Produces, `Connection`: `[[nodiscard]] std::uint32_t receive_budget() const noexcept` and `[[nodiscard]] std::uint32_t receive_window() const noexcept` (the bytes on offer, `edge - RCV.NXT`).
- Produces: `TcpConfig::receive_segments` defaults to 512. `tests/shared/tcp/tcp_fixture.hpp`: `inline constexpr std::uint32_t tcp_fabric_budget = 65535;  ///< The default budget against a peer without window scaling.`

- [ ] **Step 1: Write the failing unit tests of the part**

`tests/unit_tests/common/tcp/test_tcp_receive_window.cpp`, plain `TEST`s over `aloe::tcp::detail::ReceiveWindow`, with `const TcpSequence start{1000}` and a helper `ack(window, next, unread)` that records `window.offer(next, unread, true)` and returns it:

```cpp
TEST(ReceiveWindow, TheBudgetIsHalfOfWhatTheSlotsHold) {
    EXPECT_EQ(ReceiveWindow::budget_of(512, 1448, 0, 3), 370688U);
    EXPECT_EQ(ReceiveWindow::budget_of(512, 1460, 0, 3), 373760U);
    EXPECT_EQ(ReceiveWindow::budget_of(32, 1448, 0, 0), 23168U);
}
TEST(ReceiveWindow, AConfiguredWindowIsCappedAtWhatTheSlotsHold) {
    EXPECT_EQ(ReceiveWindow::budget_of(512, 1448, 100000, 3), 100000U);
    EXPECT_EQ(ReceiveWindow::budget_of(4, 1448, 100000, 3), 5792U);
}
TEST(ReceiveWindow, AnUnscaledBudgetIsAtMostTheField) {
    EXPECT_EQ(ReceiveWindow::budget_of(512, 1448, 0, 0), 65535U);
    EXPECT_EQ(ReceiveWindow::budget_of(512, 1448, 0, 14), 370688U);
}
TEST(ReceiveWindow, TheShiftIsTheSmallestThatStatesTheBudget) {
    EXPECT_EQ(ReceiveWindow::shift_for(65535), 0);
    EXPECT_EQ(ReceiveWindow::shift_for(65536), 1);
    EXPECT_EQ(ReceiveWindow::shift_for(373760), 3);
    EXPECT_EQ(ReceiveWindow::shift_for(65535U << 14U), 14);
    EXPECT_EQ(ReceiveWindow::shift_for(0xffffffffU), 14);
}
TEST(ReceiveWindow, TheHandshakeStatesTheBudgetUnscaledAndTheFirstAckScaled) {
    ReceiveWindow window;
    window.open(start, 370688, 1448, std::uint8_t{3});
    EXPECT_EQ(window.handshake(start).field, 65535);
    EXPECT_EQ(window.edge(), start + 65535U);
    const auto first = window.offer(start, 0, true);
    EXPECT_EQ(first.field, 46336);
    EXPECT_EQ(first.edge, start + 370688U);
}
TEST(ReceiveWindow, TheEdgeMovesOnlyByAFullSegment) {
    ReceiveWindow window;
    window.open(start, 65535, 1448, std::nullopt);
    EXPECT_EQ(ack(window, start + 1000U, 1000).field, 64535) << "the edge stands; the window is what is left";
    EXPECT_EQ(ack(window, start + 1000U, 0).field, 64535) << "1000 bytes read: less than a full segment";
    EXPECT_EQ(ack(window, start + 1448U, 0).field, 65535) << "1448 beyond the edge: it moves";
}
TEST(ReceiveWindow, NoFreeSlotAndARestateBothLeaveTheEdge) {
    ReceiveWindow window;
    window.open(start, 65535, 1448, std::nullopt);
    std::ignore = ack(window, start + 3000U, 3000);
    EXPECT_EQ(window.offer(start + 3000U, 0, false).field, 62535);
    EXPECT_EQ(window.restate(start + 3000U).field, 62535);
    EXPECT_EQ(window.offer(start + 3000U, 0, true).field, 65535);
}
TEST(ReceiveWindow, TheFieldIsRoundedUpAndTheReconstructedEdgeRecorded) {
    ReceiveWindow window;
    window.open(start, 370688, 1448, std::uint8_t{3});
    std::ignore       = ack(window, start, 0);              // the edge is start + 370688
    const auto offer  = window.offer(start + 3U, 3, true);  // 370685 bytes to the edge: 46335.6 units
    EXPECT_EQ(offer.field, 46336);
    EXPECT_EQ(offer.edge, start + 370691U) << "what the peer reconstructs, three bytes past the computed edge";
}
TEST(ReceiveWindow, SegmentsShorterThanTheUnitNeverCloseTheWindow) {
    ReceiveWindow window;
    window.open(start, 370688, 1448, std::uint8_t{3});
    std::ignore = ack(window, start, 0);
    for (std::uint32_t received = 4; received <= 4000; received += 4) {
        EXPECT_EQ(ack(window, start + received, received).field, 46336);
    }
    EXPECT_EQ(window.edge(), start + 370688U + 4000U) << "the creep the spec describes: the cap bounds memory";
}
TEST(ReceiveWindow, AClosedWindowStaysClosedUntilAFullSegmentFits) {
    ReceiveWindow window;
    window.open(start, 65535, 1448, std::nullopt);
    std::ignore = ack(window, start + 65535U, 65535);
    EXPECT_EQ(window.window(start + 65535U), 0U);
    EXPECT_EQ(window.offer(start + 65535U, 65535 - 1447, true).field, 0);
    EXPECT_FALSE(window.update_due(start + 65535U, 65535 - 1447, true));
    EXPECT_TRUE(window.update_due(start + 65535U, 65535 - 1448, true));
    EXPECT_EQ(window.offer(start + 65535U, 65535 - 1448, true).field, 1448);
}
TEST(ReceiveWindow, AConsumeOwesAnUpdateOnlyWhenTheWindowDoubles) {
    ReceiveWindow window;
    window.open(start, 65535, 1448, std::nullopt);
    std::ignore = ack(window, start + 40000U, 40000);  // 25535 on offer
    EXPECT_FALSE(window.update_due(start + 40000U, 20000, true)) << "45535 is less than twice 25535";
    EXPECT_TRUE(window.update_due(start + 40000U, 10000, true)) << "55535 is more";
    EXPECT_FALSE(window.update_due(start + 40000U, 10000, false)) << "no slot: the edge may not move";
}
TEST(ReceiveWindow, WrapsAcrossTheTopOfTheSequenceSpace) {
    ReceiveWindow window;
    const TcpSequence high{0xffffff00U};
    window.open(high, 65535, 1448, std::nullopt);
    EXPECT_TRUE(window.edge().after(high));
    EXPECT_EQ(window.window(high), 65535U);
    EXPECT_EQ(ack(window, high + 0x200U, 0).field, 65535 - 0x200) << "past the wrap, 512 bytes on: the edge stands";
    EXPECT_EQ(ack(window, high + 0x600U, 0).edge, high + 0x600U + 65535U) << "1536 bytes on: it moves";
    EXPECT_EQ(window.window(high + 0x600U), 65535U);
}
```

and `TheEdgeNeverMovesLeftOverARandomRun`: a `std::mt19937` with a fixed seed (one-line `NOLINT(cert-msc51-cpp)` with the reason), `std::uniform_int_distribution`s, 100000 steps each of which arrives up to `window.window(next)` bytes, consumes up to `unread`, or records an `offer` or a `restate` with a random `slot_free`; after every `record` it expects `!window.edge().before(previous)` and `!window.edge().before(next)`. Run once with `scale` 3 and once with none.

- [ ] **Step 2: Run them to see them fail** (`cmake --build --preset debug`: `receive_window.hpp` not found)

- [ ] **Step 3: Write `ReceiveWindow`, add it to `CMakeLists.txt` and the umbrella, and run its tests**

Run: `ctest --preset debug -R Tcp` — Expected: the `ReceiveWindow.*` tests pass; the brick is unchanged so far.

- [ ] **Step 4: Write the failing tests of the brick**

In `test_tcp_window.cpp`, in the style of its neighbours (`open_passive(peer_)`, `receive(peer_.data(...))`, `collect()`):

- `TheBudgetCountsTheTimestampedSegment`: `configure_tcp({.receive_segments = 32})`, `peer_.set_timestamps(100)`, a passive open; the SYN-ACK's window is `23168` (`32 * 1448 / 2`) and `c->receive_budget() == 23168U`.
- `AnActiveOpenTakesItsBudgetAfterTheHandshake`: the same configuration and timestamps, `open_active(peer_)`; the handshake's ACK states `23168`, not `8576` (536-byte slots) and not `23360` (1460-byte slots); the SYN stated `23360`.
- `ASmallConsumeSendsNothingAndLeavesTheEdge`: 3000 bytes in and acknowledged (`65535 - 3000`), `consume(1000)`, `collect()` is empty and `window_updates` is unchanged.
- `AConsumeThatDoublesTheWindowSendsOneUpdate`: 40000 bytes in over several segments with the application not reading, then `consume(30000)`; `collect()` holds one pure ACK whose window is `55535`.
- `ADuplicateAckRestatesTheEdgeAfterAConsume`: 3000 bytes in and acknowledged, `consume(3000)`, then the first segment again; the ACK sent at once states `62535`; the next in-order segment's ACK at the flush states more.
- `APeerWithASmallMssIsAcknowledgedEverySecondSegment`: a peer whose spec has `mss = 536`; two 536-byte segments are answered by one ACK at once.

Rewrite the tests that pinned the old rules, keeping what each pins: `PartialConsumeFreesNoNodeButMovesTheEdge` no longer moves the edge; `ZeroWindowAcceptsPureAcksAndReopensOnConsume` reopens once 1460 bytes are consumed; `TinySegmentsExhaustSlotsWithCreditLeft` and `SlotCapDropsWithoutAdvancingAndRecovers` pin `.receive_segments = 32`; `BudgetIsCappedAtTheWireMaximum` expects 65535; `SharedPoolExhaustionNeverAdvancesRcvNxt` pins the cap beside its small pool. Every other reader of `tcp_fabric_budget` takes the new constant.

- [ ] **Step 5: Move the edge into `ReceiveWindow`, reorder the opens, pass the edge kind, and change the default cap**

Order that keeps each build step small: `ReceiveQueue` and `ControlBlock`; `StackContext`; the Transmitter's `emit`, `offer_of`, `record_offer`, `send_reset`; the Receiver; the accessors.

- [ ] **Step 6: Re-derive every script**

Rules, from "Read This First": the stack's SYN and SYN-ACK state `win 65535`; a later `win` is the edge less RCV.NXT, with the edge at `65536` relative until a candidate at least `min(32767, mss())` beyond it replaces it; a `read` line is followed by a window-update line only when the doubling rule holds; a script with `--tcp_receive_segments=n` has the budget `n * mss() / 2`. Worked example, `data/above-a-gap-is-acked-not-stored.pkt`: the SYN-ACK's `win 46720` becomes `win 65535`; the two ACKs' `win 45720` become `win 64535`; the last line, the update after `read`, is deleted, since 65535 is not twice 64535.

Run: `./build/debug/tests/unit_tests/scenarios/Aloe.Tests.Unit.Scenarios`
Expected: 150 runs pass. A failure whose message shows a window the rules do not give is a defect in the code, not a number to copy.

- [ ] **Step 7: Run everything**

Run: `ctest --preset debug`, then `gcc-debug`, `asan` and `tsan` the same way, `./scripts/check-format.sh`, `./scripts/check-includes.sh`
Expected: PASS on all four presets.

- [ ] **Step 8: Commit**

```bash
git add common/tcp tests docs/architecture
git commit -m "feat(tcp): a receive window of its own, budgeted in full segments, moved by full segments"
```

---

## Task 4: The handshake's options and window scaling

Both options are negotiated here so a SYN changes once. SACK-permitted is negotiated and recorded; the blocks follow in Task 7, and until then the stack holds nothing out of order, so it has nothing to report. Spec: "The scale", "The peer's window", "The handshake's options", "Negotiation".

**Files:**
- Create: `common/tcp/connection/detail/sack_blocks.hpp` (the negotiation only; Task 7 grows it)
- Modify: `common/tcp/connection/detail/send_sequence.hpp`, `control_block.hpp`, `common/tcp/stack/detail/stack_context.hpp`, `segment_writer.hpp`, `transmitter.hpp`, `receiver.hpp`, `lifecycle.hpp`, `ack_processor.hpp`, `common/tcp/stack/tcp_stack.hpp` (the headroom assertion), `common/tcp/connection/tcp_connection.hpp`, `common/tcp/CMakeLists.txt`, `common/tcp/export/aloe/tcp`
- Modify: `fixtures/frames/frames/tcp_peer.hpp`, the three active-open scripts, `docs/architecture/fixtures.md` (what `TcpPeer` offers)
- Test: `tests/unit_tests/common/tcp/test_tcp_handshake.cpp`, `test_tcp_window.cpp`, `test_tcp_data.cpp`, `test_tcp_recovery.cpp`, `test_tcp_probes.cpp`, `tests/unit_tests/fixtures/frames/test_tcp_frames.cpp`; new scripts under `tests/scenarios/tcp/handshake/` and `window/`

**Interfaces:**
- Produces, `sack_blocks.hpp`, the negotiation, shaped as `Timestamps`' is:

```cpp
class SackBlocks {
public:
    void offer(bool enabled) noexcept;        ///< What the SYN or SYN-ACK offers.
    void answer(bool peer_offered) noexcept;  ///< In use only when both ends sent the option.
    void withdraw() noexcept;                 ///< The offer was never answered.
    /// In SynSent the SYN's offer; from the handshake on, the option both ends sent.
    [[nodiscard]] bool in_use() const noexcept;
};
```

  `ControlBlock` gains `SackBlocks sack{};` behind the parts an in-order segment touches, reset in `clear()`.
- Produces, `SendSequence`:

```cpp
std::uint8_t peer_shift = 0;  ///< The peer's window scale, zero unless both ends offered the option; at most 14.
/// A window field of a segment on a synchronized connection, in bytes.
[[nodiscard]] std::uint32_t scaled(std::uint16_t field) const noexcept;
```

  `synchronize(header)` keeps taking the field as it is (a SYN or SYN-ACK is never scaled); `update_window(header)` stores `scaled(header.window)`. `AckProcessor::on_ack`'s duplicate test compares `block.send.scaled(header.window) == block.send.wnd`.
- Produces, `StackContext`: `window_shift = config.window_scaling ? ReceiveWindow::shift_for(ReceiveWindow::budget_of(config.receive_segments, mss, config.receive_window, 14)) : 0`, then `largest_budget` with that shift.
- Produces, `OutgoingSegment`:

```cpp
bool sack_permitted = false;                 ///< On a handshake segment.
std::optional<std::uint8_t> window_scale{};  ///< On a handshake segment: our shift.
```

  `size()` adds 4 for `sack_permitted` only when `timestamps` is absent, and 4 for `window_scale`. `SegmentWriter::write` writes, in order: the MSS; `write_sack_permitted_timestamps` or `write_timestamps` or `write_sack_permitted`; `write_window_scale`. `tcp::Stack`'s headroom assertion becomes `net::Prebuilt::size + wire::TcpHeader::max_size <= device::packet_headroom`, which covers every option block there can be.
- Produces, the Transmitter's `emit`: a handshake segment carries `sack_permitted = block.sack.in_use()` and `window_scale`: in SynSent `context_.config.window_scaling ? std::optional{context_.window_shift} : std::nullopt`, otherwise `block.window.scale()`. A segment that is not a handshake segment carries neither.
- Produces, the Receiver: `Segment` gains `std::optional<std::uint8_t> window_scale{}` and `bool sack_permitted = false`, set from the options of a segment with SYN only, as the MSS is. `passive_open` and `receive_syn_sent` call `block.sack.answer(segment.sack_permitted)` and compute `const bool scaling = context_.config.window_scaling && segment.window_scale.has_value();`, store `block.send.peer_shift = scaling ? std::min(*segment.window_scale, wire::TcpOptions::max_window_scale) : 0`, and pass `scaling` to `open_receive`, which opens the window with `scale = scaling ? std::optional{context_.window_shift} : std::nullopt` and the budget under that shift. `Lifecycle::open` calls `block.sack.offer(context_.config.sack)`; `set_closed` in SynSent calls `block.sack.withdraw()`.
- Produces, `Connection`: `[[nodiscard]] bool sack() const noexcept`, `[[nodiscard]] bool window_scaling() const noexcept` (`window.scale().has_value()`), `[[nodiscard]] std::uint8_t window_shift() const noexcept`, `[[nodiscard]] std::uint8_t peer_window_shift() const noexcept`.
- Produces, `frames::TcpPeer`:

```cpp
/// While engaged, a SYN or SYN-ACK the peer builds offers window scaling with `shift`; `set_window` keeps taking the raw field.
void set_window_scale(std::optional<std::uint8_t> shift) noexcept;
/// While set, a SYN or SYN-ACK the peer builds offers SACK-permitted.
void set_sack_permitted(bool offered) noexcept;
[[nodiscard]] std::optional<std::uint8_t> stack_window_scale() const noexcept;  ///< From the stack's SYN or SYN-ACK.
[[nodiscard]] bool stack_sack_permitted() const noexcept;
/// The stack's window in bytes: the last field seen, shifted when both ends offered scaling and the frame was not a SYN.
[[nodiscard]] std::uint32_t stack_window_bytes() const noexcept;
```

  A segment built `with_mss` carries its options in Linux's layout: `mss`; then `sackOK, TS` or `nop, nop, TS` or `nop, nop, sackOK`; then `nop, wscale`. Every other segment is as today.

- [ ] **Step 1: Write the failing tests**

`test_tcp_handshake.cpp`:

- `TheOptionsAreInUseOnlyWhenBothEndsSendThem`: for each of the four combinations of the peer offering SACK-permitted and window scaling (shift 7), with timestamps on and off, a passive open; the SYN-ACK's option bytes are exactly the table of "Global Constraints" (compare `frames::tcp_options_of(frame)` with the bytes built from the `frames` option builders), and `c->sack()`, `c->window_scaling()`, `c->window_shift()` (3 or 0) and `c->peer_window_shift()` (7 or 0) agree. Repeated with `configure_tcp({.sack = false})` and `{.window_scaling = false}`: the option is absent whatever the peer offered.
- `AnActiveOpenOffersBothAndTakesThePeersAnswer`: the SYN's bytes with timestamps are `mss 1460, sackOK, TS, nop, wscale 3`; for each of the four answers the accessors agree after `Connected`.
- `ASynAndASynAckStateTheWindowUnscaled`: both state 65535 with the defaults.
- `TheFirstAckAfterTheHandshakeStatesTheBudgetScaled`: a passive open against a scaling peer without timestamps, one byte in; the ACK's field is `(373760 - 1 + 7) >> 3 = 46720` and `c->receive_budget() == 373760U`; with timestamps on the peer the budget is `370688U`.
- `APeerShiftAboveFourteenIsUsedAsFourteen`: `peer_.set_window_scale(15)`; `c->peer_window_shift() == 14`.
- `ARetransmittedSynOffersEveryOptionAgain` and `ARetransmittedSynAckKeepsItsOptions`: after one RTO the resent segment's option bytes equal the first's.
- `AnActiveOpenThatEndsUnansweredNegotiatedNothing`: a SYN that times out leaves `c->sack()` false.

`test_tcp_data.cpp`, on a fixture over `detail::FabricPair{GetParam(), 1, 256}` and `TcpHarness<fabric::Port, tcp::NoCongestionControl>` configured with `.send_segments = 64`:

- `APeersScaledWindowLetsTheFlightPass65535Bytes`: the peer offers shift 7 and a field of 1024 (131072 bytes); `send` until it accepts nothing; `c->unacknowledged() > 65535U`.

`test_tcp_recovery.cpp`: `ADuplicateAckIsJudgedByTheScaledWindow`: the peer offers shift 7, a field of 512; three duplicate ACKs with that field start a fast retransmit (`fast_retransmits == 1`). `test_tcp_probes.cpp`: `AZeroFieldFromAScalingPeerIsProbed`.

`test_tcp_frames.cpp`: `APeerOffersTheOptionsInLinuxLayout` (the SYN's bytes for each combination) and `ThePeerReadsTheStacksOptions`.

- [ ] **Step 2: Run them to see them fail**

- [ ] **Step 3: Implement the negotiation, the writer's layout, the peer's shift and `TcpPeer`**

- [ ] **Step 4: Change the three active-open scripts and add the new ones**

The stack's SYN in the three scripts becomes `win 65535 <mss 1460,sackOK,TS val 0 ecr 0,nop,wscale 3>`, or `<mss 1460,nop,nop,sackOK,nop,wscale 3>` under `--tcp_timestamps=0`. Their peers answer with neither option, so every later line stands.

New scripts, each with its opening comment:

`handshake/passive-open-negotiates-sack-and-window-scale.pkt`:

```
// A peer that offers SACK-permitted and window scaling is answered with both, in Linux's layout. The SYN-ACK's
// window is never scaled; the first ACK states the budget, 370688 bytes less what is unread, in units of 8.
0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0 bind(3, ..., ...) = 0
+0 listen(3, 1) = 0

+0 < S 0:0(0) win 65535 <mss 1460,sackOK,TS val 100 ecr 0,nop,wscale 7>
+0 > S. 0:0(0) ack 1 win 65535 <mss 1460,sackOK,TS val 0 ecr 100,nop,wscale 3>
+0 < . 1:1(0) ack 1 win 512 <nop,nop,TS val 100 ecr 0>
+0 accept(3, ..., ...) = 4

+0 < P. 1:1001(1000) ack 1 win 512 <nop,nop,TS val 100 ecr 0>
+0 > . 1:1(0) ack 1001 win 46211 <nop,nop,TS val 0 ecr 100>
```

  The last number: the candidate edge is `1001 + 370688 - 1000 = 370689`, far beyond the handshake's `65536`, so it moves; `370689 - 1001 = 369688`, and `369688 / 8 = 46211`.

`handshake/active-open-offers-sack-and-window-scale.pkt`: the call lines of `handshake/active-open.pkt`, then

```
+0 > S 0:0(0) win 65535 <mss 1460,sackOK,TS val 0 ecr 0,nop,wscale 3>
+0 < S. 0:0(0) ack 1 win 65535 <mss 1460,sackOK,TS val 100 ecr 0,nop,wscale 7>
+0 > . 1:1(0) ack 1 win 46336 <nop,nop,TS val 0 ecr 100>
```

and, derived the same way: `handshake/options-without-timestamps.pkt` (`--tcp_timestamps=0`: the SYN-ACK is `<mss 1460,nop,nop,sackOK,nop,wscale 3>` and the first ACK's window 46720 less the unread bytes in units of 8); `handshake/sack-and-window-scale-off-by-config.pkt` (`--tcp_sack=0`, `--tcp_window_scaling=0`: the SYN-ACK is `<mss 1460,nop,nop,TS val 0 ecr 100>`); `window/peer-without-window-scale-gets-65535.pkt`; `window/shift-above-14-is-used-as-14.pkt` (`--congestion=none`: the peer offers `wscale 15` and a field of 1; a `write` of 20000 bytes accepts 16384); `window/scaled-peer-window-passes-65535.pkt` (`--congestion=none`, `--tcp_send_segments=64`: the peer's field 1024 under `wscale 7`, and a `write` of 80000 bytes leaves whole).

- [ ] **Step 5: Run everything** — `ctest --preset debug`, the scripts binary, both check scripts. Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add common/tcp fixtures/frames tests docs/architecture
git commit -m "feat(tcp): negotiate SACK-permitted and window scaling, and scale both windows"
```

---

## Task 5: `ReassemblyQueue` and the pool's chain operations

A part with its own tests, used by nothing yet. Spec: "The reassembly queue" ("Inserting", "Joining", "A FIN above a gap"). Read `tcp_data_queue_ofo` and `tcp_ofo_queue` first.

**Files:**
- Create: `common/tcp/connection/detail/reassembly_queue.hpp`
- Modify: `common/tcp/pool/detail/node_pool.hpp`, `held_segment_pool.hpp`, `common/tcp/CMakeLists.txt`, `common/tcp/export/aloe/tcp`
- Test: `tests/unit_tests/common/tcp/test_tcp_reassembly.cpp` (new, added to the CMake list), `test_tcp_buffers.cpp` (the pool)

**Interfaces:**
- Produces, `NodePool<N>`:

```cpp
/// Links the taken node `index` after `previous` in `chain`; `no_node` links it at the head.
void insert_after(NodeChain& chain, std::uint32_t previous, std::uint32_t index) noexcept;
/// Unlinks the node after `previous`, the head for `no_node`, and returns it: still taken, linked to nothing.
[[nodiscard]] std::uint32_t unlink_after(NodeChain& chain, std::uint32_t previous) noexcept;
/// Links every node of `run` behind `chain`'s tail, in order, and leaves `run` empty.
void splice_back(NodeChain& chain, NodeChain& run) noexcept;
```

- Produces, `HeldSegment<Packet>`: `wire::TcpSequence first{};  ///< Of the first held byte; the reassembly queue writes and reads it, the in-order chain neither.` (plan's call: the in-order chain does not write it).
- Produces, `reassembly_queue.hpp`, namespace `aloe::tcp::detail`:

```cpp
/**
 * @brief A connection's out-of-order payload: a chain of the shard's held-segment nodes sorted by sequence, each
 * the packet a segment arrived in, and the sequence of a FIN that arrived above a gap.
 *
 * The ranges are disjoint and none reaches RCV.NXT: what reaches it is taken by `take_run`. Nothing is copied:
 * an overlap moves a node's offset, length and first sequence.
 */
template <device::IsPacket Packet>
class ReassemblyQueue {
public:
    using Pool = HeldSegmentPool<Packet>;

    enum class Placed : std::uint8_t {
        Held,       ///< The packet was moved into a node; `kept` is the range it holds after the trims.
        Duplicate,  ///< Held data covers every byte: nothing is kept and the packet is still the caller's.
        NoNode,     ///< The pool had no node: nothing is kept and the packet is still the caller's.
    };
    struct Inserted {
        Placed placed = Placed::Held;
        wire::TcpSackBlock kept{};
        std::optional<wire::TcpSackBlock> duplicate{};  ///< The first range of the segment that was already held.
    };
    /// What the join took: nodes still taken, linked in order, ready for `ReceiveQueue::append`.
    struct Run {
        NodeChain chain{};
        std::uint32_t bytes = 0;
        std::optional<wire::TcpSackBlock> duplicate{};  ///< Held data that RCV.NXT had already passed.
    };

    /// A node is held or a FIN is remembered: the one question an in-order segment asks.
    [[nodiscard]] bool holds() const noexcept;
    [[nodiscard]] std::uint32_t segments() const noexcept;
    [[nodiscard]] std::size_t bytes() const noexcept;
    /// The range of the node with the highest sequence.
    [[nodiscard]] std::optional<wire::TcpSackBlock> highest() const noexcept;
    [[nodiscard]] std::optional<wire::TcpSequence> fin() const noexcept;

    /// Places `length` bytes at `offset` of `packet`, the first of which is `first`, by the three rules.
    [[nodiscard]] Inserted insert(Pool& pool, Packet& packet, std::uint16_t offset, std::uint16_t length,
                                  wire::TcpSequence first) noexcept;
    /// Unlinks the run that follows `next`, RCV.NXT after an in-order segment was held.
    [[nodiscard]] Run take_run(Pool& pool, wire::TcpSequence next) noexcept;
    /// Releases the node with the highest sequence and forgets a remembered FIN; the range it held.
    [[nodiscard]] wire::TcpSackBlock release_highest(Pool& pool) noexcept;
    void remember_fin(wire::TcpSequence at) noexcept;
    void forget_fin() noexcept;
    /// Every node back to the pool, and the FIN forgotten.
    void release(Pool& pool) noexcept;
};
```

  `insert`, with `end = first + length`:

```
the chain is empty, or `first` is at or after the tail node's end:  acquire, write the node, push_back.   (no walk)
otherwise walk from the head, remembering the node before:
  pass every node whose end is at or before `first`
  the next node N, when it starts at or before `first`:
      N.end >= end                    -> Duplicate, duplicate = [first, end)                       (held data covers it)
      N.first < first                 -> duplicate = [first, N.end); first = N.end; pass N        (held bytes win the front)
      N.first == first, N.end < end   -> N is covered: the loop below releases it
  while the next node M starts before `end`:
      M.end <= end                    -> duplicate = M's range if none yet; unlink and release M   (the newcomer replaces it)
      otherwise                       -> duplicate = [M.first, end) if none yet; end = M.first; stop  (held bytes win the tail)
  first == end                        -> Duplicate
  acquire, after the releases so a replaced node's slot is the one taken; none -> NoNode
  node = {packet, offset + what the front lost, end - first, first}; insert_after the node before
```

  `take_run`: while the head starts at or before `next`: a head that ends at or before `next` is released and its range reported as the duplicate; a head that starts before `next` loses its front (`offset`, `length`, `first` move) and that front is reported; then the head is unlinked onto the run and `next` moves past it. One duplicate is reported: the first found, extended by a later one only when the two touch.
  `release_highest` walks to the node before the tail, since the chain is singly linked, and uses `release_back`.

- [ ] **Step 1: Write the failing tests**

`test_tcp_reassembly.cpp`, a fixture shaped as `TcpBuffers` in `test_tcp_buffers.cpp` with a port of 64 packets, a `Pool pool{32}`, and helpers: `segment(first, length, seed)` returns a packet whose `length` payload bytes at offset 54 are `frames::pattern(length, seed)`; `put(queue, first, length, seed)` calls `insert(pool, packet, 54, length, TcpSequence{first})`; `read(run)` returns the bytes of the run's nodes in order. With `const std::uint32_t next = 1000`:

| Test | What it does | What it expects |
|---|---|---|
| `ARunAboveTheGapIsHeldInOrder` | `put` 2000+500, 2500+500, 3000+500 | each `Held`, `segments() == 3`, `bytes() == 1500`, `highest()` is `[3000, 3500)` |
| `ArrivalInAnyOrderReadsTheSame` | every permutation of four adjacent 100-byte segments from 1000, then `take_run(1000)` | the run's bytes equal the four patterns in sequence order; `holds()` is false |
| `ADuplicateOfHeldDataIsRefusedAndReported` | 2000+500, then 2100+200 | `Duplicate`, `duplicate == [2100, 2300)`, the packet not empty, `segments() == 1` |
| `ANewcomerCoveredByTwoNeighboursIsADuplicate` | 2000+100, 2100+100, then 2050+100 | `Duplicate`, `duplicate == [2050, 2100)` |
| `ACoveringNewcomerReplacesWhatItCovers` | 2100+100, 2300+100, then 2000+500 | `Held`, `kept == [2000, 2500)`, `duplicate == [2100, 2200)`, `segments() == 1`, `pool.available()` one more than before |
| `ACoveringNewcomerAtTheSameStartReplacesTheNode` | 2000+100, then 2000+200 | `Held`, `kept == [2000, 2200)`, `duplicate == [2000, 2100)`, `segments() == 1` |
| `HeldBytesWinAPartialOverlapOnBothSides` | 2000+100 (seed 1), 2300+100 (seed 2), then 2050+300 (seed 3) | `kept == [2100, 2300)`, `duplicate == [2050, 2100)`; `take_run(2000)` reads seed 1's 100 bytes, seed 3's bytes 50..249, seed 2's 100 bytes |
| `TheJoinTakesTheRunAndStopsAtTheNextGap` | 1100+100, 1200+100, 1400+100; `take_run(1100)` | `run.chain.count == 2`, `run.bytes == 200`, `segments() == 1` |
| `TheJoinTrimsANodeRcvNxtPassedInPart` | 1100+200; `take_run(1150)` | `run.bytes == 150`, `duplicate == [1100, 1150)`, the bytes are the pattern's 50..199 |
| `TheJoinReleasesANodeRcvNxtPassedEntirely` | 1100+50, 1200+100; `take_run(1200)` | `duplicate == [1100, 1150)`, `run.bytes == 100`, one node back in the pool |
| `ReleaseHighestReturnsTheTailAndForgetsTheFin` | 2000+100, 3000+100, `remember_fin(3100)`; `release_highest` | `[3000, 3100)`, `fin()` empty, `highest()` is `[2000, 2100)` |
| `AnEmptyPoolRefusesAndThePacketStaysWithTheCaller` | `Pool pool{1}`, 2000+100, then 3000+100 | `NoNode`, the packet not empty |
| `AReplacingNewcomerNeedsNoFreeNode` | `Pool pool{1}`, 2000+100, then 2000+200 | `Held`, `kept == [2000, 2200)` |
| `ARememberedFinKeepsTheQueueHolding` | `remember_fin(5000)` on an empty queue | `holds()`, `segments() == 0`; `forget_fin()` and it does not |
| `WrapsAcrossTheTopOfTheSequenceSpace` | with first sequences `0xffffff00`, `0xffffff80`, `0x00000000`, `0x00000080`, 128 bytes each, inserted in the order 3, 1, 4, 2; `take_run(TcpSequence{0xffffff00})` | one run of 512 bytes in sequence order |
| `ARandomStreamIsReadBackByteExact` | 200 rounds with a fixed seed (`NOLINT(cert-msc51-cpp)`): a 4000-byte pattern from sequence 1000 cut into segments of 1 to 300 bytes, shuffled, a fifth of them delivered twice and a fifth widened to overlap a neighbour; the segment at 1000 is held back; after all are inserted, `take_run` | after every `insert`: the nodes' ranges sorted and disjoint, `segments()` at most the pool's size; at the end the run's bytes equal the pattern from the held-back segment's end on, and `holds()` is false |

In `test_tcp_buffers.cpp`, over the `Bare` node: `InsertAfterAndUnlinkAfterKeepTheChainInStep` (head, middle and tail; `chain.head`, `chain.tail` and `chain.count` after each) and `SpliceBackLinksARunAndEmptiesIt` (onto an empty chain and onto one with nodes).

- [ ] **Step 2: Run them to see them fail**

- [ ] **Step 3: Write the pool's three operations, the node's field and `ReassemblyQueue`; add the header to the CMake list and the umbrella**

- [ ] **Step 4: Run** `ctest --preset debug -R Tcp`, then the same under `asan`. Expected: PASS; ASan reports nothing for the randomized test.

- [ ] **Step 5: Commit**

```bash
git add common/tcp tests/unit_tests/common/tcp
git commit -m "feat(tcp): a reassembly queue over the held-segment pool"
```

---

## Task 6: The receive path holds what arrives above a gap

The sequence check gets its third outcome and the queue of Task 5 gets its caller. Every segment above a gap is answered at once with the edge restated, which is the final rule for a connection without SACK; Task 7 adds the blocks and the SACK connection's rule. Spec: "The life of a received segment", "Room", "The end of a connection".

**Files:**
- Modify: `common/tcp/stack/detail/receiver.hpp`, `lifecycle.hpp`, `common/tcp/stack/tcp_stack.hpp` (the destructor), `common/tcp/connection/detail/receive_queue.hpp`, `control_block.hpp`, `common/tcp/connection/tcp_connection.hpp`
- Delete: `tests/scenarios/tcp/data/above-a-gap-is-acked-not-stored.pkt`
- Test: `tests/unit_tests/common/tcp/test_tcp_data.cpp`, `test_tcp_acks.cpp`, `test_tcp_close.cpp`, `test_tcp_events.cpp`; new scripts under `tests/scenarios/tcp/reassembly/`

**Interfaces:**
- Consumes: `ReassemblyQueue` (Task 5), `Edge` and `ReceiveWindow` (Task 3).
- Produces, `ReceiveQueue`: `void append(Pool& pool, NodeChain& run, std::uint32_t bytes) noexcept;` — the run joins the tail (`splice_back`), RCV.NXT and the unread count move past `bytes`.
- Produces, `ControlBlock`: `ReassemblyQueue<Packet> reassembly{};` behind `window`; `held_segments()` is `receive.segments() + reassembly.segments()`; `clear()` asserts `!reassembly.holds()`.
- Produces, the Receiver:

```cpp
/// What the sequence check made of a segment.
enum class Sequenced : std::uint8_t {
    Refused,   ///< Dropped, counted and answered: it goes no further.
    InOrder,   ///< At RCV.NXT, trimmed to it, or occupying no sequence space inside the window.
    AboveGap,  ///< Payload or a FIN above RCV.NXT, inside the window.
};
[[nodiscard]] Sequenced accept_sequence(ControlBlock& block, Segment& segment) noexcept;
/// The payload and FIN of a segment above a gap, after its RST, SYN and ACK were dealt with.
void receive_above_gap(ControlBlock& block, Datagram& datagram, const Segment& segment) noexcept;
/// After an in-order segment was held with something held above: the run that now follows RCV.NXT joins the
/// in-order chain, and a remembered FIN it reaches is taken.
void join(ControlBlock& block) noexcept;
/// The connection is at its cap, or the pool is empty, and a segment that starts at `first` needs a node: the
/// lower sequence wins. True when the highest out-of-order node was released for it. The one place that
/// decides what gives way at the cap.
[[nodiscard]] bool make_room(ControlBlock& block, Sequence first) noexcept;
```

- Produces, `Connection`: `[[nodiscard]] std::uint32_t out_of_order_segments() const noexcept` and `[[nodiscard]] std::size_t out_of_order_bytes() const noexcept`.
- Produces, the Lifecycle: `set_closed` and `return_chain` call `block.reassembly.release(context_.pool)`; `Stack::~Stack` does too.

The procedure, which the signatures do not determine:

```
accept_sequence
  a segment that occupies no sequence space: as today (InOrder when acceptable); it is never AboveGap
  entirely before RCV.NXT: as today
  sequence after RCV.NXT:  window == 0 or end past the edge -> Refused (dropped_out_of_window, ACK restated)
                           otherwise                        -> AboveGap
  the rest: as today (InOrder, or Refused past the edge)

receive_synchronized
  PAWS, accept_sequence, timestamps.update, RST, SYN, receive_ack     (unchanged, and now reached above a gap)
  AboveGap -> receive_above_gap; return
  payload  -> receive_payload
  FIN      -> as today

receive_above_gap
  the peer's FIN was taken (receive.closed()), or the segment's data starts at or past a remembered FIN
      -> dropped_unexpected, ACK at once restated; return
  payload:
      at the cap, or the pool empty, and make_room refuses
          -> dropped_out_of_order, ack_policy.gap(end), ACK at once restated; return
      insert:  Held      -> ++out_of_order_held
               Duplicate -> ++dropped_duplicate
               NoNode    -> as when make_room refuses
  a FIN flag, when the payload was held or there was none -> reassembly.remember_fin(sequence + payload_length)
  ACK at once, restated

make_room
  highest = reassembly.highest(); none, or it does not start after `first` -> false
  released = reassembly.release_highest(pool); ++out_of_order_released; ack_policy.gap(released.end); true

receive_payload
  at the cap, or the pool empty: make_room(block, RCV.NXT) before refusing as today (dropped_no_slot at the cap,
      dropped_no_node with the pool empty)
  hold, raise Readable
  held_above = reassembly.holds();  held_above -> join
  ask the ACK policy as today, with RCV.NXT after the join; held_above -> the answer is Now

join
  run = reassembly.take_run(pool, receive.next()); receive.append(pool, run.chain, run.bytes)
  reassembly.fin() == receive.next() -> receive_fin(block)

receive_fin
  as today, and reassembly.release(pool): nothing above the peer's FIN is kept
```

- [ ] **Step 1: Write the failing tests**

`test_tcp_data.cpp` (`TcpReceive`), with the peer's default spec (no SACK, so plain duplicate ACKs) and `at(n)` for `peer_.snd_nxt() + n`:

- `ASegmentAboveAGapIsHeldAndDeliveredWhenTheGapFills`: 1000 bytes at `at(1000)` (pattern seed 2): `c->out_of_order_segments() == 1`, `c->unread().empty()`, `out_of_order_held == 1`, `dropped_out_of_order == 0`, no `Readable`. Then `peer_.data(pattern(1000, 1))`: one `Readable`, `c->unread().size() == 2000`, the bytes read are seed 1's then seed 2's, `c->out_of_order_segments() == 0`.
- `ARunIsDeliveredInOrderWhateverOrderItArrivedIn`: four 500-byte segments above a 500-byte gap in the order 3, 1, 4, 2, then the gap; 2500 bytes byte-exact.
- `AnOverlapAboveAGapIsTrimmedAndReadsByteExact`: `[1000, 2000)` then `[1500, 2500)` with a different seed, then the gap; the held bytes won `[1500, 2000)`; `dropped_duplicate == 0`.
- `ADuplicateAboveAGapIsCountedAndDropped`: the same segment twice; `dropped_duplicate == 1`, `out_of_order_held == 1`, `burst_empty()`.
- `AtTheCapTheLowerSequenceWins`: `configure_tcp({.receive_segments = 4})`, which makes the budget 2920 bytes, so the segments are 100 bytes each; four above a gap at `at(500)`, `at(1000)`, `at(1500)`, `at(2000)`; a fifth at `at(200)` is held and the one at `at(2000)` is gone (`out_of_order_released == 1`); the in-order segment is held and `at(1500)`'s is gone (`out_of_order_released == 2`); `c->out_of_order_segments() == 3`.
- `AtTheCapASegmentThatWouldBeTheHighestIsDropped`: the same four, then one at `at(2500)`: `dropped_out_of_order == 1`, `out_of_order_released == 0`, and the in-order data that follows is acknowledged at once until RCV.NXT passes `at(2600)`.
- `AnEmptyPoolTakesTheConnectionsOwnHighestNode`: `configure_tcp({.receive_segments = 4, .receive_pool = 4})` and two connections; the first holds three in-order packets unread, the second one out-of-order segment; an in-order segment for the second takes that node; an in-order segment for a connection that holds nothing out of order is dropped with `dropped_no_node`.
- `TheAckFieldOfASegmentAboveAGapTakesEffect`: the stack sends 100 bytes; the peer's segment above a gap acknowledges them; `c->acknowledged()` moved and `Acked` is raised.
- `APureAckAboveRcvNxtIsProcessedAndNotAnswered`: the stack sends 100 bytes; `peer_.segment({Ack}, {}, at(1000), ack of the 100 bytes)`; `c->acknowledged()` moved, `collect()` is empty, `pure_acks_sent` is unchanged, `c->out_of_order_segments() == 0`.
- `ARstAboveAGapResets` and `ASynAboveAGapIsDropped` (`dropped_unexpected`, nothing held).

`test_tcp_acks.cpp`:

- `ThreeGapSegmentsInOneBurstAttemptThreeDuplicateAcks` is rewritten: the three are held (`out_of_order_held == 3`), three duplicate ACKs leave at once, each restating the window; the segment that fills the gap is acknowledged at once, once, at the end of the run, and `c->unread().size()` counts everything.
- `InOrderDataBelowHeldDataIsAcknowledgedAtOnce`: a segment held at `at(2000)`; two 10-byte in-order segments are each answered before the flush.
- `AnAckForASegmentAboveAGapRestatesTheEdge`: 3000 bytes in and acknowledged, `consume(3000)`, a segment above a gap; its ACK states 62535.

`test_tcp_close.cpp`:

- `AFinAboveAGapTakesEffectWhenTheGapFills`: data with FIN at `at(1000)`; `PeerClosed` only after the gap is filled; the state is CloseWait and the ACK acknowledges the FIN's number.
- `ABareFinAboveAGapIsRemembered`: a bare FIN at `at(1000)` holds no packet (`out_of_order_segments() == 0`); filling the gap raises `PeerClosed`.
- `DataPastARememberedFinIsRefused`: `dropped_unexpected`, nothing held.
- `AnInOrderFinReleasesWhatIsHeldAbove`: a segment held at `at(2000)`, then an in-order FIN; `out_of_order_segments() == 0` and every node but the in-order ones is back.

`test_tcp_events.cpp`:

- `ATerminalEventReturnsTheOutOfOrderPackets`: 100 bytes in order unread and two segments held above a gap; a RST; `c->out_of_order_segments() == 0`, `tcp_->nodes_available()` is the pool less one, and `c->unread().size() == 100` until `release`.
- `ReleaseReturnsBothChains`: the same without the RST; after `c->release()` every node is available and the fabric's pool holds every packet again.

- [ ] **Step 2: Run them to see them fail**

- [ ] **Step 3: Implement the sequence check's third outcome, `receive_above_gap`, `make_room`, `join`, the append and the releases**

- [ ] **Step 4: Replace the script and add the area**

Delete `data/above-a-gap-is-acked-not-stored.pkt`. Add, each with its opening comment, against a peer that offers no SACK:

`reassembly/gap-filled-delivers-the-run.pkt`:

```
// A segment above a gap is held as the packet it arrived in and answered at once with a duplicate ACK that
// restates the window; the segment that fills the gap delivers both and is acknowledged at once. This peer
// offered no SACK, so the duplicate carries no blocks. Linux does the same.
0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0 bind(3, ..., ...) = 0
+0 listen(3, 1) = 0

+0 < S 0:0(0) win 65535 <mss 1460,nop,nop,TS val 100 ecr 0>
+0 > S. 0:0(0) ack 1 win 65535 <mss 1460,nop,nop,TS val 0 ecr 100>
+0 < . 1:1(0) ack 1 win 65535 <nop,nop,TS val 100 ecr 0>
+0 accept(3, ..., ...) = 4

+0 < P. 1:1001(1000) ack 1 win 65535 <nop,nop,TS val 100 ecr 0>
+0 > . 1:1(0) ack 1001 win 64535 <nop,nop,TS val 0 ecr 100>
+0 < P. 2001:3001(1000) ack 1 win 65535 <nop,nop,TS val 100 ecr 0>
+0 > . 1:1(0) ack 1001 win 64535 <nop,nop,TS val 0 ecr 100>
+0 read(4, ..., 3000) = 1000
+0 < P. 1001:2001(1000) ack 1 win 65535 <nop,nop,TS val 100 ecr 0>
+0 > . 1:1(0) ack 3001 win 62535 <nop,nop,TS val 0 ecr 100>
+0 read(4, ..., 3000) = 2000
```

  The numbers: the edge stands at 65536; `65536 - 1001 = 64535`; the `read` of 1000 moves the candidate 1000 bytes, less than a segment, and doubles nothing; `65536 - 3001 = 62535`.

and, derived the same way: `reassembly/overlap-is-trimmed.pkt`; `reassembly/covering-segment-replaces-a-held-one.pkt`; `reassembly/fin-above-a-gap.pkt` (the FIN's ACK only after the gap fills); `reassembly/cap-drops-the-highest.pkt` (`--tcp_receive_segments=4`: the budget is `4 * 1448 / 2 = 2896`, so its segments are 100 bytes).

- [ ] **Step 5: Run everything on the four presets, the scripts binary and both check scripts.** Expected: PASS; the scripts binary reports the old script gone and the five new ones in both modes and policies.

- [ ] **Step 6: Commit**

```bash
git add -A common/tcp tests
git commit -m "feat(tcp): hold segments above a gap as packets and join them when it fills"
```

---

## Task 7: SACK blocks, duplicate SACK, and three ACKs then the flush

Spec: "SACK blocks and duplicate SACK", "Acknowledgement timing" ("A burst above a gap"). Read `__tcp_ack_snd_check`, `tcp_sack_new_ofo_skb`, `tcp_sack_remove`, `tcp_dsack_set`, `tcp_dsack_extend` and `tcp_established_options` first.

**Files:**
- Modify: `common/tcp/connection/detail/sack_blocks.hpp`, `ack_policy.hpp`, `common/tcp/stack/detail/segment_writer.hpp`, `transmitter.hpp`, `receiver.hpp`, `lifecycle.hpp`
- Test: `tests/unit_tests/common/tcp/test_tcp_sack_blocks.cpp`, `test_tcp_ack_policy.cpp`, `test_tcp_sack.cpp` (all new, added to the CMake list); new scripts under `tests/scenarios/tcp/sack/`

**Interfaces:**
- Produces, `SackBlocks`, beside Task 4's negotiation:

```cpp
static constexpr std::size_t capacity = wire::TcpOptions::max_sack_blocks;

/// [first, end) is held above the gap, or was touched again: the block that contains or touches it is extended
/// over it, blocks it joins are merged, and the result moves to the front; the oldest falls off past four.
void held(wire::TcpSequence first, wire::TcpSequence end) noexcept;
/// RCV.NXT moved to `next`: blocks at or below it go, and one it passed in part starts at it.
void advanced(wire::TcpSequence next) noexcept;
/// Held data [first, end), the tail of its block, was released: the block ends at `first`, or goes.
void released(wire::TcpSequence first, wire::TcpSequence end) noexcept;
/// [first, end) arrived twice. One duplicate is pending: a second extends it when they touch, else is dropped.
void duplicate(wire::TcpSequence first, wire::TcpSequence end) noexcept;
/// A segment with payload arrived: a duplicate not yet reported is forgotten (Linux, `tcp_data_queue`).
void forget_duplicate() noexcept;
/// The next pure ACK carries the option: SACK is in use and a block is held or a duplicate pending.
[[nodiscard]] bool pending() const noexcept;
/// What the next pure ACK carries, at most `room` blocks: the duplicate first, then the blocks in order.
[[nodiscard]] std::size_t report(std::span<wire::TcpSackBlock, capacity> out, std::size_t room) const noexcept;
[[nodiscard]] bool duplicate_pending() const noexcept;
/// An ACK that carried the report left: the duplicate is forgotten.
void reported() noexcept;
/// The blocks and the duplicate go; the negotiation stays. At the peer's FIN and at a terminal event.
void drop() noexcept;
[[nodiscard]] std::size_t count() const noexcept;
```

  Every mutator but the negotiation does nothing when SACK is not in use. The blocks are kept as an array in report order (plan's call).
- Produces, `AckPolicy`: `Ack::Delayed` is renamed `Ack::Flush` ("by the next flush, unless a segment with an ACK leaves first"), in `Lifecycle::peer_closed` and the Receiver too; and

```cpp
/// A segment above a gap was held with RCV.NXT at `next`. Without SACK every one is answered at once; with it
/// the first three since RCV.NXT last moved, and the rest by the flush (Linux, `__tcp_ack_snd_check`).
[[nodiscard]] Ack on_out_of_order(wire::TcpSequence next, bool sack) noexcept;
```

  It owns the count and the RCV.NXT the count is for.
- Produces, `OutgoingSegment`: `std::span<const wire::TcpSackBlock> sack{};  ///< On a pure ACK.`; `size()` adds `wire::TcpOptions::sack_size(sack.size())` when it is not empty; the writer writes it after the timestamps.
- Produces, the Transmitter: `emit` gains `bool pure` (true from `send_bare_ack` only). A pure ACK with `block.sack.pending()` carries `block.sack.report(blocks, block.timestamps.enabled() ? 3 : 4)`; when it leaves, `sack_acks_sent` is counted, `duplicate_sacks_sent` when a duplicate was pending, and `block.sack.reported()` runs. `record_offer` keeps the block on the ACK list when the segment was not pure and `block.sack.pending()`: a data segment does not cancel an ACK that owes blocks.
- Produces, the Receiver's calls, each at the step the spec names:

```
a segment with payload enters receive_synchronized      -> sack.forget_duplicate()
refused as a duplicate, or by PAWS, starting before RCV.NXT -> sack.duplicate(sequence, min(end, RCV.NXT)), before the ACK at once
in order with an old prefix                              -> sack.duplicate(sequence, RCV.NXT)
above a gap, Held                                        -> sack.held(kept.first, kept.end)
above a gap, with a duplicate range                      -> sack.duplicate(range), then sack.held(range) so its block is first
a FIN remembered at `at`                                 -> sack.held(at, at + 1)
make_room, before it releases, with a FIN remembered at `at` -> sack.released(at, at + 1): the release forgets the FIN
make_room released [first, end)                          -> sack.released(first, end)
after a join                                             -> sack.advanced(RCV.NXT); run.duplicate -> sack.duplicate(range)
the peer's FIN taken; set_closed                         -> sack.drop()
the ACK for a segment above a gap                        -> ack_policy.on_out_of_order(RCV.NXT, sack.in_use()):
                                                            Now -> ack_now(block, Edge::Restate); Flush -> queue_ack
```

- [ ] **Step 1: Write the failing unit tests**

`test_tcp_sack_blocks.cpp`, on a `SackBlocks` with `offer(true); answer(true);`, a helper `blocks(sack, room = 4)` returning the report as a vector, and `B(a, b)` for a block:

| Test | Steps | Expects |
|---|---|---|
| `TheNewestBlockIsFirst` | `held(2000, 2100)`, `held(4000, 4100)`, `held(3000, 3100)` | `{B(3000,3100), B(4000,4100), B(2000,2100)}` |
| `ATouchingSegmentExtendsItsBlockAndMovesItToTheFront` | `held(2000, 2100)`, `held(4000, 4100)`, `held(2100, 2200)` | `{B(2000,2200), B(4000,4100)}` |
| `ASegmentThatJoinsTwoBlocksMergesThem` | `held(2000, 2100)`, `held(2200, 2300)`, `held(2100, 2200)` | `{B(2000,2300)}` |
| `TheFifthRangeDropsTheOldest` | five separate ranges | four blocks, the first one held is gone |
| `BlocksGoAsRcvNxtPassesThem` | `held(2000, 2100)`, `held(3000, 3100)`, `advanced(2100)` | `{B(3000,3100)}`; then `advanced(3050)` gives `{B(3050,3100)}` |
| `AReleaseCutsTheBlockBack` | `held(2000, 2300)`, `released(2200, 2300)` | `{B(2000,2200)}`; `released(2000, 2200)` leaves none and `pending()` false |
| `ThreeBlocksFitBesideTimestampsAndFourWithout` | four ranges | `blocks(sack, 3).size() == 3`, `blocks(sack, 4).size() == 4` |
| `ADuplicateIsFirstAndForgottenOnceReported` | `held(2000, 2100)`, `duplicate(900, 1000)` | `{B(900,1000), B(2000,2100)}`; after `reported()`, `{B(2000,2100)}` |
| `ASecondDuplicateExtendsThePendingOneOnlyWhenTheyTouch` | `duplicate(900, 950)`, `duplicate(950, 1000)`, `duplicate(1200, 1300)` | the report starts with `B(900,1000)` |
| `APayloadSegmentForgetsThePendingDuplicate` | `duplicate(900, 1000)`, `forget_duplicate()` | `pending()` false |
| `ADuplicateAloneIsPending` | `duplicate(900, 1000)` | `pending()`, `blocks == {B(900,1000)}` |
| `ARememberedFinJoinsTheBlockThatEndsAtIt` | `held(2000, 2100)`, `held(2100, 2101)` | `{B(2000,2101)}`; a bare one: `held(5000, 5001)` gives `B(5000,5001)` |
| `NothingIsHeldOrReportedWhenSackIsNotInUse` | `answer(false)` first, then `held`, `duplicate` | `pending()` false, `count() == 0` |
| `DropKeepsTheNegotiation` | `held`, `duplicate`, `drop()` | `count() == 0`, `in_use()` |
| `WrapsAcrossTheTopOfTheSequenceSpace` | `held(0xffffff00, 0xffffff80)`, `held(0xffffff80, 0x00000040)`, `advanced(0xffffffc0)` | `{B(0xffffffc0, 0x00000040)}` |

`test_tcp_ack_policy.cpp`: `ThreeAboveAGapGoAtOnceAndTheRestAtTheFlushWithSack` (`Now, Now, Now, Flush, Flush` for the same `next`); `EveryOneGoesAtOnceWithoutSack` (ten times `Now`); `TheCountStartsAgainWhenRcvNxtMoves` (`Now` ×3, `Flush`, then a new `next`: `Now`).

- [ ] **Step 2: Write the failing tests of the brick**

`test_tcp_sack.cpp`, on `TcpFixture` with `peer_.set_timestamps(100)`, `peer_.set_sack_permitted(true)` and `peer_.set_window_scale(7)` before the open unless the test says otherwise; `sacks(frame)` is `frames::sack_blocks_of(frame)` in the peer's sequence numbers:

- `EveryPureAckCarriesTheBlocksWhileDataIsHeld`: a segment above a gap; its ACK carries one block equal to its range, after the timestamps (`tcp_options_of` is 12 + 12 bytes); a `consume`-driven update at the flush carries it too; after the gap fills the ACK carries none.
- `ADataSegmentCarriesNoBlocksAndTheOwedAckStillGoes`: five segments above a gap in one `process` call (the fourth and fifth leave an ACK owed to the flush), then a `commit`; the data segment has no SACK option and the flush still sends one pure ACK with the block.
- `ABurstAboveAGapIsAnsweredByThreeAcksAndOneAtTheFlush`: thirty segments above one gap injected together and processed in one pass, as `ThreeGapSegmentsInOneBurstAttemptThreeDuplicateAcks` injects three; `collect_queued()` holds three pure ACKs, each restating the window; `collect()` one more, whose first block covers all thirty.
- `WithoutSackEverySegmentAboveAGapIsAnsweredAtOnce`: the same thirty against a peer that did not offer SACK-permitted; thirty pure ACKs before the flush, none after, none with a SACK option.
- `TheCountOfThreeStartsAgainWhenRcvNxtMoves`: three at once, the gap filled, a new gap: three at once again.
- `ADuplicateBelowRcvNxtIsReportedFirst`: a segment already delivered arrives again; the ACK at once has one block, its range; `duplicate_sacks_sent == 1`.
- `ADuplicatePrefixIsReportedAtTheOrdinaryAck`: an in-order segment whose first 100 bytes are old; nothing leaves before the flush; the flush's ACK reports `[sequence, RCV.NXT it met)`.
- `ADuplicateOfHeldDataIsFollowedByItsBlock`: a held segment arrives again; the ACK's blocks are the duplicate range, then the block that holds it.
- `HeldDataAnInOrderSegmentOverlappedIsReportedAtTheJoin`: held `[at(500), at(1000))`, then an in-order segment `[at(0), at(750))`; the ACK at once acknowledges `at(1000)` and its one block is `[at(500), at(750))`.
- `AReleaseForRoomCutsTheBlockInTheSameAck`: `configure_tcp({.receive_segments = 4})`, four held, a lower one arrives; the ACK's blocks no longer cover the released range.
- `ABareFinAboveAGapIsABlockOfItsOwn` and `AFinOnAHeldSegmentEndsItsBlock`.
- `ThreeBlocksBesideTimestampsAndFourWithout`: four separate ranges; three blocks with timestamps; on a connection without timestamps, four.
- `ARefusedAckKeepsItsReportForTheFlush` (on `TcpRefusingFixture`): `refuse()`, a duplicate below RCV.NXT and a segment above a gap; nothing leaves; `allow()` and `collect()`: one pure ACK with the duplicate first and the block after it.
- `AProbeAndAResetCarryNoBlocksTheyDoNotOwe`: an `abort` while data is held sends a RST without a SACK option.

- [ ] **Step 3: Run both sets to see them fail**

- [ ] **Step 4: Implement the blocks, the policy's count, the option on a pure ACK and the Receiver's calls**

- [ ] **Step 5: Add the scripts**

Against a peer that offers SACK-permitted, timestamps and `wscale 7` (so the windows are Task 4's: the first ACK after 1000 unread bytes states 46211). Each inbound line is its own tick in the runner, so no script shows three ACKs and then one; that is `ABurstAboveAGapIsAnsweredByThreeAcksAndOneAtTheFlush`.

`sack/held-segment-is-reported.pkt`, after the handshake of `handshake/passive-open-negotiates-sack-and-window-scale.pkt`:

```
+0 < P. 1:1001(1000) ack 1 win 512 <nop,nop,TS val 100 ecr 0>
+0 > . 1:1(0) ack 1001 win 46211 <nop,nop,TS val 0 ecr 100>
+0 < P. 2001:3001(1000) ack 1 win 512 <nop,nop,TS val 100 ecr 0>
+0 > . 1:1(0) ack 1001 win 46211 <nop,nop,TS val 0 ecr 100,nop,nop,sack 2001:3001>
+0 < P. 1001:2001(1000) ack 1 win 512 <nop,nop,TS val 100 ecr 0>
+0 > . 1:1(0) ack 3001 win 45961 <nop,nop,TS val 0 ecr 100>
+0 read(4, ..., 3000) = 3000
```

  The last window: the edge stands at 370689; `370689 - 3001 = 367688`, and `367688 / 8 = 45961`. The `read` doubles nothing.

and, derived the same way: `sack/newest-block-first.pkt`; `sack/three-blocks-with-timestamps.pkt`; `sack/four-blocks-without-timestamps.pkt` (`--tcp_timestamps=0`); `sack/duplicate-below-rcv-nxt.pkt`; `sack/duplicate-prefix.pkt`; `sack/duplicate-of-held-data.pkt`; `sack/overlapped-held-data-is-reported-at-the-join.pkt`; `sack/blocks-leave-as-the-gap-fills.pkt` (two gaps, filled one at a time); `sack/fourth-segment-above-a-gap-is-acked-at-the-flush.pkt` (four segments above one gap, a `read` that frees a full segment before the fourth: the first three ACKs restate the window and the fourth, an ordinary ACK, moves it).

- [ ] **Step 6: Run everything on the four presets, the scripts binary and both check scripts.** Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add common/tcp tests
git commit -m "feat(tcp): report held data with SACK blocks and duplicates with D-SACK, on pure ACKs"
```

---

## Task 8: Delayed ACK

Off by default; with `delayed_ack` set the ACK of in-order data waits on a second wheel timer. Spec: "Acknowledgement timing" (the first three paragraphs and the last).

**Files:**
- Modify: `common/tcp/connection/detail/ack_policy.hpp`, `control_block.hpp`, `common/tcp/stack/detail/stack_context.hpp`, `transmitter.hpp`, `receiver.hpp`, `lifecycle.hpp`, `common/tcp/stack/tcp_stack.hpp`
- Test: `tests/unit_tests/common/tcp/test_tcp_ack_policy.cpp`, `test_tcp_acks.cpp`; new scripts under `tests/scenarios/tcp/ack/`

**Interfaces:**
- Produces, `AckPolicy`:

```cpp
enum class Ack : std::uint8_t {
    Flush,    ///< By the next flush, unless a segment with an ACK leaves first.
    Delayed,  ///< When the delay runs out, unless a segment with an ACK leaves first.
    Now,      ///< Before the next segment is processed.
};
/// In-order data of `accepted` bytes moved RCV.NXT to `next`. At once while a refused end is ahead, while
/// anything is held above, and for the second full-sized segment; with `delay`, at once when two full segments'
/// worth of bytes is unacknowledged and `Delayed` otherwise.
[[nodiscard]] Ack on_data(std::size_t accepted, std::size_t full_size, wire::TcpSequence next, bool held_above,
                          bool delay) noexcept;
```

  `acknowledged()` also clears the bytes a delayed ACK covers. Task 6's "held above means Now" moves into `on_data`.
- Produces, `ControlBlock`: `TimerNode ack_timer;` built from a second trampoline, `fire_ack`, which the constructor and `StackContext`'s constructor take after `fire`; `clear()` asserts it is not armed.
- Produces, the Transmitter:

```cpp
/// The delay starts with the first segment the pending ACK covers and is not extended: arms only an unarmed timer.
void arm_ack(ControlBlock& block) noexcept;
void disarm_ack(ControlBlock& block) noexcept;
/// The delay ran out: the pure ACK now, counted in `delayed_acks_sent`; a refused one waits for the flush.
[[nodiscard]] bool send_delayed_ack(ControlBlock& block) noexcept;
```

  `record_offer` cancels an armed ACK timer: any segment that leaves with an ACK ends the delay.
- Produces, `Stack`: `static void on_ack_timer_fired(loop::Timer& timer) noexcept`, which sends the delayed ACK and then calls `lifecycle_.maybe_finish(block)` as the flush's loop does. The destructor cancels the timer. `Lifecycle::set_closed` and `free_slot` call `transmitter_.disarm_ack(block)`.
- Produces, the Receiver: `receive_payload` passes `context_.config.delayed_ack > core::Duration::zero()`; on `Ack::Delayed` it calls `transmitter_.arm_ack(block)` and does not queue the block. The handshake's ACK and a FIN's ACK are untouched.

- [ ] **Step 1: Write the failing tests**

`test_tcp_ack_policy.cpp`: `WithADelayOneSegmentWaits` (`Delayed`); `WithADelayTwoFullSegmentsWorthGoesAtOnce` (1448 then 1448: `Delayed`, `Now`; and 724 four times: `Delayed` ×3, `Now`); `WithADelayHeldAboveStillGoesAtOnce`; `AnAckThatLeftStartsTheBytesAgain`; `WithoutADelayTheRuleIsTheSecondFullSegment` (unchanged behaviour: `Flush`, `Now`).

`test_tcp_acks.cpp`, on a fixture that calls `configure_tcp({.delayed_ack = 40ms})`, stepping `advance(1ms)`:

- `OneSegmentIsAcknowledgedAtTheDeadline`: 100 bytes; `collect()` is empty now and after 39 steps; the fortieth step and `collect_queued()` give one pure ACK; `delayed_acks_sent == 1`.
- `TheDelayCountsFromTheFirstSegment`: 100 bytes at 0 ms and 100 more at 30 ms; one ACK at 40 ms covering both, none at 70 ms.
- `TwoFullSegmentsWorthIsAcknowledgedAtOnce`: two full segments; and four half-sized ones.
- `AWriteCarriesTheAckAndEndsTheDelay`: 100 bytes in, a `commit`; the data segment acknowledges them; forty more steps send nothing.
- `ASegmentAboveAGapIsAcknowledgedAtOnceAllTheSame`.
- `TheHandshakesAckAndAFinsAckStayAtTheFlush`: an active open's ACK and a FIN's ACK in Established leave at `collect()`, not 40 ms later.
- `AWindowUpdateFromConsumeGoesAtTheFlushAndCarriesTheAck`.
- `ARefusedDelayedAckWaitsForTheFlush` (on `TcpRefusingFixture`).
- `ReleaseWithADelayedAckPendingCancelsTheTimer`: 100 bytes in, `c->release()`, forty steps: nothing is sent and nothing asserts; the same with a RST from the peer in place of the release.

- [ ] **Step 2: Run them to see them fail**

- [ ] **Step 3: Implement the policy's delay, the second timer and its trampoline**

- [ ] **Step 4: Add the scripts**

`ack/delayed-ack-waits-for-the-deadline.pkt`:

```
// With a delay configured, the ACK of one in-order segment waits 40 ms, counted from that segment. The default
// is no delay: the ACK goes at the flush of the same tick. Linux delays by default, by an adaptive time.
--tcp_delayed_ack_ms=40
0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0 bind(3, ..., ...) = 0
+0 listen(3, 1) = 0

+0 < S 0:0(0) win 65535 <mss 1460,nop,nop,TS val 100 ecr 0>
+0 > S. 0:0(0) ack 1 win 65535 <mss 1460,nop,nop,TS val 0 ecr 100>
+0 < . 1:1(0) ack 1 win 65535 <nop,nop,TS val 100 ecr 0>
+0 accept(3, ..., ...) = 4

+0 < P. 1:101(100) ack 1 win 65535 <nop,nop,TS val 100 ecr 0>
+0.04 > . 1:1(0) ack 101 win 65435 <nop,nop,TS val 40 ecr 100>
```

and, derived the same way: `ack/delayed-ack-two-full-segments-at-once.pkt`; `ack/delayed-ack-rides-a-write.pkt`; `ack/delayed-ack-not-above-a-gap.pkt`.

- [ ] **Step 5: Run everything on the four presets, the scripts binary and both check scripts.** Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add common/tcp tests
git commit -m "feat(tcp): an optional delayed ACK on a timer of its own"
```

---

## Task 9: Two stacks on the fabric, and the DPDK ring

Spec: "Testing" ("Two Aloe stacks on the fabric"). No production code is expected to change; a failure here is a defect in Tasks 3 to 8.

**Files:**
- Test: `tests/unit_tests/common/tcp/test_tcp_two_stacks.cpp`, `tests/integration_tests/tcp/test_tcp_ring.cpp`

**Interfaces:**
- Consumes: `Connection::sack()`, `window_shift()`, `receive_budget()`, `out_of_order_segments()` and the counters of Task 2. The fabric's impairment (`fabric::Impairment{loss, duplicate, reorder, delay, jitter, seed, script}`) is set as the existing cases of this file set it.

- [ ] **Step 1: Write the tests**

- `BothOptionsAreNegotiatedBetweenTwoStacks`: after `establish`, both ends report `sack()`, `window_scaling()`, `window_shift() == 3`, `peer_window_shift() == 3` and `receive_budget() == 370688U`.
- `AReorderedStreamIsHeldAndDeliveredInOrder`: reordering of 0.3 on the client's egress; `run_echo(50000, 4000)` is true; the server's `out_of_order_held` is above zero and its `dropped_out_of_order` zero.
- `ALossBehindThreeDuplicateAcksIsRepairedWithoutATimeout`: a script that drops the fourth data frame the client sends, once, with the client sending at least eight segments behind it; the echo completes; the client's `retransmits` is zero and its `fast_retransmits` at least one; the server's `out_of_order_held` is above zero and its `sack_acks_sent` above zero.
- `LossBothWaysStillDeliversEveryByteInOrder`: 2% loss on both egresses, `run_echo(200000, 60000)`; the echo matches, whatever the counters say.
- `AWindowPast65535BytesIsUsed`, on `BasicTwoStacks<NoCongestionControl>` with the client configured `.send_segments = 128`: the server does not read; the client's `unacknowledged()` passes 65535 before `writable()` reaches zero, and the server's `unread().size()` does too.
- `TcpTwoStacksFourSlots`'s two cases keep their meaning with the cap pinned at four: the budget is 2920 bytes, so the window, not only the cap, now paces the sender; re-derive what each asserts from that, and keep `SmallSegmentsBeyondTheSlotsRecoverAfterOneRto` as the case where short segments meet the cap first.

The spec's limit applies: two Aloe stacks negotiate SACK, the sender does not read blocks until step 5, and behind a loss it sees three duplicates and then one ACK per tick. No test here expects new data to flow during a recovery.

In `test_tcp_ring.cpp`, the existing echo over DPDK's ring driver gains the same negotiation check after its handshake.

- [ ] **Step 2: Run** `ctest --preset debug -R 'Tcp|Ring'`, then `tsan`. Expected: PASS.

- [ ] **Step 3: Commit**

```bash
git add tests
git commit -m "test(tcp): two stacks under reordering and loss with the receive side in place"
```

---

## Task 10: The tap cases against Linux

Manual tests: they compile in every preset and run by hand as root. Spec: "Testing" ("Against Linux over the tap").

**Files:**
- Test: `tests/manual_tests/tcp/test_tcp_tap.cpp`

**Interfaces:**
- Consumes: the file's `TcpTap` fixture, its `tc(...)` helper and `readable_by`; `Connection::sack()`, `window_shift()`, `peer_window_shift()`.
- Produces: a suite `TcpTapReceive` derived from `TcpTap`, whose kernel end is the sender, and a helper that reads the kernel socket's `struct tcp_info` with `getsockopt(fd, IPPROTO_TCP, TCP_INFO, ...)`.

- [ ] **Step 1: Write the cases**

- `TheKernelNegotiatesBothOptionsInBothDirections`, once with the kernel connecting and once with the kernel listening on a port above 1024: on the stack, `sack()` and `window_scaling()` are true and `peer_window_shift()` is the kernel's; on the kernel socket, `tcpi_options` has `TCPI_OPT_SACK` and `TCPI_OPT_WSCALE` and `tcpi_snd_wscale` is 3.
- `AKernelSenderFillsAWindowPast65535Bytes`: the kernel writes 1 MB while the stack does not read; the stack's `unread().size()` passes 65535, and the kernel's `tcpi_snd_wnd`, read at the start, is above 65535. Then the stack reads everything, byte-exact.
- `UnderNetemReorderingTheKernelSeesNoTimeout`: `tc qdisc replace dev aloe-tcp root netem delay 5ms reorder 25% 50%`; the kernel sends 2 MB and the stack reads it byte-exact; the stack's `out_of_order_held` is above zero; the kernel socket's `tcpi_total_rto` is zero.
- `UnderNetemLossTheKernelRecoversFromTheBlocks`: `tc qdisc replace dev aloe-tcp root netem loss 2%`; the kernel sends 2 MB, read byte-exact; the stack's `sack_acks_sent` is above zero; on the kernel socket `tcpi_total_retrans` is above zero and `tcpi_total_rto` is at most a quarter of it. A lost retransmission or a lost tail costs the kernel a timeout whatever the receiver does, which is why the bound is a share and not zero.

Each skips, as `TcpTapLoss` does, when `tc` or `sch_netem` is missing, and removes the qdisc afterwards. `tcpi_total_rto` is in `struct tcp_info` from Linux 6.7; if the build machine's `<linux/tcp.h>` lacks it, read the deltas of `TcpExtTCPTimeouts` and `TcpExtTCPSackRecovery` from `/proc/net/netstat` on an otherwise idle machine and say so in the case's comment.

- [ ] **Step 2: Build on the four presets**

Run: `cmake --build --preset debug` and the others. Expected: the manual binary builds; `ctest` does not run it.

- [ ] **Step 3: Run by hand where root is available**

Run: `sudo ./build/debug/tests/manual_tests/tcp/Aloe.Tests.Manual.Tcp.Tap --gtest_filter='TcpTapReceive.*'`
Expected: PASS. Without root the cases skip; record the run as owed in the hand-off and in the roadmap line of Task 12.

- [ ] **Step 4: Commit**

```bash
git add tests/manual_tests
git commit -m "test(tcp): the receive side against a Linux sender over the tap"
```

---

## Task 11: The benchmark

Spec: "The tick path", "Fixtures and the scenario runner" (the venue).

**Files:**
- Modify: `benchmarks/tick_to_trade/venue_peer.cpp`, `benchmarks/tick_to_trade/stack_sanity.hpp`
- Modify: `docs/benchmarks/tick-to-trade.md`; create the result files under `docs/benchmarks/results/`

- [ ] **Step 1: The venue offers what a Linux venue offers**

Where `venue_peer.cpp` sets its timestamps, call `script_.set_sack_permitted(true)` and `script_.set_window_scale(7)` before the first SYN or SYN-ACK is built. Wherever the venue reads the stack's window, read `script_.stack_window_bytes()`.

- [ ] **Step 2: The sanity list**

`stack_sanity.hpp` expects zero for `out_of_order_held`, `out_of_order_released`, `sack_acks_sent`, `duplicate_sacks_sent` and `delayed_acks_sent`, beside the drop counters it lists today: a clean ring never reorders.

- [ ] **Step 3: Build and test with the benchmarks on**

Run: `cmake --preset debug -DDO_BENCHMARKS=ON && cmake --build --preset debug && ctest --preset debug`
Expected: PASS, the benchmark's own tests included.

- [ ] **Step 4: The rerun**

On one machine, `release`, on hugepages: build `main` at the branch's base and the branch's head; run each three times, alternately (base, head, base, head, base, head), with the options the benchmark page's last recorded run used; compare by medians, with the `tcp` leg broken out. The bar is no change beyond run-to-run noise, which the stack-split comparison put at 10 to 20 ns, at p50 and p99 on the 64-byte tick at level 0, cross-core.

A change beyond the bar is not merged silently. In this order: find it (the candidates are the store in `SackBlocks::forget_duplicate` per data segment, the `sack.pending()` load on a data segment's send, and the offer's extra compare); fix it; if it stands, the first fix the spec names is reading the usual `nop, nop, TS` block with one 32-bit compare in place of the option walk; and what cannot be fixed is recorded on the benchmark page with its number and its reason.

Where the machine at hand has no hugepages or the run needs `sudo`, stop there and record the rerun as owed.

- [ ] **Step 5: Record and commit**

The benchmark page gains the rerun and its result files, or the sentence that it is owed.

```bash
git add benchmarks docs/benchmarks
git commit -m "bench(tick-to-trade): a venue that offers SACK and window scaling, and the rerun"
```

---

## Task 12: Documentation, the traps, the gates and the pull request

**Files:**
- Modify: `docs/architecture/tcp.md`, `docs/architecture/scenario.md`, `docs/architecture/fixtures.md`, `docs/roadmap.md`, `AGENTS.md`, `README.md` only if its module table's `tcp` row names what this step changes

- [ ] **Step 1: `docs/architecture/tcp.md`**

- Key types: `TcpConfig` and `TcpCounters` lines name the new fields; the `Connection` line names the new accessors.
- Configuration and Counters: the rows of Task 2 with the final defaults (`receive_segments` 512 and its new meaning); the sizing note gains two sentences: with the defaults a connection offers about 370 KB over 512 slots, and `receive_pool` at 2048 is what four connections stalled at the default cap hold between them, while the cap is per connection.
- Design notes rewritten: "Two limits on what is held, independent of each other" (the budget as half of what the slots hold, why, and what `receive_window` and `receive_segments` do for a feed of small segments; a segment at the cap is dropped and recovered by the peer's resend, and the offered window is never taken back); "ACKs wait, with exceptions" (the full list of what goes at once, the three-then-flush rule with SACK and every one without, the restated edge, the delay and its default); "What is still left out, and why" (loses the receive side; keeps what steps 5 and 6 own, with the two entries the spec adds: a floor under the peer's MSS, and the wider window before RFC 5961).
- New notes: the out-of-order hold (the second chain, the three insert rules, the join that cannot fail, a FIN above a gap as one number, the lower sequence winning at the cap); where SACK blocks ride and why a data segment never carries them; duplicate SACK and its four sources; the scaled window, the smallest shift, the rounding up and the creep it allows; the peer's scaled window; that two Aloe stacks send no new data during a recovery until step 5.
- "One class per concern" names the new parts and what each owns.

Nothing in it links to or names `docs/superpowers/`.

- [ ] **Step 2: The other pages**

- `docs/architecture/scenario.md`: the five option lines; the new scripts in its table, by area; the failure example with a current window; the paragraph on how Aloe's scripts differ from Linux's loses the SACK, window-scale and out-of-order sentences and keeps the delayed ACK's default; a sentence that each inbound line is its own tick, so a burst's three ACKs and one are a brick test.
- `docs/architecture/fixtures.md`: `TcpPeer::set_window_scale`, `set_sack_permitted`, `stack_window_bytes`; `sack_blocks_of`.
- `docs/roadmap.md`: step 4 landed, in the style of step 3's entry, with what is owed to a machine with root (the tap cases, the benchmark rerun if not run).
- `AGENTS.md`: the opening paragraph ("minimal on the receive side" no longer holds) and the layout row for `tcp` (the new parts); under "Traps already found", the traps this work found, and at least these if nothing replaced them: the default budget against a peer without window scaling is 65535 and a script's `win` is the raw field, so the same window reads `46336` scaled and `65535` unscaled; a `TcpPeer` offers neither option until told; a test of the cap pins `receive_segments` because the fixtures' pools are smaller than 512; each inbound script line is its own tick; the window edge creeps right under rounding, so no test bounds it by the budget.

- [ ] **Step 3: The gates**

```bash
for preset in debug gcc-debug asan tsan; do
  cmake --preset $preset && cmake --build --preset $preset && ctest --preset $preset || break
done
./scripts/check-format.sh && ./scripts/check-includes.sh
cmake --preset debug -DDO_BENCHMARKS=ON && cmake --build --preset debug && ctest --preset debug
```

Expected: every preset passes; both scripts pass. If a `gcc-debug` scenario binary aborts, rebuild it with `--clean-first` before anything else.

- [ ] **Step 4: The whole-branch review**

A fresh reviewer reads the branch against the spec's "Goals", "Anti-goals" and "Testing", and checks by search: no copy of payload on a receive path (`grep -n "ranges::copy\|memcpy" common/tcp/stack/detail/receiver.hpp common/tcp/connection/detail/re*.hpp` shows none); every `TcpSequence` comparison in the new parts is `before`, `after` or the signed difference, never `.value <`; the Receiver, the Transmitter and the Lifecycle gained no data members.

- [ ] **Step 5: Commit, and the pull request**

```bash
git add docs AGENTS.md README.md
git commit -m "docs: the receive side"
git add -f docs/superpowers/specs/2026-11-12-receive-side-design.md docs/superpowers/plans/2026-11-12-receive-side.md
git commit -m "docs(tcp): add the receive side's spec and plan"
```

The pull request: `[TCP][WIRE][FRAMES][SCENARIO] Receive side: out-of-order segments held as packets, SACK blocks, window scaling and delayed ACK`. Its description says what changed and why in a few lines, then how it was verified, and what is owed to a machine with root. It closes #19 with a comment that states what landed and what is open: the tap run and the rerun if owed; congestion-window growth on an application-limited flow, which goes with step 5; and #39, the window at the cap, which the author holds.

---

## Self-Review Notes

- **Spec coverage.** "The life of a received segment", "Room" and "The end of a connection": Task 6. "The reassembly queue": Tasks 5 and 6. "SACK blocks and duplicate SACK": Tasks 4 (negotiation) and 7. "The receive window": Tasks 3 and 4. "The peer's window" and "The handshake's options": Task 4. "Acknowledgement timing": Tasks 3 (the restated edge), 6, 7 and 8. "Configuration, counters and what the application sees": Tasks 2, 3, 4 and 6. "Parts, and where the state lives": the Interfaces blocks. "The tick path": Task 11. "Fixtures and the scenario runner": Tasks 1, 2, 4 and 11. "Testing": every task, with the two-stack, tap and benchmark runs in Tasks 9 to 11. "Documentation" and "Delivery": Task 12.
- **What the plan decided where the spec left a choice** (each marked "plan's call" or stated in its task): the counter names; the accessors' names; the blocks kept as an array in report order; the node's `first` written by the reassembly queue only; the three-way result as `Receiver::Sequenced`; the ACK timer's trampoline on the façade beside the first; `Edge` as the name of what a duplicate ACK restates; the window lands unscaled first so the scripts are re-derived once; `TCP_INFO` as the tap cases' evidence.
- **What the plan could not check.** No code here was compiled. The script numbers are derived by hand from the arithmetic under "Read This First"; if the built `ReceiveWindow` passes its unit tests and a script still disagrees, check the script's derivation before the code.
