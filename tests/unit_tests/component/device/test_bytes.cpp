#include <aloe/device>
#include <array>
#include <cstddef>
#include <span>

#include <gtest/gtest.h>

namespace {

    constexpr std::array<std::byte, 6> bytes = {
        std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}, std::byte{0x9a}, std::byte{0xbc}};

}  // namespace

TEST(Bytes, LoadsBigEndianValues) {
    static_assert(aloe::device::load_be16(bytes) == 0x1234);
    static_assert(aloe::device::load_be32(bytes) == 0x12345678U);
    EXPECT_EQ(aloe::device::load_be16(std::span<const std::byte>{bytes}.subspan(4)), 0x9abc);
    EXPECT_EQ(aloe::device::load_be32(std::span<const std::byte>{bytes}.subspan(2)), 0x56789abcU);
}

TEST(Bytes, StoresBigEndianValues) {
    std::array<std::byte, 6> buffer{};
    aloe::device::store_be16(std::span<std::byte>{buffer}.first(2), 0x1234);
    aloe::device::store_be32(std::span<std::byte>{buffer}.subspan(2), 0x56789abcU);
    EXPECT_EQ(buffer, bytes);
}
