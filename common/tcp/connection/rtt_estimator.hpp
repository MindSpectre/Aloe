#pragma once

#include <algorithm>
#include <aloe/core>
#include <chrono>
#include <optional>

namespace aloe::tcp::detail {

    /**
     * @brief RFC 6298's round-trip estimator and the retransmission timeout it gives, with Linux's floor.
     *
     * Before the first sample the RTO is the configured initial one; each expiry of the timer doubles it up to the
     * ceiling, and the next sample ends that backoff.
     */
    class RttEstimator {
    public:
        /// No sample yet: the RTO is `initial`.
        void reset(const core::Duration initial) noexcept {
            srtt_ = std::nullopt;
            rto_  = initial;
        }

        /**
         * One sample: the first sets `srtt = R` and `rttvar = R / 2`, each later one `rttvar = 3/4 rttvar + 1/4 |srtt -
         * R|` and then `srtt = 7/8 srtt + 1/8 R`, in integer nanoseconds, truncating. The RTO follows with Linux's
         * floor and ceiling, `srtt + max(4 rttvar, floor)` capped at `ceiling`, which ends any backoff.
         */
        void sample(const core::Duration sample, const core::Duration floor, const core::Duration ceiling) noexcept {
            if (!srtt_) {
                srtt_   = sample;
                rttvar_ = sample / 2;
            } else {
                rttvar_ = (3 * rttvar_ + std::chrono::abs(*srtt_ - sample)) / 4;
                srtt_   = (7 * *srtt_ + sample) / 8;
            }
            rto_ = std::min(*srtt_ + std::max(4 * rttvar_, floor), ceiling);
        }

        /// An expiry of the timer: the RTO doubles, up to `ceiling`.
        void back_off(const core::Duration ceiling) noexcept {
            rto_ = std::min(rto_ * 2, ceiling);
        }

        /// The smoothed round-trip time; nothing before the first sample.
        [[nodiscard]] std::optional<core::Duration> srtt() const noexcept {
            return srtt_;
        }

        /// The retransmission timeout now, the backoff included.
        [[nodiscard]] core::Duration rto() const noexcept {
            return rto_;
        }

    private:
        std::optional<core::Duration> srtt_{};
        core::Duration rttvar_{};
        core::Duration rto_{};
    };

}  // namespace aloe::tcp::detail
