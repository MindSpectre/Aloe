#include <aloe/net>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <frames.hpp>
#include <gtest/gtest.h>
#include <net_fixture.hpp>

namespace {

    using aloe::device::Ipv4Protocol;
    using aloe::testing::harness_ip;
    using aloe::testing::harness_mac;
    using aloe::testing::stack_ip;
    using aloe::testing::stack_mac;

    class NetReceive : public aloe::testing::NetFixture {
    protected:
        /// A UDP or TCP datagram from the harness to the stack, port 40000 to port 80.
        [[nodiscard]] static aloe::testing::Ipv4Spec
        datagram(const Ipv4Protocol protocol,
                 const aloe::testing::Checksums checksums = aloe::testing::Checksums::Correct) {
            return {.destination_mac  = stack_mac,
                    .source_mac       = harness_mac,
                    .source           = harness_ip,
                    .destination      = stack_ip,
                    .source_port      = 40000,
                    .destination_port = 80,
                    .protocol         = protocol,
                    .checksums        = checksums};
        }
    };

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             NetReceive,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

}  // namespace

TEST_P(NetReceive, AUdpDatagramLandsInTheUdpListWithWhatIpv4Parsed) {
    const auto payload = aloe::testing::pattern(32);
    inject(aloe::testing::ipv4_frame(datagram(Ipv4Protocol::Udp), payload));

    ASSERT_EQ(ip_.received(Ipv4Protocol::Udp).size(), 1);
    EXPECT_TRUE(ip_.received(Ipv4Protocol::Tcp).empty());
    auto& received = ip_.received(Ipv4Protocol::Udp)[0];
    EXPECT_FALSE(received.packet.empty());
    EXPECT_EQ(received.source, harness_ip);
    EXPECT_EQ(received.destination, stack_ip);
    EXPECT_EQ(received.protocol, Ipv4Protocol::Udp);
    EXPECT_EQ(received.l3_offset, 14);
    EXPECT_EQ(received.l3_length, 20);
    EXPECT_EQ(received.l4_length, 8 + 32);
    ASSERT_EQ(received.l4().size(), 40U);
    EXPECT_EQ(aloe::device::load_be16(received.l4().first(2)), 40000);
    EXPECT_EQ(std::vector<std::byte>(received.l4().begin() + 8, received.l4().end()), payload);
    EXPECT_EQ(received.l4_checksum,
              offloads() ? aloe::device::ChecksumVerdict::Good : aloe::device::ChecksumVerdict::Unknown)
        << "the device's verdict, passed through for the transport to judge";
    EXPECT_TRUE(burst_empty());
    EXPECT_EQ(ip_.counters().frames, 1);
    EXPECT_EQ(ip_.counters().datagrams_received, 1);
    EXPECT_EQ(ip_.counters().delivered_udp, 1);
    EXPECT_EQ(ip_.counters().delivered_tcp, 0);
}

TEST_P(NetReceive, ATcpSegmentLandsInTheTcpList) {
    inject(aloe::testing::ipv4_frame(datagram(Ipv4Protocol::Tcp), aloe::testing::pattern(10)));
    ASSERT_EQ(ip_.received(Ipv4Protocol::Tcp).size(), 1);
    EXPECT_TRUE(ip_.received(Ipv4Protocol::Udp).empty());
    EXPECT_EQ(ip_.received(Ipv4Protocol::Tcp)[0].protocol, Ipv4Protocol::Tcp);
    EXPECT_EQ(ip_.received(Ipv4Protocol::Tcp)[0].l4_length, 20 + 10);
    EXPECT_EQ(ip_.counters().delivered_tcp, 1);
}

TEST_P(NetReceive, TheListsClearOnTheNextProcess) {
    inject(aloe::testing::ipv4_frame(datagram(Ipv4Protocol::Udp), aloe::testing::pattern(8, 1)));
    inject(aloe::testing::ipv4_frame(datagram(Ipv4Protocol::Udp), aloe::testing::pattern(8, 2)));
    ASSERT_EQ(ip_.received(Ipv4Protocol::Udp).size(), 1) << "only the second";
    EXPECT_EQ(ip_.received(Ipv4Protocol::Udp)[0].l4()[8], std::byte{2});
    process_pending();  // nothing pending
    EXPECT_TRUE(ip_.received(Ipv4Protocol::Udp).empty());
    EXPECT_EQ(ip_.counters().delivered_udp, 2);
}

TEST_P(NetReceive, ATransportMayKeepThePacket) {
    inject(aloe::testing::ipv4_frame(datagram(Ipv4Protocol::Udp), aloe::testing::pattern(8)));
    ASSERT_EQ(ip_.received(Ipv4Protocol::Udp).size(), 1);
    Packet kept = std::move(ip_.received(Ipv4Protocol::Udp)[0].packet);
    EXPECT_TRUE(ip_.received(Ipv4Protocol::Udp)[0].packet.empty());
    process_pending();  // clears the list; the kept packet is the transport's
    EXPECT_EQ(kept.size(), 14U + 20U + 8U + 8U);
}

TEST_P(NetReceive, EmptySlotsAreSkipped) {
    std::vector<Packet> empties(3);
    ip_.process(empties, now_);
    EXPECT_EQ(ip_.counters().frames, 0);
}

TEST_P(NetReceive, ADatagramsLengthComesFromTheTotalLengthNotTheFrame) {
    auto frame = aloe::testing::ipv4_frame(datagram(Ipv4Protocol::Udp), aloe::testing::pattern(4));  // 46 bytes
    frame.resize(60);  // padded to the minimum frame, as a card does
    inject(frame);
    ASSERT_EQ(ip_.received(Ipv4Protocol::Udp).size(), 1);
    EXPECT_EQ(ip_.received(Ipv4Protocol::Udp)[0].l4_length, 12);
    EXPECT_EQ(ip_.received(Ipv4Protocol::Udp)[0].packet.size(), 60U) << "the padding stays in the frame";
}

TEST_P(NetReceive, AHeaderWithOptionsIsAcceptedAndItsLengthReported) {
    inject(aloe::testing::with_ipv4_options(
        aloe::testing::ipv4_frame(datagram(Ipv4Protocol::Udp), aloe::testing::pattern(8)), 2));
    ASSERT_EQ(ip_.received(Ipv4Protocol::Udp).size(), 1);
    auto& received = ip_.received(Ipv4Protocol::Udp)[0];
    EXPECT_EQ(received.l3_length, 28);
    EXPECT_EQ(received.l4_length, 16);
    EXPECT_EQ(aloe::device::load_be16(received.l4().first(2)), 40000) << "l4() starts after the options";
}

TEST_P(NetReceive, BroadcastDatagramsAreForUs) {
    aloe::testing::Ipv4Spec subnet = datagram(Ipv4Protocol::Udp);
    subnet.destination_mac         = aloe::device::MacAddress::broadcast();
    subnet.destination             = {10, 0, 0, 255};
    inject(aloe::testing::ipv4_frame(subnet, aloe::testing::pattern(8)));
    EXPECT_EQ(ip_.counters().delivered_udp, 1);

    aloe::testing::Ipv4Spec limited = subnet;
    limited.destination             = {255, 255, 255, 255};
    inject(aloe::testing::ipv4_frame(limited, aloe::testing::pattern(8)));
    EXPECT_EQ(ip_.counters().delivered_udp, 2);
    EXPECT_EQ(ip_.counters().dropped_not_for_us, 0);
}

TEST_P(NetReceive, WhatIsNotForUsIsDroppedAndCounted) {
    aloe::testing::Ipv4Spec other_address = datagram(Ipv4Protocol::Udp);
    other_address.destination             = {10, 0, 0, 9};
    inject(aloe::testing::ipv4_frame(other_address, aloe::testing::pattern(8)));
    EXPECT_EQ(ip_.counters().dropped_not_for_us, 1);
    EXPECT_TRUE(ip_.received(Ipv4Protocol::Udp).empty());

    aloe::testing::Ipv4Spec other_mac = datagram(Ipv4Protocol::Udp);
    other_mac.destination_mac         = {0x02, 0, 0, 0, 0, 0x09};
    local(aloe::testing::ipv4_frame(other_mac, aloe::testing::pattern(8)));  // the fabric would not deliver it
    EXPECT_EQ(ip_.counters().dropped_not_for_us, 2);
    EXPECT_TRUE(burst_empty());
}

TEST_P(NetReceive, AShortFrameAndAnUnknownEthertypeAreDropped) {
    local(aloe::testing::pattern(10));  // the fabric refuses it, the brick must not trust the device
    EXPECT_EQ(ip_.counters().dropped_short, 1);
    inject(aloe::testing::ethernet_frame(
        stack_mac, harness_mac, aloe::testing::ethertype_experimental, aloe::testing::pattern(40)));
    EXPECT_EQ(ip_.counters().dropped_ethertype, 1);
    EXPECT_TRUE(burst_empty());
}

TEST_P(NetReceive, ABadHeaderIsDropped) {
    auto frame = aloe::testing::ipv4_frame(datagram(Ipv4Protocol::Udp), aloe::testing::pattern(8));
    frame[14]  = std::byte{0x65};  // version 6
    inject(frame);
    EXPECT_EQ(ip_.counters().dropped_bad_header, 1);
    EXPECT_EQ(ip_.counters().datagrams_received, 0);
}

TEST_P(NetReceive, ABadChecksumIsDroppedOnBothPaths) {
    inject(aloe::testing::ipv4_frame(datagram(Ipv4Protocol::Udp, aloe::testing::Checksums::Wrong),
                                     aloe::testing::pattern(8)));
    EXPECT_EQ(ip_.counters().dropped_bad_checksum, 1) << (offloads() ? "the device's Bad verdict" : "software");
    inject(aloe::testing::ipv4_frame(datagram(Ipv4Protocol::Udp, aloe::testing::Checksums::Zero),
                                     aloe::testing::pattern(8)));
    EXPECT_EQ(ip_.counters().dropped_bad_checksum, 2) << "a zero header checksum is a wrong one";
    EXPECT_TRUE(ip_.received(Ipv4Protocol::Udp).empty());
    EXPECT_EQ(ip_.counters().datagrams_received, 0);
}

TEST_P(NetReceive, AFragmentIsDropped) {
    aloe::testing::Ipv4Spec more = datagram(Ipv4Protocol::Udp);
    more.flags_fragment          = 0x2000;  // more fragments, offset zero
    inject(aloe::testing::ipv4_frame(more, aloe::testing::pattern(8)));
    aloe::testing::Ipv4Spec offset = datagram(Ipv4Protocol::Udp);
    offset.flags_fragment          = 0x0001;  // the second piece
    inject(aloe::testing::ipv4_frame(offset, aloe::testing::pattern(8)));
    EXPECT_EQ(ip_.counters().dropped_fragment, 2);
    EXPECT_TRUE(ip_.received(Ipv4Protocol::Udp).empty());
}

TEST_P(NetReceive, AnUnknownProtocolIsDropped) {
    inject(aloe::testing::ipv4_frame(datagram(Ipv4Protocol{47}), aloe::testing::pattern(8)));  // GRE
    EXPECT_EQ(ip_.counters().dropped_protocol, 1);
    EXPECT_EQ(ip_.counters().datagrams_received, 1) << "valid IPv4 for us, just nobody's";
}

TEST_P(NetReceive, TheBrickReportsItsConfigurationAndTheDevicesMtu) {
    EXPECT_EQ(ip_.address(), stack_ip);
    EXPECT_EQ(ip_.prefix(), 24);
    EXPECT_EQ(ip_.gateway(), aloe::testing::gateway_ip);
    EXPECT_EQ(ip_.mtu(), 1500);
    EXPECT_EQ(ip_.max_l4_size(), 1480);
}

TEST_P(NetReceive, ABadConfigThrows) {
    using namespace std::chrono_literals;
    EXPECT_THROW((Ipv4{queue_, {.address = {}}}), std::invalid_argument) << "zero address";
    EXPECT_THROW((Ipv4{
                     queue_, {.address = stack_ip, .prefix = 33}
    }),
                 std::invalid_argument);
    EXPECT_THROW((Ipv4{
                     queue_, {.address = stack_ip, .prefix = 24, .gateway = aloe::testing::far_ip}
    }),
                 std::invalid_argument)
        << "the gateway must be on the subnet";
    EXPECT_THROW((Ipv4{
                     queue_, {.address = stack_ip, .ttl = 0}
    }),
                 std::invalid_argument);
    EXPECT_THROW((Ipv4{
                     queue_, {.address = stack_ip, .burst_capacity = 0}
    }),
                 std::invalid_argument);
    EXPECT_THROW((Ipv4{
                     queue_, {.address = stack_ip, .arp_capacity = 100}
    }),
                 std::invalid_argument)
        << "ArpCache's check reaches the caller";
    EXPECT_THROW((Ipv4{
                     queue_, {.address = stack_ip, .arp_reachable = 2min, .arp_expire = 1min}
    }),
                 std::invalid_argument);
    EXPECT_NO_THROW((Ipv4{
        queue_, {.address = stack_ip, .prefix = 32}
    }));
}
