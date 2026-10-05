#include <aloe/net>
#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <frames.hpp>
#include <gtest/gtest.h>
#include <net_fixture.hpp>

namespace {

    using aloe::testing::far_ip;
    using aloe::testing::harness_ip;
    using aloe::testing::harness_mac;
    using aloe::testing::stack_ip;
    using aloe::testing::stack_mac;

    class NetIcmp : public aloe::testing::NetFixture {
    protected:
        /// An echo request from the harness to the stack, identifier 0x1234, sequence 1.
        [[nodiscard]] static aloe::testing::EchoSpec echo() {
            return {
                .destination_mac = stack_mac, .source_mac = harness_mac, .source = harness_ip, .destination = stack_ip};
        }
    };

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             NetIcmp,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

}  // namespace

TEST_P(NetIcmp, AnEchoRequestGetsAReplyWithBothChecksumsRight) {
    const auto payload = aloe::testing::pattern(56);  // what `ping` sends
    inject(aloe::testing::icmp_echo_frame(echo(), payload));

    const auto frames = harness_received();
    ASSERT_EQ(frames.size(), 1);
    const auto reply = aloe::testing::parse_frame(frames[0]);
    EXPECT_EQ(reply.ethernet.destination, harness_mac);
    EXPECT_EQ(reply.ethernet.source, stack_mac);
    EXPECT_EQ(reply.ethernet.ethertype, aloe::testing::ethertype_ipv4);
    ASSERT_TRUE(reply.ipv4.has_value());
    EXPECT_EQ(reply.ipv4->source, stack_ip);
    EXPECT_EQ(reply.ipv4->destination, harness_ip);
    EXPECT_EQ(reply.ipv4->protocol, aloe::wire::Ipv4Protocol::Icmp);
    EXPECT_EQ(reply.ipv4->header_length, 20);
    EXPECT_EQ(reply.ipv4->total_length, 20 + 8 + 56);
    EXPECT_EQ(reply.ipv4->ttl, 64);
    EXPECT_TRUE(reply.ipv4->dont_fragment);
    EXPECT_FALSE(reply.ipv4->is_fragment());
    EXPECT_EQ(reply.ipv4->identification, 0) << "queue 0 times 4096, first datagram";
    EXPECT_EQ(aloe::wire::internet_checksum(reply.ipv4_header), 0)
        << "header checksum right, by " << (offloads() ? "the device" : "software");
    ASSERT_TRUE(reply.icmp.has_value());
    EXPECT_EQ(reply.icmp->type, aloe::wire::IcmpType::EchoReply);
    EXPECT_EQ(reply.icmp->code, 0);
    EXPECT_EQ(reply.icmp->rest, (std::uint32_t{0x1234} << 16U) | 1U);
    EXPECT_EQ(aloe::wire::internet_checksum(reply.l4), 0) << "ICMP checksum right";
    EXPECT_EQ(std::vector<std::byte>(reply.l4.begin() + 8, reply.l4.end()), payload);
    EXPECT_TRUE(burst_empty());
    EXPECT_EQ(ip_.counters().echo_replies_sent, 1);
    EXPECT_EQ(ip_.counters().datagrams_received, 1);
    EXPECT_EQ(ip_.counters().dropped_icmp, 0);
}

TEST_P(NetIcmp, TheIdentificationAdvancesPerReply) {
    inject(aloe::testing::icmp_echo_frame(echo(), aloe::testing::pattern(8)));
    inject(aloe::testing::icmp_echo_frame(echo(), aloe::testing::pattern(8)));
    const auto frames = harness_received();
    ASSERT_EQ(frames.size(), 2);
    EXPECT_EQ(aloe::testing::parse_frame(frames[0]).ipv4->identification, 0);
    EXPECT_EQ(aloe::testing::parse_frame(frames[1]).ipv4->identification, 1);
}

TEST_P(NetIcmp, APingerBehindARouterGetsTheReplyAtTheFramesSourceMac) {
    // The harness plays the router: an off-subnet source address behind the harness's own MAC. The fabric
    // delivers a unicast frame only to the port that owns the MAC, so the reply must come back to the harness.
    aloe::testing::EchoSpec routed = echo();
    routed.source                  = far_ip;
    inject(aloe::testing::icmp_echo_frame(routed, aloe::testing::pattern(8)));
    const auto frames = harness_received();
    ASSERT_EQ(frames.size(), 1);
    const auto reply = aloe::testing::parse_frame(frames[0]);
    EXPECT_EQ(reply.ethernet.destination, harness_mac) << "the last hop is the next hop back";
    ASSERT_TRUE(reply.ipv4.has_value());
    EXPECT_EQ(reply.ipv4->destination, far_ip);
    EXPECT_EQ(ip_.counters().arp_requests_sent, 0) << "the cache is never consulted";
}

TEST_P(NetIcmp, AnEchoRequestWithOptionsGetsAReplyWithATwentyByteHeader) {
    const auto payload = aloe::testing::pattern(16);
    inject(aloe::testing::with_ipv4_options(aloe::testing::icmp_echo_frame(echo(), payload), 3));
    const auto frames = harness_received();
    ASSERT_EQ(frames.size(), 1);
    const auto reply = aloe::testing::parse_frame(frames[0]);
    ASSERT_TRUE(reply.ipv4.has_value());
    EXPECT_EQ(reply.ipv4->header_length, 20) << "options are never sent";
    EXPECT_EQ(reply.ipv4->total_length, 20 + 8 + 16);
    EXPECT_EQ(aloe::wire::internet_checksum(reply.ipv4_header), 0);
    EXPECT_EQ(aloe::wire::internet_checksum(reply.l4), 0);
    EXPECT_EQ(std::vector<std::byte>(reply.l4.begin() + 8, reply.l4.end()), payload) << "the message moved up intact";
}

TEST_P(NetIcmp, AShortEchoPaddedToSixtyBytesGetsAReplyWithTheRightTotalLength) {
    const auto payload = aloe::testing::pattern(2);
    auto frame         = aloe::testing::icmp_echo_frame(echo(), payload);  // 44 bytes
    frame.resize(60);
    inject(frame);
    const auto frames = harness_received();
    ASSERT_EQ(frames.size(), 1);
    const auto reply = aloe::testing::parse_frame(frames[0]);
    ASSERT_TRUE(reply.ipv4.has_value());
    EXPECT_EQ(reply.ipv4->total_length, 20 + 8 + 2) << "the padding is not part of the datagram";
    ASSERT_EQ(reply.l4.size(), 10U);
    EXPECT_EQ(aloe::wire::internet_checksum(reply.l4), 0) << "the checksum covers the message, not the padding";
    EXPECT_EQ(std::vector<std::byte>(reply.l4.begin() + 8, reply.l4.end()), payload);
}

TEST_P(NetIcmp, ABadIcmpChecksumIsDropped) {
    aloe::testing::EchoSpec bad = echo();
    bad.bad_icmp_checksum       = true;
    inject(aloe::testing::icmp_echo_frame(bad, aloe::testing::pattern(8)));
    EXPECT_TRUE(harness_received().empty());
    EXPECT_EQ(ip_.counters().dropped_bad_checksum, 1);
    EXPECT_EQ(ip_.counters().echo_replies_sent, 0);
}

TEST_P(NetIcmp, ABroadcastEchoAndOtherTypesAreDropped) {
    aloe::testing::EchoSpec broadcast = echo();
    broadcast.destination_mac         = aloe::wire::MacAddress::broadcast();
    broadcast.destination             = {10, 0, 0, 255};
    inject(aloe::testing::icmp_echo_frame(broadcast, aloe::testing::pattern(8)));
    EXPECT_EQ(ip_.counters().dropped_icmp, 1) << "as the kernel does by default";

    aloe::testing::EchoSpec reply_in = echo();
    reply_in.type                    = aloe::wire::IcmpType::EchoReply;
    inject(aloe::testing::icmp_echo_frame(reply_in, aloe::testing::pattern(8)));
    EXPECT_EQ(ip_.counters().dropped_icmp, 2);
    EXPECT_TRUE(harness_received().empty());
    EXPECT_EQ(ip_.counters().datagrams_received, 2) << "valid IPv4 for us, just not answered";
}

TEST_P(NetIcmp, AShortIcmpMessageIsDropped) {
    const aloe::testing::Ipv4Spec stub{.destination_mac = stack_mac,
                                       .source_mac      = harness_mac,
                                       .source          = harness_ip,
                                       .destination     = stack_ip,
                                       .protocol        = aloe::wire::Ipv4Protocol::Icmp};
    inject(aloe::testing::ipv4_frame(stub, aloe::testing::pattern(4)));  // four bytes where eight are the minimum
    EXPECT_EQ(ip_.counters().dropped_bad_header, 1);
    EXPECT_TRUE(harness_received().empty());
}
