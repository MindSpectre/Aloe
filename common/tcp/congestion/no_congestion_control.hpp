#pragma once

#include <cstdint>
#include <limits>

#include <congestion_control.hpp>

namespace aloe::tcp {

    /// The peer's window is the only limit: for private cross-connects. Loss recovery still happens; this is not a
    /// stripped stack.
    struct NoCongestionControl {
        struct State {};

        static constexpr void reset(State& /*state*/, const std::uint16_t /*mss*/) noexcept {
        }

        [[nodiscard]] static constexpr std::uint32_t window(const State& /*state*/) noexcept {
            return std::numeric_limits<std::uint32_t>::max();
        }

        static constexpr void on_ack(State& /*state*/, const AckInfo& /*info*/) noexcept {
        }

        static constexpr void
        on_loss(State& /*state*/, const std::uint32_t /*flight*/, const std::uint16_t /*mss*/) noexcept {
        }

        static constexpr void on_dupack(State& /*state*/, const std::uint16_t /*mss*/) noexcept {
        }

        static constexpr void
        on_partial_ack(State& /*state*/, const std::uint32_t /*acked*/, const std::uint16_t /*mss*/) noexcept {
        }

        static constexpr void
        on_recovery_end(State& /*state*/, const std::uint32_t /*flight*/, const std::uint16_t /*mss*/) noexcept {
        }

        static constexpr void
        on_timeout(State& /*state*/, const std::uint32_t /*flight*/, const std::uint16_t /*mss*/) noexcept {
        }

        static constexpr void on_idle(State& /*state*/, const std::uint16_t /*mss*/) noexcept {
        }
    };

}  // namespace aloe::tcp
