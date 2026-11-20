#pragma once

#include <cstdint>
#include <limits>

#include <congestion_control.hpp>

namespace aloe::tcp {

    /// The peer's window is the only limit: for private cross-connects. Loss recovery still happens; this is not a
    /// stripped stack. Its target is unbounded too, so recovery runs no rate reduction over it: what is marked lost
    /// goes again as soon as it is marked.
    struct NoCongestionControl {
        struct State {};

        static constexpr void reset(State& /*state*/, const std::uint16_t /*mss*/) noexcept {
        }

        [[nodiscard]] static constexpr std::uint32_t window(const State& /*state*/) noexcept {
            return std::numeric_limits<std::uint32_t>::max();
        }

        [[nodiscard]] static constexpr std::uint32_t target(const State& /*state*/) noexcept {
            return std::numeric_limits<std::uint32_t>::max();
        }

        static constexpr void on_ack(State& /*state*/, const AckInfo& /*info*/) noexcept {
        }

        static constexpr void
        on_congestion(State& /*state*/, const std::uint32_t /*flight*/, const std::uint16_t /*mss*/) noexcept {
        }

        static constexpr void on_recovery_end(State& /*state*/) noexcept {
        }

        static constexpr void
        on_timeout(State& /*state*/, const std::uint32_t /*in_flight*/, const std::uint16_t /*mss*/) noexcept {
        }

        static constexpr void on_idle(State& /*state*/, const std::uint16_t /*mss*/) noexcept {
        }

        static constexpr void on_undo(State& /*state*/, const State& /*prior*/) noexcept {
        }
    };

    static_assert(IsCongestionControl<NoCongestionControl>);

}  // namespace aloe::tcp
