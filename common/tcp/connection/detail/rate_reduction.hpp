#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace aloe::tcp::detail {

    /**
     * @brief Proportional Rate Reduction, RFC 9937 in bytes: the congestion window while a loss is repaired, on the way
     * from the window the policy had to the target it set.
     *
     * It owns the episode's counts, what the ACKs delivered and what was sent, and the window each step leaves:
     * what is in flight plus what the step allows. While what is in flight is above the target, sending follows the
     * proportional part, the target's share of what was delivered; at or below it, the conservative bound, what was
     * delivered and not yet sent, plus one segment on an ACK that showed progress, never past the target. What is in
     * flight and what an ACK delivered are the caller's to say: they differ with and without SACK.
     */
    class RateReduction {
    public:
        /// RFC 9937 6.1: an episode begins, nothing delivered and nothing sent. `recover_fs` is RFC 9937's RecoverFS,
        /// what the episode may still see delivered; `target` the policy's, RFC 5681's ssthresh.
        void begin(const std::uint32_t recover_fs, const std::uint32_t target) noexcept {
            recover_fs_ = recover_fs;
            target_     = target;
            delivered_  = 0;
            out_        = 0;
            window_     = 0;
        }

        /**
         * RFC 9937 6.2, one ACK that delivered `delivered` bytes and left `in_flight`. `safe`: it moved SND.UNA and
         * marked nothing new lost, the RFC's SafeACK. An ACK that delivered nothing changes nothing. The first step of
         * an episode always allows something, so the first resend leaves at once.
         *
         * The proportional part divides by RecoverFS, as the RFC has it; Linux's `tcp_cwnd_reduction` divides by the
         * window it had on entry.
         */
        void on_ack(const std::uint32_t delivered,
                    const std::uint32_t in_flight,
                    const bool safe,
                    const std::uint16_t mss) noexcept {
            if (delivered == 0) {
                return;
            }
            delivered_         = add(delivered_, delivered);
            std::int64_t count = 0;  // the RFC's SndCnt
            if (in_flight > target_) {
                const std::uint64_t flight = std::max<std::uint32_t>(recover_fs_, 1);
                const std::uint64_t share  = (std::uint64_t{delivered_} * target_ + flight - 1) / flight;  // rounded up
                count                      = static_cast<std::int64_t>(share) - out_;
            } else {
                count = std::max<std::int64_t>(std::int64_t{delivered_} - out_, delivered);
                if (safe) {
                    count += mss;
                }
                count = std::min<std::int64_t>(std::int64_t{target_} - in_flight, count);
            }
            if (out_ == 0 && count == 0) {
                count = mss;  // the RFC's forced fast retransmit on entering recovery
            }
            // Below zero counts as zero: more was sent than the step allows, and nothing more may leave.
            window_ = add(in_flight, static_cast<std::uint32_t>(std::clamp<std::int64_t>(count, 0, largest)));
        }

        /// An episode begun or continued with no ACK, at the reordering timer: one segment may leave.
        void allow_one(const std::uint32_t in_flight, const std::uint16_t mss) noexcept {
            window_ = add(in_flight, mss);
        }

        /// RFC 9937 6.3: `bytes` of sequence space left, new or resent.
        void on_sent(const std::uint32_t bytes) noexcept {
            out_ = add(out_, bytes);
        }

        /// The congestion window the last step left.
        [[nodiscard]] std::uint32_t window() const noexcept {
            return window_;
        }

        /// What the episode has counted as delivered, for the bound without SACK.
        [[nodiscard]] std::uint32_t delivered() const noexcept {
            return delivered_;
        }

        [[nodiscard]] std::uint32_t recover_fs() const noexcept {
            return recover_fs_;
        }

    private:
        static constexpr std::uint32_t largest = std::numeric_limits<std::uint32_t>::max();

        [[nodiscard]] static std::uint32_t add(const std::uint32_t a, const std::uint32_t b) noexcept {
            return b > largest - a ? largest : a + b;
        }

        std::uint32_t recover_fs_ = 0;  ///< RFC 9937's RecoverFS: constant through the episode.
        std::uint32_t target_     = 0;  ///< The policy's target when the episode began.
        std::uint32_t delivered_  = 0;  ///< RFC 9937's prr_delivered, saturating.
        std::uint32_t out_        = 0;  ///< RFC 9937's prr_out, saturating.
        std::uint32_t window_     = 0;  ///< What is in flight plus what the last step allowed.
    };

}  // namespace aloe::tcp::detail
