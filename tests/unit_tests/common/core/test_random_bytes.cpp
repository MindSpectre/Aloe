#include <algorithm>
#include <aloe/core>
#include <array>
#include <cstddef>
#include <span>

#include <gtest/gtest.h>

namespace {

    [[nodiscard]] bool all_zero(const std::span<const std::byte> bytes) {
        return std::ranges::all_of(bytes, [](const std::byte value) { return value == std::byte{0}; });
    }

}  // namespace

TEST(RandomBytes, FillsTheSpanAndTwoDrawsDiffer) {
    std::array<std::byte, 32> first{};
    std::array<std::byte, 32> second{};
    aloe::core::random_bytes(first);
    aloe::core::random_bytes(second);

    // Each check fails by chance with probability 2^-256.
    EXPECT_FALSE(all_zero(first));
    EXPECT_FALSE(all_zero(second));
    EXPECT_NE(first, second);
}

TEST(RandomBytes, AnEmptySpanIsFine) {
    EXPECT_NO_THROW(aloe::core::random_bytes({}));
}
