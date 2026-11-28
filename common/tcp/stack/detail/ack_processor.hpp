#pragma once

#include <algorithm>
#include <aloe/core>
#include <aloe/wire>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>

#include <congestion_control.hpp>
#include <detail/control_block.hpp>
#include <detail/loss_recovery.hpp>
#include <detail/rack.hpp>
#include <detail/stack_context.hpp>
#include <detail/tail_probe.hpp>
#include <detail/transmitter.hpp>
#include <tcp_config.hpp>

namespace aloe::tcp::detail {

    /**
     * @brief What an acknowledgement does to the send side, in the order of Linux's `tcp_ack` and
     * `tcp_fastretrans_alert`: the peer's blocks into the scoreboard; the entries the cumulative part frees, the
     * round-trip sample and the retransmission timer; RACK's state, from the entries both newly report; what the ACK
     * says of a tail probe that is out; the undo of a reduction a needless resend caused; detection, by RACK on a
     * connection that uses SACK and by the duplicate count on one that does not; the recovery state, which begins, goes
     * on or ends, with the window in recovery; the resends, as far as that window allows; the peer's window, the
     * zero-window probes and the FIN that waited for the window; and the send timer. It owns no state; it drives the
     * block's send side through the Transmitter, in place, and tells the Receiver what moved. It names neither the
     * Receiver nor the Lifecycle: an ACK ends a connection only through what the Receiver does with the outcome.
     */
    template <typename Ip, typename CongestionControl>
    class AckProcessor {
    public:
        using ControlBlock    = detail::ControlBlock<Ip, CongestionControl>;
        using Context         = StackContext<Ip, CongestionControl>;
        using Transmitter     = detail::Transmitter<Ip, CongestionControl>;
        using Sequence        = wire::TcpSequence;
        using Freed           = Transmitter::Freed;
        using Reported        = Transmitter::Reported;
        using SentNode        = Transmitter::SentNode;
        using DuplicateReport = Transmitter::DuplicateReport;

        /// What an acceptable ACK did, for the Receiver to publish.
        struct Outcome {
            bool advanced = false;  ///< SND.UNA moved forward: `Acked`, and our FIN may be acknowledged.
            bool writable = false;  ///< `writable()` grew: `Writable`.
        };

        AckProcessor(Context& context, Transmitter& transmitter) noexcept
            : context_{context},
              transmitter_{transmitter} {
        }

        /// The ACK that completes a handshake, of our SYN or SYN-ACK: its entry freed, SND.UNA at `ack`, a sample
        /// taken and the timer stopped with the queue empty (`on_acknowledged`). On a connection that uses SACK the
        /// freed entry gives RACK its first round trip, as Linux's `tcp_clean_rtx_queue` does for the SYN.
        void handshake_acknowledged(ControlBlock& block,
                                    const Sequence ack,
                                    const std::optional<std::uint32_t> echo) noexcept {
            std::ignore = acknowledge(block, ack, echo, block.queue.highest_reported());
            assert(!transmitter_.in_flight(block) && "the handshake's ACK is of SND.NXT: no RTO is left to arm");
        }

        /**
         * An ACK on a synchronized connection, of nothing past SND.NXT and nothing before `SendSequence::ack_floor`,
         * in the order of Linux's `tcp_ack` and `tcp_fastretrans_alert`:
         *
         * 1. The SACK option's blocks, `sack`, eight bytes each and empty without one, read into the scoreboard
         *    (`read_blocks`), on an ACK below SND.UNA too; each entry they newly report advances RACK. A believed
         *    duplicate report that is not the tail probe's and reports resent data (`reports_resend`) is one resend
         *    fewer for the undo to wait for (`LossRecovery::on_duplicate_report`), whatever else this ACK does. The
         *    probe's own report, while its verdict is open, settles that verdict and takes nothing off the undo's
         *    count, which is an earlier episode's: a departure from Linux, whose `tcp_check_dsack` takes it off
         *    `undo_retrans` as any other. A probe an episode forgot has no verdict open, and its report counts as any
         *    other against that episode's count, which counted the probe if it was still in flight
         *    (`LossRecovery::count_probe`).
         * 2. An ACK of new data frees what it covers, advancing RACK by each entry no block had reported, and samples
         *    the round trip (`acknowledge_data`). A duplicate ACK, of something in flight, is judged against the
         *    window as it stands, before this segment updates it, the field scaled by the peer's shift, and counted.
         * 3. RACK's state: the reordering window's round with the duplicate report (`Rack::on_ack`), then reordering,
         *    when the blocks or the cumulative part showed it. A duplicate report grows the window only when reordering
         *    was seen before this ACK, as Linux's `tcp_dsack_seen` reads `reord_seen` before its walk records any, and
         *    never when it is the tail probe's own (`reports_probe`), which is no sign of reordering.
         * 4. The tail probe's ACK, while its verdict is open (`on_probe_ack`): needless, a loss it repaired, or nothing
         *    yet, as Linux's `tcp_process_tlp_ack` runs before `tcp_fastretrans_alert`.
         * 5. Reneging: an ACK that moved SND.UNA and left a head the peer reported received arms the RTO short
         *    (`Transmitter::arm_reneging`), and nothing more of this ACK runs until the peer's window, as Linux's
         *    `tcp_check_sack_reneging` returns from `tcp_fastretrans_alert`. Where this departs from Linux: `tcp_ack`
         *    still runs `tcp_cong_control` on that ACK, a step of the rate reduction or the policy's growth, where this
         *    takes no step of the policy or of the rate reduction.
         * 6. The undo's checks (`try_undo`), before anything more is marked, as Linux tries its undos in
         *    `tcp_fastretrans_alert` before `tcp_identify_packet_loss`: an undo may end the episode the ACK found, and
         *    a partial ACK that answers an original while a resend is out holds the rest of this ACK back from marking
         *    and resending.
         * 7. The end of an episode: an ACK that covers `recover` in recovery or in the loss state ends it here, before
         *    detection (`end_on_this_ack`), unless an undo ended it, as Linux's `tcp_fastretrans_alert` settles its
         *    state's exit (its step D) before it looks for losses (step E). An episode ended either way forgets its
         *    first resend's stamp when no resend of an episode is unacknowledged any more
         *    (`LossRecovery::forget_first_resend`), as Linux's `tcp_try_to_open` clears `retrans_stamp` unless
         *    `tcp_any_retrans_done`, and `tcp_try_undo_partial` the same. Detection on that ACK then reads the
         *    reordering window of the open state it leaves, which a reordering timer it arms reads too unless a new
         *    episode begins on that ACK, whose recovery window the timer then reads. On every other ACK detection
         *    comes first and reads the window of the state the ACK found, which the recovery state's step then moves
         *    on.
         * 8. Detection: RACK's, on a connection that uses SACK, when its state moved, and after every undo, which
         *    makes RACK look at the queue again whether or not it moved, as Linux's `tcp_undo_cwnd_reduction` sets
         *    `rack.advanced` (`Transmitter::detect`); without SACK, the duplicate count's, in the recovery state's
         *    step.
         * 9. The recovery state (`update_recovery`): on an ACK that ended an episode, by covering `recover` or by an
         *    undo, a new one when something is marked lost; on any other, by the state the ACK found. Then the resend
         *    pass, which sends what is marked lost as far as the window allows.
         * 10. The peer's window: RFC 793 updates it only when SND.UNA <= SEG.ACK <= SND.NXT, a stale ACK carrying a
         *    stale window; then the zero-window probes and the FIN that waited for the window.
         * 11. The send timer (`Transmitter::set_timer`), after an ACK that ran detection or moved SND.UNA: the
         *    reordering timer, the tail probe or the RTO. The cumulative part recorded the RTO's moment without arming,
         *    so an ACK that moved SND.UNA arms the wheel here, and before only for a resend of the head
         *    (`Transmitter::resend_lost`) or for the FIN step 10 lets out, a FIN that waited for the window whose send
         *    arms the timer as every first send does (`Transmitter::arm_after_send`): then it may be armed twice.
         *    Any other ACK leaves the timer as it was, a reordering timer still waiting for its entries or a probe's
         *    included.
         *
         * `payload_length` is the segment's, since an ACK that carries data is no duplicate.
         */
        [[nodiscard]] Outcome on_ack(ControlBlock& block,
                                     const wire::TcpHeader& header,
                                     const std::uint16_t payload_length,
                                     const std::optional<std::uint32_t> echo,
                                     const std::span<const std::byte> sack) noexcept {
            const Sequence ack                = header.acknowledgement;
            const std::size_t writable_before = transmitter_.writable(block);
            const Found found{.state    = block.recovery.state(),
                              .reported = block.queue.reported_bytes(),
                              .highest  = block.queue.highest_reported()};
            const Blocks blocks     = read_blocks(block, ack, sack);
            // RFC 5681's duplicate ACK but for the data in flight it requires: no data, no SYN or FIN, no advance, the
            // window as it stands.
            const bool repeated     = payload_length == 0 && !header.flags.has(wire::TcpFlag::Syn) &&
                                      !header.flags.has(wire::TcpFlag::Fin) && ack == block.send.una &&
                                      block.send.scaled(header.window) == block.send.wnd;
            const bool duplicate    = repeated && transmitter_.in_flight(block);
            const bool probe_report = reports_probe(block, blocks);
            if (blocks.duplicate && !probe_report && reports_resend(block, *blocks.duplicate)) {
                block.recovery.on_duplicate_report(blocks.duplicate->end);
            }
            Outcome outcome{};
            std::optional<AckInfo> info{};
            std::uint8_t duplicates = 0;  // the count with this ACK, when it is a duplicate
            bool reordering         = blocks.reordering;
            if (ack.after(block.send.una)) {
                const Acknowledged acknowledged = acknowledge_data(block, ack, echo, found.highest);
                info                            = acknowledged.info;
                reordering                      = reordering || acknowledged.reordering;
                outcome.advanced                = true;
            } else if (duplicate) {
                ++context_.counters.duplicate_acks;
                duplicates = block.recovery.count_duplicate();
            }
            block.rack.on_ack(block.send.una, block.send.nxt, blocks.duplicate.has_value() && !probe_report);
            if (reordering) {
                block.rack.see_reordering();
                ++context_.counters.reordering_seen;
            }
            if (block.probe_state.verdict_open()) {
                on_probe_ack(block, ack, probe_report, repeated && sack.empty());
            }
            const bool reneged = outcome.advanced && transmitter_.head_reported(block);
            Rack::Detected detected{};
            bool detecting = false;
            if (reneged) {
                transmitter_.arm_reneging(block);
            } else {
                const Undone undone = try_undo(block, found.state, ack, echo, info, reordering);
                const bool ended =
                    undone.reopened || (found.state != RecoveryState::Open && block.recovery.recovered(ack));
                if (ended && !undone.reopened) {
                    end_on_this_ack(block, found, ack, info);
                }
                if (ended && !transmitter_.resent_unacknowledged(block)) {
                    block.recovery.forget_first_resend();  // Linux's `tcp_try_to_open`
                }
                detecting = block.sack.in_use() && !undone.held && (block.rack.advanced() || undone.undone);
                if (detecting) {
                    detected = transmitter_.detect(block);
                }
                update_recovery(block, found, ack, duplicates, info, detected, ended, undone.held);
                if (!undone.held) {
                    transmitter_.resend_lost(block);
                }
            }
            if (!ack.before(block.send.una)) {
                block.send.update_window(header);
                update_probing(block);  // a window that opened ends probing, and the comparison below sees it
                send_unsent(block);     // and sends the FIN a close left waiting for it
            }
            if (!reneged && (detecting || outcome.advanced)) {
                transmitter_.set_timer(block, detected.wait);
            }
            outcome.writable = transmitter_.writable(block) > writable_before;
            return outcome;
        }

        /**
         * The end of an acceptable ACK, after its window update: the peer answered, so the probes' count and stamp
         * clear (`ZeroWindowProbe::answered`). A window that opened ends probing; a zero window with nothing in flight
         * starts it: in Established or CloseWait whether or not the application has data, and in any state while a
         * FIN the window did not cover waits in the queue (`close`). A head waiting for the next hop is the ARP poll's.
         */
        void update_probing(ControlBlock& block) noexcept {
            block.probe.answered();
            if (block.probing()) {
                if (block.send.wnd > 0) {
                    transmitter_.disarm(block);
                }
                return;
            }
            if (block.send.wnd == 0 && !transmitter_.in_flight(block) && block.timer_kind != TimerKind::ArpPoll &&
                (!block.queue.empty() || block.state == State::Established || block.state == State::CloseWait)) {
                transmitter_.start_probing(block);
            }
        }

    private:
        /// The connection as an ACK found it, before its blocks and its cumulative part.
        struct Found {
            RecoveryState state    = RecoveryState::Open;
            std::uint32_t reported = 0;  ///< Sequence space reported received.
            /// The highest sequence the peer had acknowledged either way, Linux's `prior_fack`: a hole below it that
            /// the cumulative part fills with an entry sent once was reordered.
            std::optional<Sequence> highest{};
        };

        /// What the cumulative part of an ACK did: the entries it freed, whether one of them, never resent and
        /// reported by no block, lay below the highest sequence reported before, which is reordering, and the
        /// round-trip sample taken.
        struct Cumulative {
            Freed freed{};
            bool reordering = false;
            std::optional<core::Duration> rtt{};
        };

        /// What an ACK of new data did: what the policy reads of it, and whether it showed reordering.
        struct Acknowledged {
            AckInfo info{};
            bool reordering = false;
        };

        /// What the undo's checks did on one ACK (`try_undo`).
        struct Undone {
            bool undone   = false;  ///< A reduction was taken back: RACK looks at the queue again on this ACK.
            bool reopened = false;  ///< That ended the episode the ACK found: the connection is open.
            /// A partial ACK answered an original while a resend of the episode is out: it marks and resends nothing.
            bool held     = false;
        };

        /// What an ACK's SACK option said.
        struct Blocks {
            std::uint32_t reported = 0;  ///< Sequence space newly marked received.
            bool reordering        = false;
            std::optional<wire::TcpSackBlock> duplicate{};  ///< A believed duplicate report.
            /// A duplicate report no longer than the largest window the peer offered, believed or not: what the tail
            /// probe's verdict reads (`reports_probe`).
            std::optional<wire::TcpSackBlock> recognised{};
        };

        /**
         * The first step of an acceptable ACK: its SACK option read into the scoreboard. Nothing on a connection that
         * does not use SACK or with no option. An ACK older than SND.UNA by more than the largest window the peer
         * offered, whose blocks `tcp_sacktag_write_queue` ignores, never reaches it: the Receiver drops every ACK
         * before `SendSequence::ack_floor`, which is never further back. The duplicate report is recognised first, from
         * the option alone, kept for the tail probe's verdict, and counted when believed
         * (`RetransmissionQueue::duplicate_report`); then each other block that is valid marks the entries it covers
         * whole, lowest first, as Linux sorts them, so lower data reported in the same option is not taken for
         * reordering. A believed duplicate report that is valid and names a resent entry a block reported shows
         * reordering. Blocks a correct peer never sends mark nothing and are not an error.
         *
         * One departure from Linux: a duplicate report of the highest entry reported, when that entry was resent, shows
         * reordering as one of a lower entry does, where `tcp_check_sack_reordering` returns for a sequence at or above
         * the highest reported (`tcp_highest_sack_seq`) and counts reordering only below it.
         */
        [[nodiscard]] Blocks
        read_blocks(ControlBlock& block, const Sequence ack, const std::span<const std::byte> sack) noexcept {
            if (!block.sack.in_use() || sack.empty()) {
                return {};
            }
            ++context_.counters.sack_acks_received;
            const std::size_t count =
                std::min(sack.size() / wire::TcpOptions::sack_block_size, wire::TcpOptions::max_sack_blocks);
            const Sequence edge            = ack.after(block.send.una) ? ack : block.send.una;
            const wire::TcpSackBlock first = wire::TcpOptions::read_sack_block(sack, 0);
            const std::optional<wire::TcpSackBlock> second =
                count > 1 ? std::optional{wire::TcpOptions::read_sack_block(sack, 1)} : std::nullopt;
            Blocks blocks{};
            const DuplicateReport report = transmitter_.duplicate_report(block, first, second, ack);
            if (report != DuplicateReport::None) {
                blocks.recognised = first;
            }
            if (report == DuplicateReport::Believed) {
                blocks.duplicate  = first;
                blocks.reordering = valid_block(block, first, edge) && transmitter_.names_reported_resend(block, first);
                ++context_.counters.duplicate_sacks_received;
            }
            std::array<wire::TcpSackBlock, wire::TcpOptions::max_sack_blocks> ordinary{};
            std::size_t used = 0;
            for (std::size_t index = blocks.duplicate ? 1 : 0; index < count; ++index) {
                if (const wire::TcpSackBlock candidate = wire::TcpOptions::read_sack_block(sack, index);
                    valid_block(block, candidate, edge)) {
                    ordinary[used++] = candidate;
                }
            }
            const std::span<wire::TcpSackBlock> valid = std::span{ordinary}.first(used);
            std::ranges::sort(valid, [](const wire::TcpSackBlock& one, const wire::TcpSackBlock& other) noexcept {
                return one.first.before(other.first);  // valid blocks lie within the window: serial order is total
            });
            for (const wire::TcpSackBlock& sack_block : valid) {
                // Each entry newly reported advances RACK (Linux's `tcp_sacktag_one`); a block is no cumulative ACK,
                // so no echo judges a resend here, only the minimum round trip.
                const Reported reported =
                    transmitter_.report(block, sack_block, [this, &block](const SentNode& entry) noexcept {
                        block.rack.advance(entry.sent, entry.first + entry.length, entry.resent(), false, context_.now);
                    });
                blocks.reported   += reported.bytes;
                blocks.reordering  = blocks.reordering || reported.reordering;
            }
            return blocks;
        }

        /**
         * A duplicate report is of resent data, the spec's condition for the undo's count, read before the cumulative
         * part moves SND.UNA. Above SND.UNA the data is still queued: the report must name an entry that was resent
         * (`RetransmissionQueue::names_resend`), as Linux's `tcp_sacktag_one` counts down only a buffer with
         * `TCPCB_RETRANS`, so a report of an original the network duplicated counts nothing. At or below it the queue
         * no longer holds the data, and the report counts as it is, as `tcp_check_dsack` counts data it has forgotten;
         * the belief in the report, no more duplicates reported than segments ever resent, is all that guards it there.
         */
        [[nodiscard]] bool reports_resend(const ControlBlock& block, const wire::TcpSackBlock& report) const noexcept {
            return !report.end.after(block.send.una) || transmitter_.names_resend(block, report);
        }

        /**
         * The ACK's duplicate report is the tail probe's: of at most one segment, ending at the end of a probe whose
         * verdict is open, so the original and the probe both arrived. RFC 8985 7.4's "DSACK option matching
         * TLP.end_seq", Linux's `FLAG_DSACK_TLP`, in `tcp_dsack_seen`'s order: after its first test, a report longer
         * than the largest window the peer offered is none (`Blocks::recognised`), and before its second, the count of
         * duplicates against resends, so a report of the probe settles its verdict believed or not. The count still
         * gates all the rest: only a believed report is counted, undoes, or grows RACK's window (`Blocks::duplicate`).
         */
        [[nodiscard]] static bool reports_probe(const ControlBlock& block, const Blocks& blocks) noexcept {
            const std::optional<Sequence> end = block.probe_state.end();
            return end && blocks.recognised && blocks.recognised->end == *end &&
                   static_cast<std::uint32_t>(blocks.recognised->end - blocks.recognised->first) <= block.mss();
        }

        /**
         * The tail probe's ACK, RFC 8985 7.4's `TLP_process_ack` and Linux's `tcp_process_tlp_ack`
         * (`TailProbe::on_ack`), with `plain_duplicate` an ACK that repeats SND.UNA and the window, carries nothing and
         * has no SACK option. A probe both copies of which arrived is counted as needless. One that repaired a loss, an
         * ACK past its end with no duplicate report of it, tells the policy of that loss once, with the flight as a
         * recovery's beginning reads it, and its window becomes the target at once (`LossRecovery::on_repaired`): no
         * state entered, nothing resent, no undo kept. An ACK on the probe's end that says neither leaves the verdict
         * open, for a later ACK or a second probe (`Transmitter::on_tail_probe_timeout`).
         *
         * RFC 8985 7.4.2 reads a plain duplicate as "a DupAck without any SACK option", as here, where Linux's test is
         * that no block newly reported data (`FLAG_DATA_SACKED`): a duplicate that repeats old blocks closes the
         * verdict there and leaves it open here. Neither needs data in flight, which RFC 5681's duplicate ACK does: the
         * probe of a lone segment leaves nothing in flight once its end is acknowledged.
         */
        void on_probe_ack(ControlBlock& block,
                          const Sequence ack,
                          const bool probe_report,
                          const bool plain_duplicate) noexcept {
            switch (block.probe_state.on_ack(ack, probe_report, plain_duplicate)) {
                case TailProbe::Verdict::Open:
                    break;
                case TailProbe::Verdict::Needless:
                    ++context_.counters.tail_probes_needless;
                    break;
                case TailProbe::Verdict::Repaired:
                    block.recovery.on_repaired(block.send.flight(), block.mss());
                    ++context_.counters.tail_probe_recoveries;
                    break;
            }
        }

        /**
         * The undo's checks, after the probe's ACK and before detection, in the order of Linux's
         * `tcp_fastretrans_alert`; `state` is the recovery state the ACK found, `info` is set when it moved SND.UNA,
         * and `reordering` says it showed reordering already. Each undo is counted by its sign and takes the policy's
         * reduction back (`LossRecovery::undo`); the record it closes undoes nothing more, so an episode is undone
         * once.
         *
         * - Every resend of the episode reported as a duplicate (`tcp_try_undo_dsack`), open, which is after the
         *   episode ended, or in recovery: the lost marks stay. In recovery the episode ends there, as Linux's
         *   `tcp_try_to_open` follows that undo; with the state kept, PRR would go on toward the target the undo took
         *   back, and a loss found next would ride an undone episode with no reduction of its own. RACK's grown window
         *   persists one recovery longer (`Rack::undone_by_report`).
         * - An ACK that moved SND.UNA in recovery and answers an original (`answers_original`). Short of `recover`
         *   (`tcp_try_undo_partial`), reordering is recorded at once; while a resend is still out, neither reported nor
         *   acknowledged, the undo waits and this ACK marks and resends nothing more; else every lost mark is cleared
         *   and the connection is open. At the ACK that covers `recover` (`tcp_try_undo_recovery`) the undo opens the
         *   connection in place of the end's step to the target, the marks left to detection.
         * - An ACK that moved SND.UNA in the loss state with either sign (`tcp_try_undo_loss`, which reads
         *   `tcp_may_undo`): every lost mark is cleared, and the connection is open; the retry count the ACK's
         *   cumulative part has cleared already, as on every ACK of new data (`on_acknowledged`). The policy
         *   then hears the ACK, from the window restored, as it hears every ACK of new data in the loss state and as
         *   Linux's `tcp_cong_control` grows after the undo; when detection then marks something and a new episode
         *   begins on the ACK, it has grown first, as at the end of the loss state (`end_on_this_ack`). Linux reads the
         *   duplicate reports' sign in the loss state only here, on an ACK of new data, and so does this: the marks the
         *   timeout set must not leave under the window restored before they are cleared.
         *
         * An undo that ends an episode in recovery leaves the policy deaf to that ACK, as the end of recovery does,
         * where Linux's `tcp_cong_control` grows on either, the state being open by then. The connection without
         * SACK opens at once too, where Linux's `tcp_try_undo_loss` holds it in the loss state until the episode's data
         * is acknowledged (`tcp_is_non_sack_preventing_reopen`): RFC 6582's `recover`, which the undo leaves where the
         * episode set it, is the guard here.
         */
        [[nodiscard]] Undone try_undo(ControlBlock& block,
                                      const RecoveryState state,
                                      const Sequence ack,
                                      const std::optional<std::uint32_t> echo,
                                      const std::optional<AckInfo>& info,
                                      const bool reordering) noexcept {
            if (!block.recovery.undo_open()) {
                return {};
            }
            const bool reported = block.recovery.every_resend_reported();
            if (reported && state != RecoveryState::Loss) {
                const bool reopen = state == RecoveryState::Recovery;
                block.recovery.undo(reopen);
                block.rack.undone_by_report();
                ++context_.counters.undos_duplicate_sack;
                return {.undone = true, .reopened = reopen};
            }
            if (!info || state == RecoveryState::Open) {
                return {};
            }
            if (state == RecoveryState::Loss) {
                if (!reported && !answers_original(block, echo)) {
                    return {};
                }
                transmitter_.clear_all_lost(block);
                block.recovery.undo(true);
                block.recovery.on_ack(*info, ack);
                if (reported) {
                    block.rack.undone_by_report();
                    ++context_.counters.undos_duplicate_sack;
                } else {
                    ++context_.counters.undos_original_ack;
                }
                return {.undone = true, .reopened = true};
            }
            if (!answers_original(block, echo)) {
                return {};
            }
            if (!block.recovery.recovered(ack)) {
                block.rack.see_reordering();
                if (!reordering) {
                    ++context_.counters.reordering_seen;
                }
                if (transmitter_.resends_out(block) > 0) {
                    return {.held = true};
                }
                transmitter_.clear_all_lost(block);
            }
            block.recovery.undo(true);
            ++context_.counters.undos_original_ack;
            return {.undone = true, .reopened = true};
        }

        /// The ACK answers an original (`LossRecovery::answers_original`): its echo, with timestamps and not zero, as
        /// Linux's `tcp_tsopt_ecr_before` reads `rcv_tsecr`, against the timestamp value of the episode's first resend.
        [[nodiscard]] static bool answers_original(const ControlBlock& block,
                                                   const std::optional<std::uint32_t> echo) noexcept {
            const bool stamped                         = block.timestamps.enabled();
            const std::optional<core::TimePoint> first = block.recovery.first_resend();
            const std::optional<std::uint32_t> echoed  = stamped && echo && *echo != 0 ? echo : std::nullopt;
            const std::optional<std::uint32_t> resent_at =
                stamped && first ? std::optional{block.timestamps.value(*first)} : std::nullopt;
            return block.recovery.answers_original(echoed, resent_at);
        }

        /**
         * Linux's `tcp_is_sackblock_valid` for a block that touches the entries, with `edge` the later of the ACK and
         * SND.UNA, where SND.UNA is about to be: it starts before it ends and before SND.NXT, it ends at or before
         * SND.NXT, and it starts after `edge`. A duplicate report at or below `edge` touches no entry.
         */
        [[nodiscard]] static bool
        valid_block(const ControlBlock& block, const wire::TcpSackBlock& sack_block, const Sequence edge) noexcept {
            return sack_block.first.before(sack_block.end) && sack_block.first.before(block.send.nxt) &&
                   !sack_block.end.after(block.send.nxt) && sack_block.first.after(edge);
        }

        /// The head that never left, the FIN of a close the peer's window did not cover: sent once the window covers
        /// it, as Linux's `tcp_push_pending_frames` on an ACK. A head waiting for the next hop is the ARP poll's. The
        /// send arms the RTO (`transmit_entry`); a refused one leaves the RTO to try again, as `close` does.
        void send_unsent(ControlBlock& block) noexcept {
            if (block.queue.empty() || transmitter_.in_flight(block) || block.timer_kind == TimerKind::ArpPoll) {
                return;
            }
            if (const typename Transmitter::SentNode& head = transmitter_.head(block);
                !block.send.covers(head.first + head.length)) {
                return;
            }
            std::ignore = transmitter_.transmit_entry(block, block.queue.head());
            if (!block.timer.armed()) {
                transmitter_.arm_retransmit(block);
            }
        }

        /**
         * SND.UNA moves to `ack`, here alone, and the sum of its advances with it, which the bound on an old ACK reads
         * (`SendSequence::ack_floor`): the queue frees what it covers (`Transmitter::acknowledge`; a partial entry
         * stays whole), then the sample and the timer (`on_acknowledged`). On a connection that uses SACK each entry it
         * frees that no block reported advances RACK, as Linux's `tcp_clean_rtx_queue` calls `tcp_rack_advance`
         * unless `tcp_skb_spurious_retrans`: a resend is skipped when the ACK's echo is older than the resend
         * (`echo_before`). One such entry never resent that lies below `highest`, the highest sequence reported
         * before this ACK, filled a hole: reordering, as Linux's `tcp_check_sack_reordering` sees it there.
         */
        [[nodiscard]] Cumulative acknowledge(ControlBlock& block,
                                             const Sequence ack,
                                             const std::optional<std::uint32_t> echo,
                                             const std::optional<Sequence> highest) noexcept {
            Cumulative cumulative{};
            const bool sack  = block.sack.in_use();
            cumulative.freed = transmitter_.acknowledge(
                block, ack, [this, &block, &cumulative, sack, echo, highest](const SentNode& entry) noexcept {
                    if (!sack) {
                        return;
                    }
                    const bool resent = entry.resent();
                    block.rack.advance(entry.sent,
                                       entry.first + entry.length,
                                       resent,
                                       resent && echo_before(block, echo, entry.sent),
                                       context_.now);
                    cumulative.reordering =
                        cumulative.reordering || (!resent && highest && entry.first.before(*highest));
                });
            block.send.acknowledged += static_cast<std::uint32_t>(ack - block.send.una);  // Linux's bytes_acked
            block.send.una           = ack;
            cumulative.rtt           = on_acknowledged(block, cumulative.freed, echo);
            return cumulative;
        }

        /// The ACK's echo names a transmission older than the one at `sent`: the ACK cannot be for that transmission.
        /// Nothing without timestamps, without an echo, or with an echo of zero, as Linux's `tcp_skb_spurious_retrans`
        /// reads `rcv_tsecr`.
        [[nodiscard]] static bool echo_before(const ControlBlock& block,
                                              const std::optional<std::uint32_t> echo,
                                              const core::TimePoint sent) noexcept {
            return block.timestamps.enabled() && echo && *echo != 0 &&
                   static_cast<std::int32_t>(*echo - block.timestamps.value(sent)) < 0;
        }

        /**
         * SND.UNA moved forward. The retry count clears. The round-trip sample follows Linux's `tcp_clean_rtx_queue`
         * and `tcp_ack_update_rtt`: from the oldest entry the ACK freed, measured from its transmission, when no entry
         * it freed left more than once (Karn's rule); else, on a connection with timestamps, from the ACK's echo, which
         * names the transmission it answers, so the echo on the ACK of a resend ends the backoff too
         * (`Timestamps::sample`). The sample feeds RACK's minimum round trip too, as Linux's `tcp_update_rtt_min`, and
         * lets the next tail probe go (RFC 8985 7.3: a sample since the last probe; `TailProbe::sampled`). The
         * RTO restarts from now while anything is in flight and the timer stops when nothing is, RFC 6298's rule, which
         * covers RFC 6582's re-arm on a partial ACK; a head that never left, the FIN a close holds for the window, is
         * `send_unsent`'s or the probes'. The restart records the RTO's moment only
         * (`Transmitter::restart_rto_moment`): the ACK's last step arms the wheel once, with the kind it chooses
         * (`Transmitter::set_timer`), or reneging arms the RTO short. A partial ACK frees nothing, samples only from
         * its echo, and restarts the RTO. Returns the sample taken, if any.
         *
         * Where this departs from Linux: the echo of a partial ACK that frees no whole entry gives a sample, where
         * `tcp_clean_rtx_queue` sets no `FLAG_DATA_ACKED` for it and `tcp_ack_update_rtt` takes none, so an RTO after
         * such an ACK can fall a tick away from Linux's.
         */
        std::optional<core::Duration> on_acknowledged(ControlBlock& block,
                                                      const Transmitter::Freed& freed,
                                                      const std::optional<std::uint32_t> echo) noexcept {
            block.retransmit_count = 0;
            const std::optional<core::Duration> sample =
                freed.length > 0 && !freed.resent
                    ? std::optional{std::max<core::Duration>(context_.now - freed.sent, core::Duration::zero())}
                    : block.timestamps.sample(echo, context_.now);
            if (sample) {
                block.rtt.sample(*sample, context_.config.rto_min, context_.config.rto_max);
                block.rack.sample(*sample, context_.now);
                block.probe_state.sampled();
                ++context_.counters.rtt_samples;
            }
            if (transmitter_.in_flight(block)) {
                transmitter_.restart_rto_moment(block);
            } else {
                transmitter_.disarm(block);
            }
            return sample;
        }

        /// An ACK of new data on a synchronized connection: the entries it frees, SND.UNA, the sample and the timer
        /// (`acknowledge`). Returns what the policy reads of it, should it hear of this ACK (`update_recovery`), with
        /// the window's use as the sends recorded it (`WindowUse`), before anything this ACK lets out; and whether it
        /// showed reordering.
        [[nodiscard]] Acknowledged acknowledge_data(ControlBlock& block,
                                                    const Sequence ack,
                                                    const std::optional<std::uint32_t> echo,
                                                    const std::optional<Sequence> highest) noexcept {
            const std::uint32_t flight_before = block.send.flight();
            const auto acked                  = static_cast<std::uint32_t>(ack - block.send.una);
            const Cumulative cumulative       = acknowledge(block, ack, echo, highest);
            return Acknowledged{
                .info       = AckInfo{.acked             = acked,
                                      .flight            = flight_before,
                                      .mss               = block.mss(),
                                      .rtt               = cumulative.rtt,
                                      .now               = context_.now,
                                      .sent              = cumulative.freed.newest_sent,
                                      .delivered         = block.queue.delivered(),
                                      .delivered_at_send = cumulative.freed.delivered_at_send,
                                      .app_limited       = cumulative.freed.app_limited,
                                      .window_limited    = block.window_use.limited(),
                                      .largest_flight    = block.window_use.largest_flight()},
                .reordering = cumulative.reordering
            };
        }

        /**
         * Detection without SACK, and the recovery state, after the blocks, the cumulative part, the undo's checks, the
         * end of an episode the ACK covered and RACK's detection. `info` is set when the ACK moved SND.UNA;
         * `duplicates` is the count with this ACK when it is a duplicate, else zero; `detected` is what RACK's
         * detection did on this ACK, nothing without SACK or when it did not run; `ended` says the ACK ended the
         * episode it found, by covering `recover` (`end_on_this_ack`) or by an undo that reopened (`try_undo`); `held`
         * says it is a partial ACK that answered an original while a resend is out, which marks nothing.
         *
         * An ACK that ended an episode begins the next at once when an entry is marked lost, as Linux's state, open
         * again, falls through to its open case on the same ACK. The mark is one RACK set on this ACK, its detection
         * having read the open state's window; or one set earlier and not resent yet, on data sent above `recover`
         * that the window held back, which the ACK does not cover; or one an undo by duplicate report kept. Any other
         * ACK is one of three, by the state it found, as Linux's `tcp_fastretrans_alert` switches on its own:
         *
         * - Open. Without SACK, the third duplicate that RFC 6582 admits marks the head lost. An entry marked lost, by
         *   either detector, begins recovery (`Transmitter::begin_recovery`), with PRR's first step for this ACK:
         *   Linux's `tcp_time_to_recover`. Else an ACK of new data is the policy's to hear.
         * - Recovery. One step of PRR with what the ACK delivered and what is in flight now; without SACK, one that
         *   moved SND.UNA is RFC 6582's partial ACK and marks the new head first, as Linux's `tcp_newreno_mark_lost`
         *   does, unless it is `held`, which still steps PRR, as Linux's `tcp_cwnd_reduction` runs after
         *   `tcp_try_undo_partial` returned. An ACK that marked something is no safe ACK for PRR. A resend RACK found
         *   lost again is resent under the same episode and reduces nothing further.
         * - Loss. Every ACK of new data is the policy's to hear, which grows its window by slow start; the entries the
         *   timeout marked leave by the resend pass. A resend RACK found lost again moves the connection to recovery,
         *   which sets a new target, as Linux's `tcp_fastretrans_alert` does on `FLAG_LOST_RETRANS`, and the policy
         *   does not hear that ACK. An original RACK marks leaves the loss state as it is.
         *
         * In recovery no ACK reaches the policy's `on_ack`, the one that ends it included: the window there is the
         * rate reduction's, and its end is `on_recovery_end`. What differs with SACK and without is how a loss is
         * detected and how what an ACK delivered is counted (`settle_delivery`), which comes before what is in flight
         * is read, in recovery and as one begins.
         */
        void update_recovery(ControlBlock& block,
                             const Found& found,
                             const Sequence ack,
                             const std::uint8_t duplicates,
                             const std::optional<AckInfo>& info,
                             const Rack::Detected& detected,
                             const bool ended,
                             const bool held) noexcept {
            const bool duplicate         = duplicates > 0;
            const bool sack              = block.sack.in_use();
            const std::uint32_t advance  = info ? info->acked : 0;
            const auto begin_on_this_ack = [this, &block, &found, advance, duplicate] {
                // Settled first: `begin_recovery` reads what is in flight, which reads the duplicate count.
                const std::uint32_t delivery = settle_delivery(block, found, advance, duplicate);
                transmitter_.begin_recovery(block, delivery);
            };
            if (ended) {
                if (block.queue.lost_bytes() > 0) {
                    begin_on_this_ack();
                }
                return;
            }
            switch (found.state) {
                case RecoveryState::Open:
                    if (!sack && duplicates == 3 && block.recovery.admits(ack)) {
                        std::ignore = mark_head(block);
                    }
                    if (block.queue.lost_bytes() > 0) {
                        begin_on_this_ack();
                    } else if (info) {
                        block.recovery.on_ack(*info, ack);
                    }
                    break;
                case RecoveryState::Recovery: {
                    const bool marked             = !held && (sack ? detected.marked > 0 : info && mark_head(block));
                    // Settled first: what is in flight reads the duplicate count.
                    const std::uint32_t delivery  = settle_delivery(block, found, advance, duplicate);
                    const std::uint32_t in_flight = transmitter_.in_flight_bytes(block);
                    block.recovery.on_recovery_ack(delivery, in_flight, info && !marked, block.mss());
                    break;
                }
                case RecoveryState::Loss: {
                    const bool begins = detected.resends_lost > 0;
                    if (info && !begins) {
                        block.recovery.on_ack(*info, ack);
                    }
                    if (begins) {
                        begin_on_this_ack();
                    }
                    break;
                }
            }
        }

        /**
         * The ACK covers `recover`, and no undo ended the episode first: the episode it found ends before detection, as
         * Linux settles the exits of `tcp_fastretrans_alert` (its step D) before it looks for losses (step E), so that
         * detection reads the reordering window of the open state. From recovery the policy's window becomes its
         * target; in the loss state the policy hears the ACK first, as it hears every ACK of new data there. That is
         * Linux's `tcp_cong_control` growing the window on the ACK `tcp_process_loss` ended the state on only when no
         * recovery begins on that ACK. When detection then marks something and a new episode begins on it
         * (`update_recovery`), the policy here has heard the ACK, and grown, before the new episode sets its target,
         * where Linux's `tcp_cong_control`, finding the state in recovery, runs `tcp_cwnd_reduction` and grows nothing.
         * Either way RACK's grown window has one recovery fewer to persist for (`end_episode`).
         */
        void end_on_this_ack(ControlBlock& block,
                             const Found& found,
                             const Sequence ack,
                             const std::optional<AckInfo>& info) noexcept {
            if (found.state == RecoveryState::Loss && info) {
                block.recovery.on_ack(*info, ack);  // heard in the loss state, before the state ends
            }
            end_episode(block, ack);
        }

        /// The episode, recovery or loss, ends at `ack` (`LossRecovery::end`), and RACK's grown reordering window has
        /// one recovery fewer to persist for: a recovery that ended and was not undone. One undone never ends here: an
        /// undo before its end opens the connection then (`try_undo`).
        static void end_episode(ControlBlock& block, const Sequence ack) noexcept {
            block.recovery.end(ack);
            block.rack.recovery_ended();
        }

        /// Marks the head lost (`Transmitter::mark_head_lost`); whether that marked anything: not a head already
        /// marked, one the peer reported received or one that never left.
        [[nodiscard]] bool mark_head(ControlBlock& block) noexcept {
            const std::uint32_t lost = block.queue.lost_bytes();
            transmitter_.mark_head_lost(block);
            return block.queue.lost_bytes() != lost;
        }

        /**
         * Settles what this ACK delivered, RFC 9937 6.2's DeliveredData, and returns it; `advance` is how far it moved
         * SND.UNA. A step, not a query: without SACK in recovery it takes the segments an advance covers out of the
         * duplicate count (`LossRecovery::remove_duplicates`), which what is in flight reads
         * (`Transmitter::in_flight_bytes`), so it runs once per ACK and before that is read.
         *
         * With SACK it is exact: the advance plus the signed change in bytes reported received, since a cumulative ACK
         * takes out of that total what it covers. Without SACK it is Linux's estimate in bytes: one segment for a
         * duplicate ACK (`tcp_add_reno_sack`); for an ACK that moves SND.UNA in recovery, the advance less one segment
         * per duplicate counted before it, and at least one (`LossRecovery::remove_duplicates`, which says where that
         * departs from RFC 9937 6.2 and why). An estimate a peer could inflate with duplicates, so in recovery it is
         * cut where the episode's total would pass RecoverFS (RFC 9937 6.2); the ACK that begins recovery is held to
         * it by the first step (`Transmitter::begin_recovery`).
         */
        [[nodiscard]] static std::uint32_t settle_delivery(ControlBlock& block,
                                                           const Found& found,
                                                           const std::uint32_t advance,
                                                           const bool duplicate) noexcept {
            if (block.sack.in_use()) {
                return advance + (block.queue.reported_bytes() - found.reported);
            }
            if (found.state != RecoveryState::Recovery) {
                return duplicate ? block.mss() : advance;
            }
            const std::uint32_t estimate = advance > 0 ? block.recovery.remove_duplicates(advance, block.mss())
                                           : duplicate ? block.mss()
                                                       : 0;
            return std::min(estimate, block.recovery.undelivered());
        }

        Context& context_;
        Transmitter& transmitter_;
    };

}  // namespace aloe::tcp::detail
