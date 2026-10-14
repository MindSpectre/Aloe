#pragma once

#include <cstdint>

namespace aloe::wire {

    /**
     * @brief A TCP sequence number with RFC 793 serial arithmetic.
     *
     * `a - b` is the wrapped signed distance, so `before` and `after` compare across the wrap: a
     * number half the space away or less counts as later. Unsigned addition wraps by definition;
     * the conversion to `std::int32_t` is modular since C++20, so nothing here overflows.
     */
    struct TcpSequence {
        std::uint32_t value = 0;

        [[nodiscard]] constexpr TcpSequence operator+(const std::uint32_t count) const noexcept {
            return TcpSequence{value + count};
        }

        /// This minus `other`, wrapped into the signed range.
        [[nodiscard]] constexpr std::int32_t operator-(const TcpSequence other) const noexcept {
            return static_cast<std::int32_t>(value - other.value);
        }

        [[nodiscard]] constexpr bool before(const TcpSequence other) const noexcept {
            return (*this - other) < 0;
        }

        [[nodiscard]] constexpr bool after(const TcpSequence other) const noexcept {
            return (*this - other) > 0;
        }

        friend constexpr bool operator==(TcpSequence, TcpSequence) noexcept = default;
    };

}  // namespace aloe::wire
