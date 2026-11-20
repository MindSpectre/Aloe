#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

#include <congestion_control.hpp>

namespace aloe::tcp {

    /// RFC 5681 slow start and congestion avoidance, its target on congestion and RFC 6928's initial window. The
    /// window while a loss is repaired is not kept here: the connection's rate reduction regulates it down to the
    /// target (RFC 9937), where RFC 6582 inflated and deflated it.
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

        [[nodiscard]] static constexpr std::uint32_t target(const State& state) noexcept {
            return state.ssthresh;
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

        /// RFC 5681 3.1, equation 4: the target is half of the flight, two segments at the least. Linux's reno halves
        /// the window (`tcp_reno_ssthresh`); the RFC is definite. The window is left as it is.
        static constexpr void
        on_congestion(State& state, const std::uint32_t flight, const std::uint16_t mss) noexcept {
            state.ssthresh    = std::max<std::uint32_t>(flight / 2, 2U * mss);
            state.acked_bytes = 0;
        }

        static constexpr void on_recovery_end(State& state) noexcept {
            state.cwnd        = state.ssthresh;
            state.acked_bytes = 0;
        }

        /// Linux's `tcp_enter_loss`: what is in flight after the timeout's marking, plus one segment.
        static constexpr void
        on_timeout(State& state, const std::uint32_t in_flight, const std::uint16_t mss) noexcept {
            state.cwnd        = add(in_flight, mss);
            state.acked_bytes = 0;
        }

        /// RFC 5681 §4.1's restart window, as Linux's `tcp_cwnd_restart` floors it: never above the window it had.
        static constexpr void on_idle(State& state, const std::uint16_t mss) noexcept {
            state.cwnd        = std::min(state.cwnd, initial_window(mss));
            state.acked_bytes = 0;
        }

        /// The larger of each pair, the window it has and the one it had, the target it has and the one it had, as
        /// Linux's `tcp_undo_cwnd_reduction` and `tcp_reno_undo_cwnd` restore them.
        static constexpr void on_undo(State& state, const State& prior) noexcept {
            state.cwnd     = std::max(state.cwnd, prior.cwnd);
            state.ssthresh = std::max(state.ssthresh, prior.ssthresh);
        }

    private:
        [[nodiscard]] static constexpr std::uint32_t add(const std::uint32_t a, const std::uint32_t b) noexcept {
            return b > std::numeric_limits<std::uint32_t>::max() - a ? std::numeric_limits<std::uint32_t>::max()
                                                                     : a + b;
        }
    };

    static_assert(IsCongestionControl<NewReno>);

}  // namespace aloe::tcp
