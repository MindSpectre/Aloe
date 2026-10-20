#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <application_work.hpp>
#include <gtest/gtest.h>

namespace {
    using namespace aloe::benchmarks;
}

TEST(ApplicationWork, RoundNumberRoundTripsLittleEndian) {
    std::array<std::byte, 8> bytes{};
    write_round(bytes, std::uint64_t{0x0102030405060708});
    EXPECT_EQ(bytes[0], std::byte{0x08});
    EXPECT_EQ(bytes[7], std::byte{0x01});
    EXPECT_EQ(read_round(bytes), std::uint64_t{0x0102030405060708});
    write_round(bytes, 0);
    EXPECT_EQ(read_round(bytes), 0U);
}

TEST(ApplicationWork, OrderOpensWithTheRoundAndFillsToItsSize) {
    std::array<std::byte, order_size> order{};
    write_order(order, 42);
    EXPECT_EQ(read_round(order), 42U);
    EXPECT_EQ(order[round_prefix], std::byte{round_prefix});
    EXPECT_EQ(order[order_size - 1], std::byte{order_size - 1});
}

TEST(ApplicationWork, TickPayloadMatchesTheOrderLayout) {
    const std::vector<std::byte> tick = tick_payload(64, 7);
    EXPECT_EQ(tick.size(), 64U);
    EXPECT_EQ(read_round(tick), 7U);
    EXPECT_EQ(tick[63], std::byte{63});
}

TEST(ApplicationWork, FoldChainsAcrossChunksAndSeesEveryByte) {
    const std::vector<std::byte> whole = tick_payload(256, 1);
    const std::span<const std::byte> first{whole.data(), 100};
    const std::span<const std::byte> second{whole.data() + 100, 156};
    EXPECT_EQ(fold(whole), fold(second, fold(first)));
    std::vector<std::byte> changed = whole;
    changed[255]                   = std::byte{0};
    EXPECT_NE(fold(whole), fold(changed));
}
