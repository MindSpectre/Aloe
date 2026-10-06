#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/wire>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

    constexpr aloe::wire::MacAddress client{0x02, 0, 0, 0, 0, 0x01};
    constexpr aloe::wire::MacAddress server{0x02, 0, 0, 0, 0, 0x02};
    constexpr aloe::wire::Ipv4Address client_ip{10, 0, 0, 1};
    constexpr aloe::wire::Ipv4Address server_ip{10, 0, 0, 2};

    aloe::frames::Ipv4Spec spec(const aloe::wire::Ipv4Protocol protocol, const aloe::frames::Checksums checksums) {
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
            EXPECT_TRUE(aloe::frames::fill(*packet, frame));
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
    const auto seeded  = aloe::frames::ipv4_frame(spec(aloe::wire::Ipv4Protocol::Udp, aloe::frames::Checksums::Seeded),
                                                  aloe::frames::pattern(9));
    const auto correct = aloe::frames::ipv4_frame(spec(aloe::wire::Ipv4Protocol::Udp, aloe::frames::Checksums::Correct),
                                                  aloe::frames::pattern(9));
    ASSERT_NE(seeded, correct);
    aloe::fabric::Packet received = exchange(seeded, fill_both);
    EXPECT_EQ(aloe::frames::bytes_of(received), correct);
    EXPECT_EQ(received.rx().l3, aloe::device::ChecksumVerdict::Good);
    EXPECT_EQ(received.rx().l4, aloe::device::ChecksumVerdict::Good);
}

TEST_F(FabricOffloads, FillsTcpToo) {
    const auto seeded  = aloe::frames::ipv4_frame(spec(aloe::wire::Ipv4Protocol::Tcp, aloe::frames::Checksums::Seeded),
                                                  aloe::frames::pattern(9));
    const auto correct = aloe::frames::ipv4_frame(spec(aloe::wire::Ipv4Protocol::Tcp, aloe::frames::Checksums::Correct),
                                                  aloe::frames::pattern(9));
    aloe::device::TxMetadata tx   = fill_both;
    tx.fill_l4_checksum           = aloe::device::L4Checksum::Tcp;
    aloe::fabric::Packet received = exchange(seeded, tx);
    EXPECT_EQ(aloe::frames::bytes_of(received), correct);
    EXPECT_EQ(received.rx().l4, aloe::device::ChecksumVerdict::Good);
}

TEST_F(FabricOffloads, FillsOnlyWhatIsAsked) {
    const auto seeded = aloe::frames::ipv4_frame(spec(aloe::wire::Ipv4Protocol::Udp, aloe::frames::Checksums::Seeded),
                                                 aloe::frames::pattern(9));
    aloe::device::TxMetadata tx   = fill_both;
    tx.fill_l4_checksum           = aloe::device::L4Checksum::None;
    aloe::fabric::Packet received = exchange(seeded, tx);
    EXPECT_EQ(received.rx().l3, aloe::device::ChecksumVerdict::Good);
    EXPECT_EQ(received.rx().l4, aloe::device::ChecksumVerdict::Bad) << "the seed alone is not a checksum";
}

TEST_F(FabricOffloads, VerifiesWrongChecksumsAsBad) {
    const auto wrong = aloe::frames::ipv4_frame(spec(aloe::wire::Ipv4Protocol::Udp, aloe::frames::Checksums::Wrong),
                                                aloe::frames::pattern(9));
    aloe::fabric::Packet received = exchange(wrong, aloe::device::TxMetadata{});
    EXPECT_EQ(received.rx().l3, aloe::device::ChecksumVerdict::Bad);
    EXPECT_EQ(received.rx().l4, aloe::device::ChecksumVerdict::Bad);
}

TEST_F(FabricOffloads, LeavesNonIpv4AndUdpWithoutChecksumUnknown) {
    const auto plain =
        aloe::frames::ethernet_frame(server, client, aloe::frames::ethertype_experimental, aloe::frames::pattern(40));
    aloe::fabric::Packet received = exchange(plain, aloe::device::TxMetadata{});
    EXPECT_EQ(received.rx().l3, aloe::device::ChecksumVerdict::Unknown);
    EXPECT_EQ(received.rx().l4, aloe::device::ChecksumVerdict::Unknown);

    auto no_checksum = aloe::frames::ipv4_frame(spec(aloe::wire::Ipv4Protocol::Udp, aloe::frames::Checksums::Correct),
                                                aloe::frames::pattern(9));
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

    const auto wrong = aloe::frames::ipv4_frame(spec(aloe::wire::Ipv4Protocol::Udp, aloe::frames::Checksums::Wrong),
                                                aloe::frames::pattern(9));
    auto packet      = a.allocate(0);
    ASSERT_TRUE(packet.has_value());
    ASSERT_TRUE(aloe::frames::fill(*packet, wrong));
    std::array<aloe::fabric::Packet, 1> burst{std::move(*packet)};
    ASSERT_EQ(a.transmit(0, burst), 1);
    std::array<aloe::fabric::Packet, 1> received;
    ASSERT_EQ(b.receive(0, received), 1);
    EXPECT_EQ(aloe::frames::bytes_of(received[0]), wrong) << "nothing is touched";
    EXPECT_EQ(received[0].rx().l3, aloe::device::ChecksumVerdict::Unknown);
    EXPECT_EQ(received[0].rx().l4, aloe::device::ChecksumVerdict::Unknown);
}

TEST_F(FabricOffloads, IgnoresEthernetPaddingWhenVerifying) {
    // The kernel pads short frames to 60 bytes; the IPv4 total length says where the datagram ends.
    auto padded = aloe::frames::ipv4_frame(spec(aloe::wire::Ipv4Protocol::Udp, aloe::frames::Checksums::Correct),
                                           aloe::frames::pattern(2));
    ASSERT_LT(padded.size(), 60);
    padded.resize(60, std::byte{0});
    aloe::fabric::Packet received = exchange(padded, aloe::device::TxMetadata{});
    EXPECT_EQ(received.size(), 60) << "the padding is delivered as it is";
    EXPECT_EQ(received.rx().l3, aloe::device::ChecksumVerdict::Good);
    EXPECT_EQ(received.rx().l4, aloe::device::ChecksumVerdict::Good);
}

TEST_F(FabricOffloads, FillsAndVerifiesAHeaderWithOptions) {
    const auto seeded = aloe::frames::with_ipv4_options(
        aloe::frames::ipv4_frame(spec(aloe::wire::Ipv4Protocol::Udp, aloe::frames::Checksums::Seeded),
                                 aloe::frames::pattern(9)),
        1);
    const auto correct = aloe::frames::with_ipv4_options(
        aloe::frames::ipv4_frame(spec(aloe::wire::Ipv4Protocol::Udp, aloe::frames::Checksums::Correct),
                                 aloe::frames::pattern(9)),
        1);
    aloe::device::TxMetadata tx   = fill_both;
    tx.l3_length                  = 24;
    aloe::fabric::Packet received = exchange(seeded, tx);
    EXPECT_EQ(aloe::frames::bytes_of(received), correct);
    EXPECT_EQ(received.rx().l3, aloe::device::ChecksumVerdict::Good);
    EXPECT_EQ(received.rx().l4, aloe::device::ChecksumVerdict::Good);
}
