#include <algorithm>
#include <aloe/wire>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include <gtest/gtest.h>

namespace {

    constexpr aloe::wire::MacAddress alpha{0x02, 0, 0, 0, 0, 0x01};
    constexpr aloe::wire::MacAddress beta{0x02, 0, 0, 0, 0, 0x02};
    constexpr aloe::wire::Ipv4Address alpha_ip{10, 0, 0, 1};
    constexpr aloe::wire::Ipv4Address beta_ip{10, 0, 0, 2};

    /// A 20-byte header written into a payload of `size` bytes, so parse sees a total length that fits.
    template <std::size_t Size>
    [[nodiscard]] std::array<std::byte, Size> ipv4_payload(const aloe::wire::Ipv4Header& header) {
        std::array<std::byte, Size> payload{};
        header.write(payload);
        return payload;
    }

    constexpr aloe::wire::Ipv4Header udp_header{.total_length   = 48,
                                                .identification = 0x1234,
                                                .ttl            = 64,
                                                .protocol       = aloe::wire::Ipv4Protocol::Udp,
                                                .checksum       = 0xabcd,
                                                .source         = alpha_ip,
                                                .destination    = beta_ip};

}  // namespace

TEST(WireFormats, EthernetRoundTripsAndRefusesAShortFrame) {
    std::array<std::byte, 14> bytes{};
    const aloe::wire::EthernetHeader header{
        .destination = beta, .source = alpha, .ethertype = std::to_underlying(aloe::wire::EtherType::Arp)};
    header.write(bytes);
    EXPECT_EQ(bytes[12], std::byte{0x08});
    EXPECT_EQ(bytes[13], std::byte{0x06});
    EXPECT_EQ(aloe::wire::EthernetHeader::parse(bytes), header);
    EXPECT_FALSE(aloe::wire::EthernetHeader::parse(std::span{bytes}.first(13)).has_value());
}

TEST(WireFormats, ArpRoundTripsAndIgnoresPadding) {
    std::array<std::byte, 46> bytes{};  // as a minimum-size frame carries it: 28 bytes and padding
    const aloe::wire::ArpPacket request{.operation  = aloe::wire::ArpOperation::Request,
                                        .sender_mac = alpha,
                                        .sender_ip  = alpha_ip,
                                        .target_mac = {},
                                        .target_ip  = beta_ip};
    request.write(bytes);
    EXPECT_EQ(bytes[7], std::byte{1});
    EXPECT_EQ(aloe::wire::ArpPacket::parse(bytes), request);
    EXPECT_FALSE(aloe::wire::ArpPacket::parse(std::span{bytes}.first(27)).has_value());
}

TEST(WireFormats, ArpRefusesWhatIsNotEthernetOverIpv4) {
    std::array<std::byte, 28> bytes{};
    aloe::wire::ArpPacket{.operation  = aloe::wire::ArpOperation::Reply,
                          .sender_mac = alpha,
                          .sender_ip  = alpha_ip,
                          .target_mac = beta,
                          .target_ip  = beta_ip}
        .write(bytes);
    ASSERT_TRUE(aloe::wire::ArpPacket::parse(bytes).has_value());

    auto token_ring = bytes;
    token_ring[1]   = std::byte{6};
    EXPECT_FALSE(aloe::wire::ArpPacket::parse(token_ring).has_value()) << "hardware type";

    auto ipv6 = bytes;
    ipv6[2]   = std::byte{0x86};
    ipv6[3]   = std::byte{0xdd};
    EXPECT_FALSE(aloe::wire::ArpPacket::parse(ipv6).has_value()) << "protocol type";

    auto long_hardware = bytes;
    long_hardware[4]   = std::byte{8};
    EXPECT_FALSE(aloe::wire::ArpPacket::parse(long_hardware).has_value()) << "hardware address length";

    auto long_protocol = bytes;
    long_protocol[5]   = std::byte{16};
    EXPECT_FALSE(aloe::wire::ArpPacket::parse(long_protocol).has_value()) << "protocol address length";

    auto rarp = bytes;
    rarp[7]   = std::byte{3};
    EXPECT_FALSE(aloe::wire::ArpPacket::parse(rarp).has_value()) << "only request and reply";
}

TEST(WireFormats, Ipv4RoundTripsWithDontFragmentSet) {
    const auto payload = ipv4_payload<48>(udp_header);
    EXPECT_EQ(payload[0], std::byte{0x45});
    EXPECT_EQ(aloe::wire::load_be16(std::span{payload}.subspan(6, 2)), 0x4000) << "DF, no offset";
    EXPECT_EQ(payload[9], std::byte{17});
    const auto parsed = aloe::wire::Ipv4Header::parse(payload);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, udp_header);
    EXPECT_FALSE(parsed->is_fragment());
}

TEST(WireFormats, Ipv4FragmentFlagsRoundTrip) {
    aloe::wire::Ipv4Header fragment = udp_header;
    fragment.dont_fragment          = false;
    fragment.more_fragments         = true;
    fragment.fragment_offset        = 0x0123;
    const auto payload              = ipv4_payload<48>(fragment);
    EXPECT_EQ(aloe::wire::load_be16(std::span{payload}.subspan(6, 2)), 0x2123);
    const auto parsed = aloe::wire::Ipv4Header::parse(payload);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, fragment);
    EXPECT_TRUE(parsed->is_fragment());

    aloe::wire::Ipv4Header last = udp_header;
    last.fragment_offset        = 1;
    EXPECT_TRUE(aloe::wire::Ipv4Header::parse(ipv4_payload<48>(last))->is_fragment())
        << "an offset alone makes a fragment";
}

TEST(WireFormats, Ipv4RefusesWhatItCannotTrust) {
    const auto good = ipv4_payload<48>(udp_header);
    ASSERT_TRUE(aloe::wire::Ipv4Header::parse(good).has_value());

    auto version_six = good;
    version_six[0]   = std::byte{0x65};
    EXPECT_FALSE(aloe::wire::Ipv4Header::parse(version_six).has_value()) << "version";

    auto four_words = good;
    four_words[0]   = std::byte{0x44};
    EXPECT_FALSE(aloe::wire::Ipv4Header::parse(four_words).has_value()) << "header length under 20";

    auto past_the_span = good;
    aloe::wire::store_be16(std::span{past_the_span}.subspan(2, 2), 49);
    EXPECT_FALSE(aloe::wire::Ipv4Header::parse(past_the_span).has_value()) << "total length past the payload";

    auto under_the_header = good;
    aloe::wire::store_be16(std::span{under_the_header}.subspan(2, 2), 19);
    EXPECT_FALSE(aloe::wire::Ipv4Header::parse(under_the_header).has_value()) << "total length under the header";

    EXPECT_FALSE(aloe::wire::Ipv4Header::parse(std::span{good}.first(19)).has_value()) << "short payload";
}

TEST(WireFormats, Ipv4AcceptsOptionsAndPadding) {
    std::array<std::byte, 60> payload{};  // 24-byte header, 48 bytes total, 12 bytes of padding
    udp_header.write(payload);
    payload[0]        = std::byte{0x46};
    const auto parsed = aloe::wire::Ipv4Header::parse(payload);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->header_length, 24);
    EXPECT_EQ(parsed->total_length, 48) << "padding past the total length is ignored";
}

TEST(WireFormats, IcmpRoundTripsAndRefusesAShortMessage) {
    std::array<std::byte, 8> bytes{};
    const aloe::wire::IcmpHeader header{
        .type = aloe::wire::IcmpType::EchoRequest, .code = 0, .checksum = 0xf7ff, .rest = 0x12340001};
    header.write(bytes);
    EXPECT_EQ(bytes[0], std::byte{8});
    EXPECT_EQ(aloe::wire::IcmpHeader::parse(bytes), header);
    EXPECT_FALSE(aloe::wire::IcmpHeader::parse(std::span{bytes}.first(7)).has_value());
}

TEST(WireFormats, MulticastMacTakesTheLowTwentyThreeBits) {
    using aloe::wire::Ipv4Address;
    using aloe::wire::MacAddress;
    constexpr Ipv4Address all_hosts{224, 0, 0, 1};
    constexpr Ipv4Address high_bit{239, 255, 1, 2};
    EXPECT_EQ(all_hosts.multicast_mac(), MacAddress(0x01, 0x00, 0x5e, 0x00, 0x00, 0x01));
    EXPECT_EQ(high_bit.multicast_mac(), MacAddress(0x01, 0x00, 0x5e, 0x7f, 0x01, 0x02))
        << "the high bit of the second octet is dropped";
    constexpr Ipv4Address upper{224, 128, 1, 2};
    constexpr Ipv4Address lower{224, 0, 1, 2};
    EXPECT_EQ(upper.multicast_mac(), lower.multicast_mac()) << "32 groups share one MAC";
}
