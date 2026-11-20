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

namespace aloe::tcp::detail {

    /**
     * @brief What an acknowledgement does to the send side: the entries it frees, the round-trip sample and the
     * retransmission timer, loss recovery and the resends it asks for, the peer's window, the zero-window probes and
     * the FIN that waited for the window. It owns no state; it drives the block's send side through the
     * Transmitter, in place, and tells the Receiver what moved. It names neither the Receiver nor the Lifecycle: an
     * ACK ends a connection only through what the Receiver does with the outcome.
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
         * bytes each and empty without one, are read into the scoreboard (`read_blocks`), on an ACK below SND.UNA too,
         * for which nothing else runs. An ACK of new data frees what it covers, samples the round trip and drives loss
         * recovery (`acknowledge_data`). A duplicate ACK, of something in flight, is judged against the window as it
         * stands, before this segment updates it, the field scaled by the peer's shift. RFC 793 updates the window
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
            // The marks it sets are the scoreboard's; no step below reads what it returns yet.
            std::ignore                       = read_blocks(block, ack, sack);
            const bool duplicate = payload_length == 0 && !header.flags.has(wire::TcpFlag::Syn) &&
                                   !header.flags.has(wire::TcpFlag::Fin) && ack == block.send.una &&
                                   block.send.scaled(header.window) == block.send.wnd && transmitter_.in_flight(block);
            Outcome outcome{};
            if (ack.after(block.send.una)) {
                acknowledge_data(block, ack, echo);
                outcome.advanced = true;
            } else if (duplicate) {
                on_duplicate_ack(block, ack);
            }
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

        /**
         * An ACK of new data on a synchronized connection: the entries it frees, the sample and the timer
         * (`on_acknowledged`), then loss recovery (`LossRecovery::on_ack`), which hears the ACK with what the policy
         * reads and says what to resend: the head as a fast retransmit on a partial ACK in fast recovery, or the new
         * head in the loss episode after a timeout, marked lost and resent by the pass. That one is counted in
         * `retransmits` alone, as it was before the pass: what the pass counted in `fast_retransmits` is taken back, an
         * interim count that goes with the `Resend` result.
         */
        void
        acknowledge_data(ControlBlock& block, const Sequence ack, const std::optional<std::uint32_t> echo) noexcept {
            const std::uint32_t flight_before = block.send.flight();
            const auto acked                  = static_cast<std::uint32_t>(ack - block.send.una);
            const typename Transmitter::Freed freed =
                transmitter_.acknowledge(block, ack, [](const SentNode&) noexcept {});  // a partial entry stays whole
            block.send.una                          = ack;
            const std::optional<core::Duration> rtt = on_acknowledged(block, freed, echo);
            const AckInfo info{.acked             = acked,
                               .flight            = flight_before,
                               .mss               = block.mss(),
                               .rtt               = rtt,
                               .now               = context_.now,
                               .sent              = freed.newest_sent,
                               .delivered         = block.queue.delivered(),
                               .delivered_at_send = freed.delivered_at_send,
                               .app_limited       = freed.app_limited};
            switch (block.recovery.on_ack(info, ack, block.send)) {
                case Resend::Fast:
                    fast_retransmit(block);
                    break;
                case Resend::Head: {
                    const std::uint64_t fast_retransmits = context_.counters.fast_retransmits;
                    transmitter_.mark_head_lost(block);
                    transmitter_.resend_lost(block);
                    context_.counters.fast_retransmits = fast_retransmits;
                    ++context_.counters.retransmits;
                    break;
                }
                case Resend::None:
                    break;
            }
        }

        /// RFC 5681's duplicate ACK, counted; the third may start fast recovery (`LossRecovery::on_duplicate`), which
        /// resends the head at once.
        void on_duplicate_ack(ControlBlock& block, const Sequence ack) noexcept {
            ++context_.counters.duplicate_acks;
            if (block.recovery.on_duplicate(ack, block.send, block.mss()) == Resend::Fast) {
                fast_retransmit(block);
                ++context_.counters.recoveries;
            }
        }

        /**
         * The head again, driven by the third duplicate or a partial ACK: marked lost, and resent by the pass
         * (`Transmitter::resend_lost`), which counts it in `fast_retransmits` and, when it leaves, in place or as a
         * copy, restarts the RTO from now. A resend deferred, or refused, leaves the timer as it was. With nothing in
         * flight the head, if any, waits for the window. Until the one recovery replaces this call, it counts once per
         * call, as it did before the pass: where the pass counts no first attempt, with nothing in flight or the head
         * already waiting for its packet, the call counts, and restarts the RTO when such a waiting head leaves.
         */
        void fast_retransmit(ControlBlock& block) noexcept {
            const bool uncounted = !transmitter_.in_flight(block) || transmitter_.deferred(block);
            if (uncounted) {
                ++context_.counters.fast_retransmits;
            }
            transmitter_.mark_head_lost(block);
            if (transmitter_.resend_lost(block) > 0 && uncounted) {
                transmitter_.arm_retransmit(block);
            }
        }

        Context& context_;
        Transmitter& transmitter_;
    };

}  // namespace aloe::tcp::detail
