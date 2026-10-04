#include <aloe/device>
#include <format>
#include <optional>

#include <gtest/gtest.h>

namespace {

    constexpr aloe::device::MacAddress mac{0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    constexpr aloe::device::MacAddress::Bytes mac_bytes{
        std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}};

}  // namespace

TEST(MacAddress, ConstructsFromBytesInTransmissionOrder) {
    EXPECT_EQ(mac.bytes()[0], std::byte{0x02});
    EXPECT_EQ(mac.bytes()[5], std::byte{0x01});
    static_assert(mac == aloe::device::MacAddress{mac_bytes});
}

TEST(MacAddress, ParsesAndFormatsColonSeparatedHex) {
    const auto parsed = aloe::device::MacAddress::parse("02:00:00:00:00:01");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, mac);
    EXPECT_EQ(parsed->to_string(), "02:00:00:00:00:01");
    EXPECT_EQ(std::format("{}", *parsed), "02:00:00:00:00:01");

    const auto upper = aloe::device::MacAddress::parse("AA:BB:CC:DD:EE:FF");
    ASSERT_TRUE(upper.has_value());
    EXPECT_EQ(upper->to_string(), "aa:bb:cc:dd:ee:ff");
}

TEST(MacAddress, RejectsMalformedText) {
    EXPECT_FALSE(aloe::device::MacAddress::parse("").has_value());
    EXPECT_FALSE(aloe::device::MacAddress::parse("02:00:00:00:00").has_value());
    EXPECT_FALSE(aloe::device::MacAddress::parse("02:00:00:00:00:0g").has_value());
    EXPECT_FALSE(aloe::device::MacAddress::parse("02-00-00-00-00-01").has_value());
    EXPECT_FALSE(aloe::device::MacAddress::parse("02:00:00:00:00:011").has_value());
}

TEST(MacAddress, KnowsBroadcastAndMulticast) {
    EXPECT_TRUE(aloe::device::MacAddress::broadcast().is_broadcast());
    EXPECT_TRUE(aloe::device::MacAddress::broadcast().is_multicast());
    EXPECT_FALSE(mac.is_broadcast());
    EXPECT_FALSE(mac.is_multicast());
    constexpr aloe::device::MacAddress multicast{0x01, 0x00, 0x5e, 0x00, 0x00, 0x01};
    EXPECT_TRUE(multicast.is_multicast());
    EXPECT_FALSE(multicast.is_broadcast());
}

TEST(MacAddress, OrdersByBytes) {
    constexpr aloe::device::MacAddress lower{0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
    EXPECT_LT(lower, mac);
    EXPECT_NE(lower, mac);
}

TEST(Ipv4Address, ConvertsBetweenOctetsAndHostOrderValue) {
    constexpr aloe::device::Ipv4Address address{192, 168, 0, 1};
    static_assert(address.to_uint32() == 0xC0A80001U);
    static_assert(aloe::device::Ipv4Address::from_uint32(0xC0A80001U) == address);
    EXPECT_EQ(address.bytes()[0], std::byte{192});
    EXPECT_EQ(address.bytes()[3], std::byte{1});
}

TEST(Ipv4Address, ParsesAndFormatsDottedDecimal) {
    const auto parsed = aloe::device::Ipv4Address::parse("10.0.255.7");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, (aloe::device::Ipv4Address{10, 0, 255, 7}));
    EXPECT_EQ(parsed->to_string(), "10.0.255.7");
    EXPECT_EQ(std::format("{}", *parsed), "10.0.255.7");
}

TEST(Ipv4Address, RejectsMalformedText) {
    EXPECT_FALSE(aloe::device::Ipv4Address::parse("").has_value());
    EXPECT_FALSE(aloe::device::Ipv4Address::parse("10.0.0").has_value());
    EXPECT_FALSE(aloe::device::Ipv4Address::parse("10.0.0.0.1").has_value());
    EXPECT_FALSE(aloe::device::Ipv4Address::parse("10.0.0.256").has_value());
    EXPECT_FALSE(aloe::device::Ipv4Address::parse("10..0.1").has_value());
    EXPECT_FALSE(aloe::device::Ipv4Address::parse("10.0.0.1.").has_value());
    EXPECT_FALSE(aloe::device::Ipv4Address::parse("a.b.c.d").has_value());
}
