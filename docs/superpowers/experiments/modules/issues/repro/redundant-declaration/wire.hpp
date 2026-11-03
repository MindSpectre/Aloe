#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace probe::wire {

    struct MacAddress {
        std::array<std::uint8_t, 6> bytes{};
        [[nodiscard]] constexpr bool is_broadcast() const noexcept {
            return std::ranges::all_of(bytes, [](const std::uint8_t b) { return b == 0xFF; });
        }
    };

    template <typename T>
    [[nodiscard]] constexpr std::uint32_t sum(const std::span<const T> values) noexcept {
        std::uint32_t total = 0;
        for (const T v : values) {
            total += v;
        }
        return total;
    }

}  // namespace probe::wire
