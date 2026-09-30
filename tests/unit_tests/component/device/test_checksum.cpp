#include <aloe/device>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>

namespace {

    // The IPv4 header from the textbook checksum example: 20 bytes, checksum field 0xb861.
    constexpr std::array<std::byte, 20> ipv4_header = {
        std::byte{0x45}, std::byte{0x00}, std::byte{0x00}, std::byte{0x73}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x40}, std::byte{0x00}, std::byte{0x40}, std::byte{0x11},
        std::byte{0xb8}, std::byte{0x61}, std::byte{0xc0}, std::byte{0xa8}, std::byte{0x00},
        std::byte{0x01}, std::byte{0xc0}, std::byte{0xa8}, std::byte{0x00}, std::byte{0xc7},
    };

    constexpr std::array<std::byte, 20> ipv4_header_without_checksum = [] {
        auto header = ipv4_header;
        header[10]  = std::byte{0};
        header[11]  = std::byte{0};
        return header;
    }();

}  // namespace

TEST(Checksum, ComputesTheTextbookIpv4HeaderChecksum) {
    static_assert(aloe::internet_checksum(ipv4_header_without_checksum) == 0xb861);
    EXPECT_EQ(aloe::ipv4_header_checksum(ipv4_header_without_checksum), 0xb861);
    EXPECT_EQ(aloe::ipv4_header_checksum(ipv4_header), 0xb861) << "the field's own value is skipped";
}

TEST(Checksum, VerifiesAHeaderWithItsChecksumInPlaceAsZero) {
    EXPECT_EQ(aloe::internet_checksum(ipv4_header), 0);
    auto corrupted = ipv4_header;
    corrupted[16]  = std::byte{0xc1};
    EXPECT_NE(aloe::internet_checksum(corrupted), 0);
}

TEST(Checksum, CountsAnOddTrailingByteAsTheHighByteOfAWord) {
    constexpr std::array<std::byte, 3> odd = {std::byte{0x12}, std::byte{0x34}, std::byte{0x56}};
    EXPECT_EQ(aloe::checksum_add(0, odd), 0x1234U + 0x5600U);
}

TEST(Checksum, FoldsCarriesRepeatedly) {
    EXPECT_EQ(aloe::checksum_fold(0x1ffffU), 0x0001);
    EXPECT_EQ(aloe::checksum_fold(0xffffU), 0xffff);
    EXPECT_EQ(aloe::checksum_finish(0), 0xffff);
}

TEST(Checksum, CompletesAnL4ChecksumFromThePseudoHeaderSumLikeACard) {
    constexpr aloe::Ipv4Address source{192, 168, 0, 1};
    constexpr aloe::Ipv4Address destination{192, 168, 0, 199};
    // A UDP datagram: source port 1234, destination port 5678, length 12, checksum 0, payload "data".
    std::array<std::byte, 12> segment = {std::byte{0x04},
                                         std::byte{0xd2},
                                         std::byte{0x16},
                                         std::byte{0x2e},
                                         std::byte{0x00},
                                         std::byte{0x0c},
                                         std::byte{0x00},
                                         std::byte{0x00},
                                         std::byte{'d'},
                                         std::byte{'a'},
                                         std::byte{'t'},
                                         std::byte{'a'}};

    const std::uint16_t full = aloe::ipv4_l4_checksum(source, destination, aloe::ipv4_protocol_udp, segment);
    EXPECT_NE(full, 0);

    // The caller seeds the field with the pseudo-header sum; the device sums the segment as it is.
    aloe::store_be16(std::span<std::byte>{segment}.subspan(6, 2),
                     aloe::ipv4_pseudo_header_sum(source, destination, aloe::ipv4_protocol_udp, 12));
    EXPECT_EQ(aloe::internet_checksum(segment), full);

    // With the full checksum in place, verification over pseudo-header and segment yields zero.
    aloe::store_be16(std::span<std::byte>{segment}.subspan(6, 2), full);
    const std::uint32_t pseudo = aloe::ipv4_pseudo_header_sum(source, destination, aloe::ipv4_protocol_udp, 12);
    EXPECT_EQ(aloe::checksum_finish(aloe::checksum_add(pseudo, segment)), 0);
}
