#pragma once

#include <algorithm>
#include <aloe/core>
#include <cstdint>
#include <optional>

namespace aloe::tcp::detail {

    /**
     * @brief A connection's zero-window probes: how many went unanswered, since when, and the next one's delay.
     *
     * Probing runs while the peer's window is zero and nothing is in flight. Every acceptable ACK answers: the count
     * and the stamp clear, as Linux's `tcp_ack` clears `icsk_probes_out`, so RFC 9293 keeps the connection open while
     * the peer acknowledges probes and the user timeout bounds a run of unanswered probes only.
     */
    class ZeroWindowProbe {
    public:
        /// What an expiry of the probe timer calls for.
        enum class Expiry : std::uint8_t {
            Probe,         ///< Send one probe; it counts as unanswered and the interval doubled.
            TimedOut,      ///< `limit` probes went unanswered.
            UserTimedOut,  ///< The user timeout passed since the first unanswered probe.
        };

        /// No probe out yet, nothing unanswered stamped, and the first probe one RTO away.
        void start(const core::Duration rto) noexcept {
            answered();
            interval_ = rto;
        }

        /// An acceptable ACK arrived.
        void answered() noexcept {
            out_     = 0;
            started_ = std::nullopt;
        }

        /**
         * The next probe's deadline: the interval from now; under a user timeout, while a probe is unanswered, never
         * later than the first unanswered probe plus the timeout, Linux's `tcp_clamp_probe0_to_user_timeout`, and
         * never earlier than now. An answer leaves the deadline as it was, so while the peer answers a probe goes at
         * least once per user timeout, and a peer that falls silent is found within about twice the timeout.
         */
        [[nodiscard]] core::TimePoint deadline(const core::TimePoint now,
                                               const core::Duration user_timeout) const noexcept {
            core::TimePoint deadline = now + interval_;
            if (user_timeout > core::Duration::zero() && started_) {
                deadline = std::min(deadline, std::max(now, *started_ + user_timeout));
            }
            return deadline;
        }

        /**
         * The timer ran out with the window still zero. The first expiry since the last answer stamps the first
         * unanswered probe, which the user timeout counts from, as Linux's `tcp_probe_timer` stamps
         * `icsk_probes_tstamp`; a later expiry ends the connection once the timeout passed since that stamp. Then
         * `limit` unanswered probes end it; else one probe goes, counted as unanswered until an answer clears the
         * count, and the interval doubles up to `ceiling`. A probe the device refuses counts all the same.
         */
        [[nodiscard]] Expiry expire(const core::TimePoint now,
                                    const core::Duration user_timeout,
                                    const std::uint8_t limit,
                                    const core::Duration ceiling) noexcept {
            if (!started_) {
                started_ = now;
            } else if (user_timeout > core::Duration::zero() && now - *started_ >= user_timeout) {
                return Expiry::UserTimedOut;
            }
            if (out_ >= limit) {
                return Expiry::TimedOut;
            }
            ++out_;
            interval_ = std::min(interval_ * 2, ceiling);
            return Expiry::Probe;
        }

    private:
        std::uint8_t out_ = 0;                      ///< Probes since the last answer.
        std::optional<core::TimePoint> started_{};  ///< The first unanswered probe: the user timeout's start.
        core::Duration interval_{};                 ///< The next probe's delay: the RTO, doubled per probe.
    };

}  // namespace aloe::tcp::detail
