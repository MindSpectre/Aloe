#include <aloe/fabric>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <frames.hpp>
#include <gtest/gtest.h>

namespace {

    constexpr aloe::device::MacAddress client{0x02, 0, 0, 0, 0, 0x01};
    constexpr aloe::device::MacAddress server{0x02, 0, 0, 0, 0, 0x02};
    constexpr aloe::device::Ipv4Address client_ip{10, 0, 0, 1};
    constexpr aloe::device::Ipv4Address server_ip{10, 0, 0, 2};

    aloe::testing::Ipv4Spec spec(const aloe::device::Ipv4Protocol protocol, const aloe::testing::Checksums checksums) {
        return {.destination_mac  = server,
                .source_mac       = client,
                .source           = client_ip,
                .destination      = server_ip,
                .source_port      = 40000,
                .destination_port = 80,
                .protocol         = protocol,
                .checksums        = checksums};
    }

    class FabricOffloads : public testing::Test {
    protected:
        aloe::fabric::Fabric fabric_;
        aloe::fabric::Port& client_port_ =
            fabric_.add_port({.mac = client, .pool_size = 16, .offloads = aloe::fabric::EmulatedOffloads::Checksums});
        aloe::fabric::Port& server_port_ =
            fabric_.add_port({.mac = server, .pool_size = 16, .offloads = aloe::fabric::EmulatedOffloads::Checksums});

        /// Transmits `frame` from the client with `tx` set, and returns what the server received.
        aloe::fabric::Packet exchange(std::span<const std::byte> frame, const aloe::device::TxMetadata& tx) {
            auto packet = client_port_.allocate(0);
            EXPECT_TRUE(packet.has_value());
            EXPECT_TRUE(aloe::testing::fill(*packet, frame));
            packet->set_tx(tx);
            std::array<aloe::fabric::Packet, 1> burst{std::move(*packet)};
            EXPECT_EQ(client_port_.transmit(0, burst), 1);
            std::array<aloe::fabric::Packet, 1> received;
            EXPECT_EQ(server_port_.receive(0, received), 1);
            return std::move(received[0]);
        }
    };

    constexpr aloe::device::TxMetadata fill_both{.l2_length          = 14,
                                                 .l3_length          = 20,
                                                 .fill_ipv4_checksum = true,
                                                 .fill_l4_checksum   = aloe::device::L4Checksum::Udp};

}  // namespace

TEST_F(FabricOffloads, AChecksumPortReportsAllFourCapabilities) {
    const aloe::device::Capabilities& capabilities = server_port_.capabilities();
    EXPECT_TRUE(capabilities.rx_ipv4_checksum);
    EXPECT_TRUE(capabilities.rx_l4_checksum);
    EXPECT_TRUE(capabilities.tx_ipv4_checksum);
    EXPECT_TRUE(capabilities.tx_l4_checksum);
}

TEST_F(FabricOffloads, FillsBothChecksumsFromASeededFrame) {
    const auto seeded = aloe::testing::ipv4_frame(
        spec(aloe::device::Ipv4Protocol::Udp, aloe::testing::Checksums::Seeded), aloe::testing::pattern(9));
    const auto correct = aloe::testing::ipv4_frame(
        spec(aloe::device::Ipv4Protocol::Udp, aloe::testing::Checksums::Correct), aloe::testing::pattern(9));
    ASSERT_NE(seeded, correct);
    aloe::fabric::Packet received = exchange(seeded, fill_both);
    EXPECT_EQ(aloe::testing::bytes_of(received), correct);
    EXPECT_EQ(received.rx().l3, aloe::device::ChecksumVerdict::Good);
    EXPECT_EQ(received.rx().l4, aloe::device::ChecksumVerdict::Good);
}

TEST_F(FabricOffloads, FillsTcpToo) {
    const auto seeded = aloe::testing::ipv4_frame(
        spec(aloe::device::Ipv4Protocol::Tcp, aloe::testing::Checksums::Seeded), aloe::testing::pattern(9));
    const auto correct = aloe::testing::ipv4_frame(
        spec(aloe::device::Ipv4Protocol::Tcp, aloe::testing::Checksums::Correct), aloe::testing::pattern(9));
    aloe::device::TxMetadata tx   = fill_both;
    tx.fill_l4_checksum           = aloe::device::L4Checksum::Tcp;
    aloe::fabric::Packet received = exchange(seeded, tx);
    EXPECT_EQ(aloe::testing::bytes_of(received), correct);
    EXPECT_EQ(received.rx().l4, aloe::device::ChecksumVerdict::Good);
}

TEST_F(FabricOffloads, FillsOnlyWhatIsAsked) {
    const auto seeded = aloe::testing::ipv4_frame(
        spec(aloe::device::Ipv4Protocol::Udp, aloe::testing::Checksums::Seeded), aloe::testing::pattern(9));
    aloe::device::TxMetadata tx   = fill_both;
    tx.fill_l4_checksum           = aloe::device::L4Checksum::None;
    aloe::fabric::Packet received = exchange(seeded, tx);
    EXPECT_EQ(received.rx().l3, aloe::device::ChecksumVerdict::Good);
    EXPECT_EQ(received.rx().l4, aloe::device::ChecksumVerdict::Bad) << "the seed alone is not a checksum";
}

TEST_F(FabricOffloads, VerifiesWrongChecksumsAsBad) {
    const auto wrong = aloe::testing::ipv4_frame(spec(aloe::device::Ipv4Protocol::Udp, aloe::testing::Checksums::Wrong),
                                                 aloe::testing::pattern(9));
    aloe::fabric::Packet received = exchange(wrong, aloe::device::TxMetadata{});
    EXPECT_EQ(received.rx().l3, aloe::device::ChecksumVerdict::Bad);
    EXPECT_EQ(received.rx().l4, aloe::device::ChecksumVerdict::Bad);
}

TEST_F(FabricOffloads, LeavesNonIpv4AndUdpWithoutChecksumUnknown) {
    const auto plain = aloe::testing::ethernet_frame(
        server, client, aloe::testing::ethertype_experimental, aloe::testing::pattern(40));
    aloe::fabric::Packet received = exchange(plain, aloe::device::TxMetadata{});
    EXPECT_EQ(received.rx().l3, aloe::device::ChecksumVerdict::Unknown);
    EXPECT_EQ(received.rx().l4, aloe::device::ChecksumVerdict::Unknown);

    auto no_checksum = aloe::testing::ipv4_frame(
        spec(aloe::device::Ipv4Protocol::Udp, aloe::testing::Checksums::Correct), aloe::testing::pattern(9));
    no_checksum[14 + 20 + 6] = std::byte{0};
    no_checksum[14 + 20 + 7] = std::byte{0};
    received                 = exchange(no_checksum, aloe::device::TxMetadata{});
    EXPECT_EQ(received.rx().l3, aloe::device::ChecksumVerdict::Good);
    EXPECT_EQ(received.rx().l4, aloe::device::ChecksumVerdict::Unknown);
}

TEST(FabricOffloadsNone, APlainPortReportsNothingAndVerifiesNothing) {
    aloe::fabric::Fabric fabric;
    auto& a = fabric.add_port({.mac = client, .pool_size = 16});
    auto& b = fabric.add_port({.mac = server, .pool_size = 16});
    EXPECT_FALSE(b.capabilities().rx_ipv4_checksum);
    EXPECT_FALSE(b.capabilities().tx_l4_checksum);

    const auto wrong = aloe::testing::ipv4_frame(spec(aloe::device::Ipv4Protocol::Udp, aloe::testing::Checksums::Wrong),
                                                 aloe::testing::pattern(9));
    auto packet      = a.allocate(0);
    ASSERT_TRUE(packet.has_value());
    ASSERT_TRUE(aloe::testing::fill(*packet, wrong));
    std::array<aloe::fabric::Packet, 1> burst{std::move(*packet)};
    ASSERT_EQ(a.transmit(0, burst), 1);
    std::array<aloe::fabric::Packet, 1> received;
    ASSERT_EQ(b.receive(0, received), 1);
    EXPECT_EQ(aloe::testing::bytes_of(received[0]), wrong) << "nothing is touched";
    EXPECT_EQ(received[0].rx().l3, aloe::device::ChecksumVerdict::Unknown);
    EXPECT_EQ(received[0].rx().l4, aloe::device::ChecksumVerdict::Unknown);
}

TEST_F(FabricOffloads, IgnoresEthernetPaddingWhenVerifying) {
    // The kernel pads short frames to 60 bytes; the IPv4 total length says where the datagram ends.
    auto padded = aloe::testing::ipv4_frame(spec(aloe::device::Ipv4Protocol::Udp, aloe::testing::Checksums::Correct),
                                            aloe::testing::pattern(2));
    ASSERT_LT(padded.size(), 60);
    padded.resize(60, std::byte{0});
    aloe::fabric::Packet received = exchange(padded, aloe::device::TxMetadata{});
    EXPECT_EQ(received.size(), 60) << "the padding is delivered as it is";
    EXPECT_EQ(received.rx().l3, aloe::device::ChecksumVerdict::Good);
    EXPECT_EQ(received.rx().l4, aloe::device::ChecksumVerdict::Good);
}

TEST_F(FabricOffloads, FillsAndVerifiesAHeaderWithOptions) {
    const auto seeded = aloe::testing::with_ipv4_options(
        aloe::testing::ipv4_frame(spec(aloe::device::Ipv4Protocol::Udp, aloe::testing::Checksums::Seeded),
                                  aloe::testing::pattern(9)),
        1);
    const auto correct = aloe::testing::with_ipv4_options(
        aloe::testing::ipv4_frame(spec(aloe::device::Ipv4Protocol::Udp, aloe::testing::Checksums::Correct),
                                  aloe::testing::pattern(9)),
        1);
    aloe::device::TxMetadata tx   = fill_both;
    tx.l3_length                  = 24;
    aloe::fabric::Packet received = exchange(seeded, tx);
    EXPECT_EQ(aloe::testing::bytes_of(received), correct);
    EXPECT_EQ(received.rx().l3, aloe::device::ChecksumVerdict::Good);
    EXPECT_EQ(received.rx().l4, aloe::device::ChecksumVerdict::Good);
}
