#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

#include <congestion_control.hpp>

namespace aloe::tcp {

    /// RFC 5681 slow start and congestion avoidance, RFC 6582 recovery, RFC 6928's initial window.
    struct NewReno {
        struct State {
            std::uint32_t cwnd     = 0;
            std::uint32_t ssthresh = std::numeric_limits<std::uint32_t>::max();
            std::uint32_t acked_bytes =
                0;  ///< Congestion avoidance's accumulator: one segment per window acknowledged.
        };

        /// min(10 mss, max(2 mss, 14600)).
        [[nodiscard]] static constexpr std::uint32_t initial_window(const std::uint16_t mss) noexcept {
            const std::uint64_t segment = mss;
            return static_cast<std::uint32_t>(
                std::min<std::uint64_t>(10 * segment, std::max<std::uint64_t>(2 * segment, 14600)));
        }

        static constexpr void reset(State& state, const std::uint16_t mss) noexcept {
            state = State{
                .cwnd = initial_window(mss), .ssthresh = std::numeric_limits<std::uint32_t>::max(), .acked_bytes = 0};
        }

        [[nodiscard]] static constexpr std::uint32_t window(const State& state) noexcept {
            return state.cwnd;
        }

        static constexpr void on_ack(State& state, const AckInfo& info) noexcept {
            if (state.cwnd < state.ssthresh) {
                state.cwnd = add(state.cwnd, std::min<std::uint32_t>(info.acked, info.mss));
                return;
            }
            state.acked_bytes = add(state.acked_bytes, info.acked);
            while (state.cwnd != 0 && state.acked_bytes >= state.cwnd) {
                state.acked_bytes -= state.cwnd;
                state.cwnd         = add(state.cwnd, info.mss);
            }
        }

        static constexpr void on_loss(State& state, const std::uint32_t flight, const std::uint16_t mss) noexcept {
            state.ssthresh    = std::max<std::uint32_t>(flight / 2, 2U * mss);
            state.cwnd        = add(state.ssthresh, 3U * mss);
            state.acked_bytes = 0;
        }

        static constexpr void on_dupack(State& state, const std::uint16_t mss) noexcept {
            state.cwnd = add(state.cwnd, mss);
        }

        static constexpr void
        on_partial_ack(State& state, const std::uint32_t acked, const std::uint16_t mss) noexcept {
            state.cwnd = state.cwnd > acked ? state.cwnd - acked : 0;
            if (acked >= mss) {
                state.cwnd = add(state.cwnd, mss);
            }
            state.cwnd = std::max<std::uint32_t>(state.cwnd, mss);
        }

        static constexpr void
        on_recovery_end(State& state, const std::uint32_t flight, const std::uint16_t mss) noexcept {
            state.cwnd        = std::min(state.ssthresh, add(std::max<std::uint32_t>(flight, mss), mss));
            state.acked_bytes = 0;
        }

        static constexpr void on_timeout(State& state, const std::uint32_t flight, const std::uint16_t mss) noexcept {
            state.ssthresh    = std::max<std::uint32_t>(flight / 2, 2U * mss);
            state.cwnd        = mss;
            state.acked_bytes = 0;
        }

        /// RFC 5681 §4.1's restart window, as Linux's `tcp_cwnd_restart` floors it: never above the window it had.
        static constexpr void on_idle(State& state, const std::uint16_t mss) noexcept {
            state.cwnd        = std::min(state.cwnd, initial_window(mss));
            state.acked_bytes = 0;
        }

    private:
        [[nodiscard]] static constexpr std::uint32_t add(const std::uint32_t a, const std::uint32_t b) noexcept {
            return b > std::numeric_limits<std::uint32_t>::max() - a ? std::numeric_limits<std::uint32_t>::max()
                                                                     : a + b;
        }
    };

    static_assert(IsCongestionControl<NewReno>);

}  // namespace aloe::tcp
