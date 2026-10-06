#include <aloe/wire>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <span>

#include <gtest/gtest.h>

TEST(Ipv4Address, ConvertsBetweenOctetsAndHostOrderValue) {
    constexpr aloe::wire::Ipv4Address address{192, 168, 0, 1};
    static_assert(address.to_uint32() == 0xC0A80001U);
    static_assert(aloe::wire::Ipv4Address::from_uint32(0xC0A80001U) == address);
    EXPECT_EQ(address.bytes()[0], std::byte{192});
    EXPECT_EQ(address.bytes()[3], std::byte{1});
}

TEST(Ipv4Address, ParsesAndFormatsDottedDecimal) {
    const auto parsed = aloe::wire::Ipv4Address::parse("10.0.255.7");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, (aloe::wire::Ipv4Address{10, 0, 255, 7}));
    EXPECT_EQ(parsed->to_string(), "10.0.255.7");
    EXPECT_EQ(std::format("{}", *parsed), "10.0.255.7");
}

TEST(Ipv4Address, RejectsMalformedText) {
    EXPECT_FALSE(aloe::wire::Ipv4Address::parse("").has_value());
    EXPECT_FALSE(aloe::wire::Ipv4Address::parse("10.0.0").has_value());
    EXPECT_FALSE(aloe::wire::Ipv4Address::parse("10.0.0.0.1").has_value());
    EXPECT_FALSE(aloe::wire::Ipv4Address::parse("10.0.0.256").has_value());
    EXPECT_FALSE(aloe::wire::Ipv4Address::parse("10..0.1").has_value());
    EXPECT_FALSE(aloe::wire::Ipv4Address::parse("10.0.0.1.").has_value());
    EXPECT_FALSE(aloe::wire::Ipv4Address::parse("a.b.c.d").has_value());
}

TEST(Ipv4Address, LoadsAndStoresTheWireBytes) {
    constexpr aloe::wire::Ipv4Address address{192, 168, 0, 1};
    std::array<std::byte, 6> bytes{};
    address.store(std::span{bytes}.subspan(2));
    EXPECT_EQ(bytes[2], std::byte{192});
    EXPECT_EQ(bytes[5], std::byte{1});
    EXPECT_EQ(aloe::wire::Ipv4Address::load(std::span{bytes}.subspan(2)), address);
}

TEST(Ipv4Address, ClassifiesTheSpecialRanges) {
    using aloe::wire::Ipv4Address;
    static_assert(Ipv4Address{}.is_unspecified());
    static_assert(!Ipv4Address(0, 0, 0, 1).is_unspecified());
    static_assert(Ipv4Address::limited_broadcast().is_limited_broadcast());
    static_assert(!Ipv4Address(10, 0, 0, 255).is_limited_broadcast());
    static_assert(Ipv4Address(224, 0, 0, 1).is_multicast());
    static_assert(Ipv4Address(239, 255, 255, 255).is_multicast());
    static_assert(!Ipv4Address(223, 255, 255, 255).is_multicast());
    static_assert(!Ipv4Address(240, 0, 0, 0).is_multicast());
    static_assert(Ipv4Address(127, 0, 0, 1).is_loopback());
    static_assert(Ipv4Address(127, 255, 0, 9).is_loopback());
    static_assert(!Ipv4Address(128, 0, 0, 1).is_loopback());
}

TEST(Ipv4Subnet, DerivesMaskNetworkAndBroadcast) {
    constexpr aloe::wire::Ipv4Subnet subnet{
        {10, 1, 2, 3},
        24
    };
    static_assert(subnet.mask() == 0xFFFFFF00U);
    static_assert(subnet.network() == aloe::wire::Ipv4Address{10, 1, 2, 0});
    static_assert(subnet.broadcast() == aloe::wire::Ipv4Address{10, 1, 2, 255});
    static_assert(subnet.has_broadcast());
    EXPECT_TRUE(subnet.contains({10, 1, 2, 200}));
    EXPECT_FALSE(subnet.contains({10, 1, 3, 1}));
}

TEST(Ipv4Subnet, HandlesTheEdgePrefixes) {
    constexpr aloe::wire::Ipv4Subnet everything{
        {10, 1, 2, 3},
        0
    };
    static_assert(everything.mask() == 0U);
    EXPECT_TRUE(everything.contains({192, 168, 0, 1}));

    constexpr aloe::wire::Ipv4Subnet host{
        {10, 1, 2, 3},
        32
    };
    static_assert(host.mask() == 0xFFFFFFFFU);
    static_assert(!host.has_broadcast());
    EXPECT_TRUE(host.contains({10, 1, 2, 3}));
    EXPECT_FALSE(host.contains({10, 1, 2, 2}));

    static_assert(!aloe::wire::Ipv4Subnet({10, 1, 2, 3}, 31).has_broadcast());
    static_assert(aloe::wire::Ipv4Subnet({10, 1, 2, 3}, 30).has_broadcast());
}
