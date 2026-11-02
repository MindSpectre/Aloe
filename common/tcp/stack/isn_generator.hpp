#pragma once

#include <cstdint>
#include <optional>
#include <random>
#include <utility>

#include <tcp_sequence.hpp>

namespace aloe::tcp::detail {

    /**
     * @brief The shard's source of initial sequence numbers and timestamp clock offsets: one engine, seeded once.
     *
     * A test may name the next initial sequence number (`force`); the offsets stay drawn.
     */
    class IsnGenerator {
    public:
        /// The forced value once, when one is set; a drawn one otherwise.
        [[nodiscard]] wire::TcpSequence next() noexcept {
            if (forced_) {
                return *std::exchange(forced_, std::nullopt);
            }
            return wire::TcpSequence{static_cast<std::uint32_t>(engine_())};
        }

        /// A connection's offset to the stack's millisecond clock, so its timestamps start nowhere predictable.
        [[nodiscard]] std::uint32_t offset() noexcept {
            return static_cast<std::uint32_t>(engine_());
        }

        void force(const wire::TcpSequence isn) noexcept {
            forced_ = isn;
        }

    private:
        std::mt19937 engine_{std::random_device{}()};
        std::optional<wire::TcpSequence> forced_{};
    };

}  // namespace aloe::tcp::detail
