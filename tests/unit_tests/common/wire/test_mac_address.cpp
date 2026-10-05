#include <aloe/wire>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <span>

#include <gtest/gtest.h>

namespace {

    constexpr aloe::wire::MacAddress mac{0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    constexpr aloe::wire::MacAddress::Bytes mac_bytes{
        std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}};

}  // namespace

TEST(MacAddress, ConstructsFromBytesInTransmissionOrder) {
    EXPECT_EQ(mac.bytes()[0], std::byte{0x02});
    EXPECT_EQ(mac.bytes()[5], std::byte{0x01});
    static_assert(mac == aloe::wire::MacAddress{mac_bytes});
}

TEST(MacAddress, ParsesAndFormatsColonSeparatedHex) {
    const auto parsed = aloe::wire::MacAddress::parse("02:00:00:00:00:01");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, mac);
    EXPECT_EQ(parsed->to_string(), "02:00:00:00:00:01");
    EXPECT_EQ(std::format("{}", *parsed), "02:00:00:00:00:01");

    const auto upper = aloe::wire::MacAddress::parse("AA:BB:CC:DD:EE:FF");
    ASSERT_TRUE(upper.has_value());
    EXPECT_EQ(upper->to_string(), "aa:bb:cc:dd:ee:ff");
}

TEST(MacAddress, RejectsMalformedText) {
    EXPECT_FALSE(aloe::wire::MacAddress::parse("").has_value());
    EXPECT_FALSE(aloe::wire::MacAddress::parse("02:00:00:00:00").has_value());
    EXPECT_FALSE(aloe::wire::MacAddress::parse("02:00:00:00:00:0g").has_value());
    EXPECT_FALSE(aloe::wire::MacAddress::parse("02-00-00-00-00-01").has_value());
    EXPECT_FALSE(aloe::wire::MacAddress::parse("02:00:00:00:00:011").has_value());
}

TEST(MacAddress, KnowsBroadcastAndMulticast) {
    EXPECT_TRUE(aloe::wire::MacAddress::broadcast().is_broadcast());
    EXPECT_TRUE(aloe::wire::MacAddress::broadcast().is_multicast());
    EXPECT_FALSE(mac.is_broadcast());
    EXPECT_FALSE(mac.is_multicast());
    constexpr aloe::wire::MacAddress multicast{0x01, 0x00, 0x5e, 0x00, 0x00, 0x01};
    EXPECT_TRUE(multicast.is_multicast());
    EXPECT_FALSE(multicast.is_broadcast());
}

TEST(MacAddress, OrdersByBytes) {
    constexpr aloe::wire::MacAddress lower{0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
    EXPECT_LT(lower, mac);
    EXPECT_NE(lower, mac);
}

TEST(MacAddress, LoadsAndStoresTheWireBytes) {
    std::array<std::byte, 8> frame{};
    mac.store(std::span{frame}.subspan(1));
    EXPECT_EQ(frame[1], std::byte{0x02});
    EXPECT_EQ(frame[6], std::byte{0x01});
    EXPECT_EQ(aloe::wire::MacAddress::load(std::span{frame}.subspan(1)), mac);
}
