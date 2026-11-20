#pragma once

#include <algorithm>
#include <aloe/core>
#include <aloe/wire>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>

#include <congestion_control.hpp>
#include <detail/control_block.hpp>
#include <detail/loss_recovery.hpp>
#include <detail/stack_context.hpp>
#include <detail/transmitter.hpp>
#include <tcp_config.hpp>

namespace aloe::tcp::detail {

    /**
     * @brief What an acknowledgement does to the send side, in the order of Linux's `tcp_ack` and
     * `tcp_fastretrans_alert`: the peer's blocks into the scoreboard; the entries the cumulative part frees, the
     * round-trip sample and the retransmission timer; detection, by the duplicate count; the recovery state, which
     * begins, goes on or ends, with the window in recovery; the resends, as far as that window allows; then the
     * peer's window, the zero-window probes and the FIN that waited for the window. It owns no state; it drives the
     * block's send side through the Transmitter, in place, and tells the Receiver what moved. It names neither the
     * Receiver nor the Lifecycle: an ACK ends a connection only through what the Receiver does with the outcome.
     */
    template <typename Ip, typename CongestionControl>
    class AckProcessor {
    public:
        using ControlBlock = detail::ControlBlock<Ip, CongestionControl>;
        using Context      = StackContext<Ip, CongestionControl>;
        using Transmitter  = detail::Transmitter<Ip, CongestionControl>;
        using Sequence     = wire::TcpSequence;
        using Freed        = Transmitter::Freed;
        using Reported     = Transmitter::Reported;
        using SentNode     = Transmitter::SentNode;

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
        /// taken and the timer stopped with the queue empty (`on_acknowledged`).
        void handshake_acknowledged(ControlBlock& block,
                                    const Sequence ack,
                                    const std::optional<std::uint32_t> echo) noexcept {
            const Freed freed = transmitter_.acknowledge(block, ack, [](const SentNode&) noexcept {});
            block.send.una    = ack;
            on_acknowledged(block, freed, echo);
        }

        /**
         * An ACK on a synchronized connection, of nothing past SND.NXT. First its SACK option's blocks, `sack`, eight
         * bytes each and empty without one, are read into the scoreboard (`read_blocks`), on an ACK below SND.UNA too.
         * An ACK of new data frees what it covers and samples the round trip (`acknowledge_data`). A duplicate ACK, of
         * something in flight, is judged against the window as it stands, before this segment updates it, the field
         * scaled by the peer's shift, and counted. Then loss recovery, by the state the ACK found (`update_recovery`),
         * and the resend pass, which sends what is marked lost as far as the window allows. RFC 793 updates the window
         * only when SND.UNA <= SEG.ACK <= SND.NXT: a stale ACK carries a stale window. `payload_length` is the
         * segment's, since an ACK that carries data is no duplicate.
         */
        [[nodiscard]] Outcome on_ack(ControlBlock& block,
                                     const wire::TcpHeader& header,
                                     const std::uint16_t payload_length,
                                     const std::optional<std::uint32_t> echo,
                                     const std::span<const std::byte> sack) noexcept {
            const Sequence ack                = header.acknowledgement;
            const std::size_t writable_before = transmitter_.writable(block);
            const Found found{.state = block.recovery.state(), .reported = block.queue.reported_bytes()};
            // The marks it sets are the scoreboard's; no step below reads what it returns yet.
            std::ignore          = read_blocks(block, ack, sack);
            const bool duplicate = payload_length == 0 && !header.flags.has(wire::TcpFlag::Syn) &&
                                   !header.flags.has(wire::TcpFlag::Fin) && ack == block.send.una &&
                                   block.send.scaled(header.window) == block.send.wnd && transmitter_.in_flight(block);
            Outcome outcome{};
            std::optional<AckInfo> info{};
            std::uint8_t duplicates = 0;  // the count with this ACK, when it is a duplicate
            if (ack.after(block.send.una)) {
                info             = acknowledge_data(block, ack, echo);
                outcome.advanced = true;
            } else if (duplicate) {
                ++context_.counters.duplicate_acks;
                duplicates = block.recovery.count_duplicate();
            }
            update_recovery(block, found, ack, duplicates, info);
            std::ignore = transmitter_.resend_lost(block);
            if (!ack.before(block.send.una)) {
                block.send.update_window(header);
                update_probing(block);  // a window that opened ends probing, and the comparison below sees it
                send_unsent(block);     // and sends the FIN a close left waiting for it
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
        };

        /// What an ACK's SACK option said.
        struct Blocks {
            std::uint32_t reported = 0;  ///< Sequence space newly marked received.
            bool reordering        = false;
            std::optional<wire::TcpSackBlock> duplicate{};  ///< A believed duplicate report.
        };

        /**
         * The first step of an acceptable ACK: its SACK option read into the scoreboard. Nothing on a connection that
         * does not use SACK, with no option, or for an ACK older than SND.UNA by more than the largest window the peer
         * offered (`tcp_sacktag_write_queue`). The duplicate report is recognised first, from the option alone, and
         * counted when believed (`RetransmissionQueue::duplicate_report`); then each other block that is valid marks
         * the entries it covers whole, lowest first, as Linux sorts them, so lower data reported in the same option
         * is not taken for reordering. A believed duplicate report that is valid and names a resent entry a block
         * reported shows reordering. Blocks a correct peer never sends mark nothing and are not an error.
         */
        [[nodiscard]] Blocks
        read_blocks(ControlBlock& block, const Sequence ack, const std::span<const std::byte> sack) noexcept {
            if (!block.sack.in_use() || sack.empty() ||
                ack.before(Sequence{block.send.una.value - block.send.max_wnd})) {
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
            if (transmitter_.duplicate_report(block, first, second, ack)) {
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
                const Reported reported  = transmitter_.report(block, sack_block, [](const SentNode&) noexcept {});
                blocks.reported         += reported.bytes;
                blocks.reordering        = blocks.reordering || reported.reordering;
            }
            return blocks;
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
         * SND.UNA moved forward. The retry count clears. The round-trip sample follows Linux's `tcp_clean_rtx_queue`
         * and `tcp_ack_update_rtt`: from the oldest entry the ACK freed, measured from its transmission, when no entry
         * it freed left more than once (Karn's rule); else, on a connection with timestamps, from the ACK's echo, which
         * names the transmission it answers, so the echo on the ACK of a resend ends the backoff too
         * (`Timestamps::sample`). The timer restarts from now while anything is in flight and stops when nothing is,
         * RFC 6298's rule, which covers RFC 6582's re-arm on a partial ACK; a head that never left, the FIN a close
         * holds for the window, is `send_unsent`'s or the probes'. A partial ACK frees nothing, samples only from its
         * echo, and restarts the timer. Returns the sample taken, if any.
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
                ++context_.counters.rtt_samples;
            }
            if (transmitter_.in_flight(block)) {
                transmitter_.arm_retransmit(block);
            } else {
                transmitter_.disarm(block);
            }
            return sample;
        }

        /// An ACK of new data on a synchronized connection: the entries it frees, SND.UNA, the sample and the timer
        /// (`on_acknowledged`). Returns what the policy reads of it, should it hear of this ACK (`update_recovery`).
        [[nodiscard]] AckInfo
        acknowledge_data(ControlBlock& block, const Sequence ack, const std::optional<std::uint32_t> echo) noexcept {
            const std::uint32_t flight_before = block.send.flight();
            const auto acked                  = static_cast<std::uint32_t>(ack - block.send.una);
            const typename Transmitter::Freed freed =
                transmitter_.acknowledge(block, ack, [](const SentNode&) noexcept {});  // a partial entry stays whole
            block.send.una                          = ack;
            const std::optional<core::Duration> rtt = on_acknowledged(block, freed, echo);
            return AckInfo{.acked             = acked,
                           .flight            = flight_before,
                           .mss               = block.mss(),
                           .rtt               = rtt,
                           .now               = context_.now,
                           .sent              = freed.newest_sent,
                           .delivered         = block.queue.delivered(),
                           .delivered_at_send = freed.delivered_at_send,
                           .app_limited       = freed.app_limited};
        }

        /**
         * Detection and the recovery state, after the blocks and the cumulative part. `info` is set when the ACK moved
         * SND.UNA; `duplicates` is the count with this ACK when it is a duplicate, else zero. One of three, by the
         * state the ACK found, as Linux's `tcp_fastretrans_alert` switches on its own:
         *
         * - Open. The third duplicate that RFC 6582 admits marks the head lost, and the first entry marked begins
         *   recovery (`Transmitter::begin_recovery`), counted, with PRR's first step for this ACK. Else an ACK of new
         *   data is the policy's to hear.
         * - Recovery. The ACK that covers `recover` ends it, and the policy's window becomes its target. Any other
         *   runs one step of PRR with what it delivered and what is in flight now; one that moved SND.UNA is RFC
         *   6582's partial ACK and marks the new head first, as Linux's `tcp_newreno_mark_lost` does, which makes it
         *   no safe ACK for PRR.
         * - Loss. Every ACK of new data is the policy's to hear, which grows its window by slow start; the entries the
         *   timeout marked are left as they are. The ACK that covers `recover` then ends the state: it is heard like
         *   the others, as Linux's `tcp_cong_control` grows the window on the ACK `tcp_process_loss` ended the state
         *   on.
         *
         * In recovery no ACK reaches the policy's `on_ack`, the one that ends it included: the window there is the
         * rate reduction's, and its end is `on_recovery_end`. The duplicate count detects for every connection, with
         * SACK and without; what differs is how what an ACK delivered is counted (`delivered`).
         */
        void update_recovery(ControlBlock& block,
                             const Found& found,
                             const Sequence ack,
                             const std::uint8_t duplicates,
                             const std::optional<AckInfo>& info) noexcept {
            const bool duplicate = duplicates > 0;
            switch (found.state) {
                case RecoveryState::Open:
                    if (duplicates == 3 && block.recovery.admits(ack) && mark_head(block)) {
                        transmitter_.begin_recovery(block, delivered(block, found, 0, duplicate));
                        ++context_.counters.recoveries;
                    } else if (info) {
                        block.recovery.on_ack(*info, ack);
                    }
                    break;
                case RecoveryState::Recovery: {
                    if (block.recovery.recovered(ack)) {
                        block.recovery.end(ack);
                        break;
                    }
                    const bool marked             = info && mark_head(block);
                    // What the ACK delivered first: it settles the duplicate count, which what is in flight reads.
                    const std::uint32_t delivery  = delivered(block, found, info ? info->acked : 0, duplicate);
                    const std::uint32_t in_flight = transmitter_.in_flight_bytes(block);
                    block.recovery.on_recovery_ack(delivery, in_flight, info && !marked, block.mss());
                    break;
                }
                case RecoveryState::Loss:
                    if (info) {
                        block.recovery.on_ack(*info, ack);  // heard in the loss state, before the state may end
                    }
                    if (block.recovery.recovered(ack)) {
                        block.recovery.end(ack);
                    }
                    break;
            }
        }

        /// Marks the head lost (`Transmitter::mark_head_lost`); whether that marked anything: not a head already
        /// marked, one the peer reported received or one that never left.
        [[nodiscard]] bool mark_head(ControlBlock& block) noexcept {
            const std::uint32_t lost = block.queue.lost_bytes();
            transmitter_.mark_head_lost(block);
            return block.queue.lost_bytes() != lost;
        }

        /**
         * What this ACK delivered, RFC 9937 6.2's DeliveredData; `advance` is how far it moved SND.UNA.
         *
         * With SACK it is exact: the advance plus the signed change in bytes reported received, since a cumulative ACK
         * takes out of that total what it covers. Without SACK it is Linux's estimate in bytes: one segment for a
         * duplicate ACK (`tcp_add_reno_sack`); for an ACK that moves SND.UNA in recovery, the advance less one segment
         * per duplicate counted before it, and at least one (`LossRecovery::remove_duplicates`, which says where that
         * departs from RFC 9937 6.2 and why). An estimate a peer could inflate with duplicates, so in recovery it is
         * cut where the episode's total would pass RecoverFS (RFC 9937 6.2); the ACK that begins recovery is held to
         * it by the first step (`Transmitter::begin_recovery`).
         */
        [[nodiscard]] static std::uint32_t
        delivered(ControlBlock& block, const Found& found, const std::uint32_t advance, const bool duplicate) noexcept {
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
