#include <algorithm>
#include <aloe/net>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include <gtest/gtest.h>

namespace {

    constexpr aloe::device::MacAddress alpha{0x02, 0, 0, 0, 0, 0x01};
    constexpr aloe::device::MacAddress beta{0x02, 0, 0, 0, 0, 0x02};
    constexpr aloe::device::Ipv4Address alpha_ip{10, 0, 0, 1};
    constexpr aloe::device::Ipv4Address beta_ip{10, 0, 0, 2};

    /// A 20-byte header written into a payload of `size` bytes, so parse sees a total length that fits.
    template <std::size_t Size>
    [[nodiscard]] std::array<std::byte, Size> ipv4_payload(const aloe::net::Ipv4Header& header) {
        std::array<std::byte, Size> payload{};
        aloe::net::write_ipv4(payload, header);
        return payload;
    }

    constexpr aloe::net::Ipv4Header udp_header{.total_length   = 48,
                                               .identification = 0x1234,
                                               .ttl            = 64,
                                               .protocol       = aloe::device::Ipv4Protocol::Udp,
                                               .checksum       = 0xabcd,
                                               .source         = alpha_ip,
                                               .destination    = beta_ip};

}  // namespace

TEST(NetWire, EthernetRoundTripsAndRefusesAShortFrame) {
    std::array<std::byte, 14> bytes{};
    const aloe::net::EthernetHeader header{
        .destination = beta, .source = alpha, .ethertype = std::to_underlying(aloe::net::EtherType::Arp)};
    aloe::net::write_ethernet(bytes, header);
    EXPECT_EQ(bytes[12], std::byte{0x08});
    EXPECT_EQ(bytes[13], std::byte{0x06});
    EXPECT_EQ(aloe::net::parse_ethernet(bytes), header);
    EXPECT_FALSE(aloe::net::parse_ethernet(std::span{bytes}.first(13)).has_value());
}

TEST(NetWire, ArpRoundTripsAndIgnoresPadding) {
    std::array<std::byte, 46> bytes{};  // as a minimum-size frame carries it: 28 bytes and padding
    const aloe::net::ArpPacket request{.operation  = aloe::net::ArpOperation::Request,
                                       .sender_mac = alpha,
                                       .sender_ip  = alpha_ip,
                                       .target_mac = {},
                                       .target_ip  = beta_ip};
    aloe::net::write_arp(bytes, request);
    EXPECT_EQ(bytes[7], std::byte{1});
    EXPECT_EQ(aloe::net::parse_arp(bytes), request);
    EXPECT_FALSE(aloe::net::parse_arp(std::span{bytes}.first(27)).has_value());
}

TEST(NetWire, ArpRefusesWhatIsNotEthernetOverIpv4) {
    std::array<std::byte, 28> bytes{};
    aloe::net::write_arp(bytes,
                         {.operation  = aloe::net::ArpOperation::Reply,
                          .sender_mac = alpha,
                          .sender_ip  = alpha_ip,
                          .target_mac = beta,
                          .target_ip  = beta_ip});
    ASSERT_TRUE(aloe::net::parse_arp(bytes).has_value());

    auto token_ring = bytes;
    token_ring[1]   = std::byte{6};
    EXPECT_FALSE(aloe::net::parse_arp(token_ring).has_value()) << "hardware type";

    auto ipv6 = bytes;
    ipv6[2]   = std::byte{0x86};
    ipv6[3]   = std::byte{0xdd};
    EXPECT_FALSE(aloe::net::parse_arp(ipv6).has_value()) << "protocol type";

    auto long_hardware = bytes;
    long_hardware[4]   = std::byte{8};
    EXPECT_FALSE(aloe::net::parse_arp(long_hardware).has_value()) << "hardware address length";

    auto long_protocol = bytes;
    long_protocol[5]   = std::byte{16};
    EXPECT_FALSE(aloe::net::parse_arp(long_protocol).has_value()) << "protocol address length";

    auto rarp = bytes;
    rarp[7]   = std::byte{3};
    EXPECT_FALSE(aloe::net::parse_arp(rarp).has_value()) << "only request and reply";
}

TEST(NetWire, Ipv4RoundTripsWithDontFragmentSet) {
    const auto payload = ipv4_payload<48>(udp_header);
    EXPECT_EQ(payload[0], std::byte{0x45});
    EXPECT_EQ(aloe::device::load_be16(std::span{payload}.subspan(6, 2)), 0x4000) << "DF, no offset";
    EXPECT_EQ(payload[9], std::byte{17});
    const auto parsed = aloe::net::parse_ipv4(payload);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, udp_header);
    EXPECT_FALSE(parsed->is_fragment());
}

TEST(NetWire, Ipv4FragmentFlagsRoundTrip) {
    aloe::net::Ipv4Header fragment = udp_header;
    fragment.dont_fragment         = false;
    fragment.more_fragments        = true;
    fragment.fragment_offset       = 0x0123;
    const auto payload             = ipv4_payload<48>(fragment);
    EXPECT_EQ(aloe::device::load_be16(std::span{payload}.subspan(6, 2)), 0x2123);
    const auto parsed = aloe::net::parse_ipv4(payload);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, fragment);
    EXPECT_TRUE(parsed->is_fragment());

    aloe::net::Ipv4Header last = udp_header;
    last.fragment_offset       = 1;
    EXPECT_TRUE(aloe::net::parse_ipv4(ipv4_payload<48>(last))->is_fragment()) << "an offset alone makes a fragment";
}

TEST(NetWire, Ipv4RefusesWhatItCannotTrust) {
    const auto good = ipv4_payload<48>(udp_header);
    ASSERT_TRUE(aloe::net::parse_ipv4(good).has_value());

    auto version_six = good;
    version_six[0]   = std::byte{0x65};
    EXPECT_FALSE(aloe::net::parse_ipv4(version_six).has_value()) << "version";

    auto four_words = good;
    four_words[0]   = std::byte{0x44};
    EXPECT_FALSE(aloe::net::parse_ipv4(four_words).has_value()) << "header length under 20";

    auto past_the_span = good;
    aloe::device::store_be16(std::span{past_the_span}.subspan(2, 2), 49);
    EXPECT_FALSE(aloe::net::parse_ipv4(past_the_span).has_value()) << "total length past the payload";

    auto under_the_header = good;
    aloe::device::store_be16(std::span{under_the_header}.subspan(2, 2), 19);
    EXPECT_FALSE(aloe::net::parse_ipv4(under_the_header).has_value()) << "total length under the header";

    EXPECT_FALSE(aloe::net::parse_ipv4(std::span{good}.first(19)).has_value()) << "short payload";
}

TEST(NetWire, Ipv4AcceptsOptionsAndPadding) {
    std::array<std::byte, 60> payload{};  // 24-byte header, 48 bytes total, 12 bytes of padding
    aloe::net::write_ipv4(payload, udp_header);
    payload[0]        = std::byte{0x46};
    const auto parsed = aloe::net::parse_ipv4(payload);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->header_length, 24);
    EXPECT_EQ(parsed->total_length, 48) << "padding past the total length is ignored";
}

TEST(NetWire, IcmpRoundTripsAndRefusesAShortMessage) {
    std::array<std::byte, 8> bytes{};
    const aloe::net::IcmpHeader header{
        .type = aloe::net::IcmpType::EchoRequest, .code = 0, .checksum = 0xf7ff, .rest = 0x12340001};
    aloe::net::write_icmp(bytes, header);
    EXPECT_EQ(bytes[0], std::byte{8});
    EXPECT_EQ(aloe::net::parse_icmp(bytes), header);
    EXPECT_FALSE(aloe::net::parse_icmp(std::span{bytes}.first(7)).has_value());
}

TEST(NetWire, MulticastMacTakesTheLowTwentyThreeBits) {
    using aloe::device::MacAddress;
    EXPECT_EQ(aloe::net::multicast_mac({224, 0, 0, 1}), MacAddress(0x01, 0x00, 0x5e, 0x00, 0x00, 0x01));
    EXPECT_EQ(aloe::net::multicast_mac({239, 255, 1, 2}), MacAddress(0x01, 0x00, 0x5e, 0x7f, 0x01, 0x02))
        << "the high bit of the second octet is dropped";
    EXPECT_EQ(aloe::net::multicast_mac({224, 128, 1, 2}), aloe::net::multicast_mac({224, 0, 1, 2}))
        << "32 groups share one MAC";
}
