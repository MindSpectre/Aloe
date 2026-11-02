#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

#include <congestion_control.hpp>
#include <send_sequence.hpp>
#include <tcp_sequence.hpp>

namespace aloe::tcp::detail {

    /// What an ACK asks the sender to resend.
    enum class Resend : std::uint8_t {
        None,  ///< Nothing.
        Fast,  ///< The head, as a fast retransmit: the third duplicate ACK, or a partial ACK in fast recovery.
        Head,  ///< The new head, in the loss episode after a timeout: what the timeout's resend left behind.
    };

    /**
     * @brief A connection's loss recovery, RFC 5681 with RFC 3042's limited transmit and RFC 6582's NewReno, over the
     * congestion control's per-connection state.
     *
     * It decides and the stack acts: each event returns what to resend, and the policy hears of every event here.
     * `recover` is RFC 6582's: the last sequence sent as an episode began; outside an episode, one before SND.UNA once
     * an ACK of new data moved it, so the serial comparison never sees it half the space behind.
     */
    template <typename CongestionControl>
    class LossRecovery {
    public:
        using State = CongestionControl::State;

        /// The connection numbered its SYN: RFC 6582's `recover` starts at the ISS.
        void open(const wire::TcpSequence iss) noexcept {
            recover_ = iss;
        }

        /// Synchronized: the policy's initial window.
        void establish(const std::uint16_t mss) noexcept {
            CongestionControl::reset(congestion_, mss);
        }

        /**
         * What the windows allow beyond the flight: the congestion window with limited transmit's allowance, within
         * the peer's window, less what is in flight, and zero when the flight fills it. In 64 bits, since
         * `NoCongestionControl`'s window is the largest 32-bit value.
         */
        [[nodiscard]] std::uint64_t room(const SendSequence& send) const noexcept {
            const std::uint64_t flight = send.flight();
            const std::uint64_t limit  = std::min<std::uint64_t>(send.wnd, std::uint64_t{window()} + limited_transmit_);
            return limit > flight ? limit - flight : 0;
        }

        /// A send after an idle longer than the RTO: RFC 5681's restart.
        void on_idle(const std::uint16_t mss) noexcept {
            CongestionControl::on_idle(congestion_, mss);
        }

        /**
         * An ACK of new data, after the queue freed what it covers: the duplicate count and limited transmit's
         * allowance clear. In fast recovery an ACK past `recover` is the full ACK and ends it, the policy told the
         * flight left; one short of it is RFC 6582's partial ACK: the policy is told what was acknowledged and the new
         * head goes again at once. Outside fast recovery the policy hears of the ACK; in the loss episode after a
         * timeout an ACK short of `recover` resends the new head at once, as Linux resends what its loss state marked
         * lost (RACK refines this later), and one past it ends the episode. Outside both, `recover` slides to one
         * before the new SND.UNA: the guard compares in serial arithmetic, which reads a `recover` left more than 2^31
         * behind as ahead, so a `recover` set at the open or by the last episode would refuse every fast retransmit
         * after 2 GiB.
         */
        [[nodiscard]] Resend
        on_ack(const AckInfo& info, const wire::TcpSequence ack, const SendSequence& send) noexcept {
            dupacks_          = 0;
            limited_transmit_ = 0;
            if (in_recovery_) {
                if (ack.after(recover_)) {
                    in_recovery_ = false;
                    CongestionControl::on_recovery_end(congestion_, send.flight(), info.mss);
                    return Resend::None;
                }
                CongestionControl::on_partial_ack(congestion_, info.acked, info.mss);
                return Resend::Fast;
            }
            CongestionControl::on_ack(congestion_, info);
            if (!loss_episode_) {
                recover_ = wire::TcpSequence{ack.value - 1};
                return Resend::None;
            }
            if (ack.after(recover_)) {
                loss_episode_ = false;
                return Resend::None;
            }
            return Resend::Head;
        }

        /**
         * RFC 5681's duplicate ACK. In fast recovery the policy hears of it; NewReno inflates its window a segment.
         * Outside it the first and second each allow a segment beyond the windows, RFC 3042's limited transmit; the
         * third starts fast recovery only when, in RFC 6582's words, "the Cumulative Acknowledgment field covers more
         * than recover", that is `ack` past `recover`: the guard against a second fast retransmit for data a timeout
         * already resent. Fast recovery records `recover` and tells the policy of the loss with the flight less what
         * limited transmit sent past the windows, which RFC 5681 leaves out of the threshold; the head goes at once.
         * Later duplicates outside recovery do nothing.
         */
        [[nodiscard]] Resend
        on_duplicate(const wire::TcpSequence ack, const SendSequence& send, const std::uint16_t mss) noexcept {
            if (dupacks_ < std::numeric_limits<std::uint8_t>::max()) {
                ++dupacks_;
            }
            if (in_recovery_) {
                CongestionControl::on_dupack(congestion_, mss);
                return Resend::None;
            }
            if (dupacks_ < 3) {
                limited_transmit_ = std::min<std::uint32_t>(limited_transmit_ + mss, 2U * mss);
                return Resend::None;
            }
            if (dupacks_ != 3 || !ack.after(recover_)) {
                return Resend::None;  // RFC 6582: the acknowledgement does not cover more than recover
            }
            const std::uint64_t window = std::min<std::uint64_t>(send.wnd, this->window());
            recover_                   = wire::TcpSequence{send.nxt.value - 1};
            in_recovery_               = true;
            limited_transmit_          = 0;
            CongestionControl::on_loss(
                congestion_, static_cast<std::uint32_t>(std::min<std::uint64_t>(send.flight(), window)), mss);
            return Resend::Fast;
        }

        /// The first expiry of the retransmission timer since an ACK of new data, RFC 6582 on a timeout: `recover`
        /// takes the highest sequence sent, fast recovery ends, and the loss episode begins; the policy hears of it.
        void on_timeout(const SendSequence& send, const std::uint16_t mss) noexcept {
            in_recovery_      = false;
            loss_episode_     = true;
            recover_          = wire::TcpSequence{send.nxt.value - 1};
            dupacks_          = 0;
            limited_transmit_ = 0;
            CongestionControl::on_timeout(congestion_, send.flight(), mss);
        }

        /// The congestion window the policy allows, in bytes.
        [[nodiscard]] std::uint32_t window() const noexcept {
            return CongestionControl::window(congestion_);
        }

        [[nodiscard]] const State& congestion() const noexcept {
            return congestion_;
        }

        /// Fast recovery: entered on the third duplicate ACK, left on the ACK past `recover` or a timeout.
        [[nodiscard]] bool in_recovery() const noexcept {
            return in_recovery_;
        }

        /// Duplicate ACKs since the last ACK of new data, saturating.
        [[nodiscard]] std::uint8_t duplicates() const noexcept {
            return dupacks_;
        }

        /// For tests: `recover` takes this value, as if an episode had recorded it.
        void force_recover(const wire::TcpSequence recover) noexcept {
            recover_ = recover;
        }

    private:
        std::uint8_t dupacks_ = 0;
        wire::TcpSequence recover_{};
        bool in_recovery_               = false;  ///< From the third duplicate to the ACK past `recover_`.
        bool loss_episode_              = false;  ///< After a timeout: each ACK short of `recover_` resends the head.
        std::uint32_t limited_transmit_ = 0;      ///< Bytes beyond the windows the first two duplicates allow.
        State congestion_{};                      ///< The policy's state; reset where the connection is Established.
    };

}  // namespace aloe::tcp::detail
