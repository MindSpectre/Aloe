#include <aloe/frames>
#include <aloe/net>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#include <net_fixture.hpp>

namespace {

    using namespace std::chrono_literals;
    using aloe::testing::gateway_ip;
    using aloe::testing::gateway_mac;
    using aloe::testing::harness_ip;
    using aloe::testing::harness_mac;
    using aloe::testing::stack_ip;
    using aloe::testing::stack_mac;

    constexpr aloe::wire::Ipv4Address stranger{10, 0, 0, 77};
    constexpr aloe::wire::MacAddress stranger_mac{0x02, 0, 0, 0, 0, 0x77};
    const aloe::wire::MacAddress broadcast = aloe::wire::MacAddress::broadcast();

    class NetArp : public aloe::testing::NetFixture {};

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             NetArp,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

}  // namespace

TEST_P(NetArp, ARequestForOurAddressGetsAReplyAndTeachesTheSender) {
    inject(aloe::frames::arp_frame(aloe::frames::arp_request(harness_mac, harness_ip, stack_ip), broadcast));

    const auto frames = harness_received();
    ASSERT_EQ(frames.size(), 1);
    const auto reply = aloe::frames::parse_frame(frames[0]).value();
    EXPECT_EQ(reply.ethernet.destination, harness_mac);
    EXPECT_EQ(reply.ethernet.source, stack_mac);
    EXPECT_EQ(reply.ethernet.ethertype, aloe::frames::ethertype_arp);
    ASSERT_TRUE(reply.arp.has_value());
    EXPECT_EQ(*reply.arp, aloe::frames::arp_reply(stack_mac, stack_ip, harness_mac, harness_ip));
    EXPECT_TRUE(burst_empty());

    ASSERT_EQ(ip_.resolved().size(), 1);
    EXPECT_EQ(ip_.resolved()[0], (aloe::net::ArpResolution{.address = harness_ip, .mac = harness_mac}));
    EXPECT_EQ(ip_.resolve(harness_ip, now_), harness_mac);
    EXPECT_TRUE(harness_received().empty()) << "no request for what is known";
    EXPECT_EQ(ip_.counters().arp_requests_received, 1);
    EXPECT_EQ(ip_.counters().arp_replies_sent, 1);
    EXPECT_EQ(ip_.counters().resolutions, 1);
}

TEST_P(NetArp, ARequestForAnotherHostTeachesNothing) {
    inject(aloe::frames::arp_frame(aloe::frames::arp_request(harness_mac, harness_ip, {10, 0, 0, 9}), broadcast));
    EXPECT_TRUE(harness_received().empty());
    EXPECT_TRUE(ip_.resolved().empty());
    EXPECT_EQ(ip_.counters().arp_requests_received, 1);
    EXPECT_EQ(ip_.counters().resolutions, 0);
    EXPECT_FALSE(ip_.resolve(harness_ip, now_).has_value()) << "the sender was not learned";
}

TEST_P(NetArp, AProbeWithAZeroSenderIsAnsweredAndTeachesNothing) {
    inject(aloe::frames::arp_frame(aloe::frames::arp_request(harness_mac, aloe::wire::Ipv4Address{}, stack_ip),
                                   broadcast));

    const auto frames = harness_received();
    ASSERT_EQ(frames.size(), 1);
    const auto reply = aloe::frames::parse_frame(frames[0]).value();
    ASSERT_TRUE(reply.arp.has_value());
    EXPECT_EQ(reply.arp->operation, aloe::wire::ArpOperation::Reply);
    EXPECT_EQ(reply.arp->target_ip, aloe::wire::Ipv4Address{});
    EXPECT_TRUE(ip_.resolved().empty());
    EXPECT_EQ(ip_.counters().resolutions, 0);
    EXPECT_EQ(ip_.counters().arp_replies_sent, 1);
}

TEST_P(NetArp, ARequestFromAGroupMacIsNeitherAnsweredNorLearned) {
    constexpr aloe::wire::MacAddress group{0x01, 0x00, 0x5e, 0x00, 0x00, 0x01};
    local(aloe::frames::arp_frame(aloe::frames::arp_request(group, harness_ip, stack_ip), broadcast));
    local(aloe::frames::arp_frame(aloe::frames::arp_request(group, aloe::wire::Ipv4Address{}, stack_ip), broadcast));

    EXPECT_TRUE(harness_received().empty()) << "a reply would go to a group";
    EXPECT_TRUE(ip_.resolved().empty());
    EXPECT_EQ(ip_.counters().dropped_martian, 2) << "probe or not, no host sends from a group MAC";
    EXPECT_EQ(ip_.counters().arp_replies_sent, 0);
    EXPECT_TRUE(burst_empty());
}

TEST_P(NetArp, ARequestForAnotherHostRefreshesAnEntryWeHold) {
    ip_.learn(harness_ip, stranger_mac, now_);  // a stale MAC
    inject(aloe::frames::arp_frame(aloe::frames::arp_request(harness_mac, harness_ip, {10, 0, 0, 9}), broadcast));

    EXPECT_TRUE(harness_received().empty());
    ASSERT_EQ(ip_.resolved().size(), 1);
    EXPECT_EQ(ip_.resolved()[0], (aloe::net::ArpResolution{.address = harness_ip, .mac = harness_mac}));
    EXPECT_EQ(ip_.resolve(harness_ip, now_), harness_mac);
    EXPECT_TRUE(harness_received().empty()) << "no request for what is known";
}

TEST_P(NetArp, ResolveSendsOneBroadcastRequestPerInterval) {
    EXPECT_FALSE(ip_.resolve(harness_ip, now_).has_value());
    auto frames = harness_received();
    ASSERT_EQ(frames.size(), 1);
    const auto request = aloe::frames::parse_frame(frames[0]).value();
    EXPECT_EQ(request.ethernet.destination, broadcast);
    EXPECT_EQ(request.ethernet.source, stack_mac);
    ASSERT_TRUE(request.arp.has_value());
    EXPECT_EQ(*request.arp, aloe::frames::arp_request(stack_mac, stack_ip, harness_ip));

    now_ += 500ms;
    EXPECT_FALSE(ip_.resolve(harness_ip, now_).has_value());
    EXPECT_TRUE(harness_received().empty()) << "within the interval";
    now_ += 500ms;
    EXPECT_FALSE(ip_.resolve(harness_ip, now_).has_value());
    EXPECT_EQ(harness_received().size(), 1);
    EXPECT_EQ(ip_.counters().arp_requests_sent, 2);
}

TEST_P(NetArp, AReplyToOurRequestResolvesAndIsReported) {
    std::ignore = ip_.resolve(harness_ip, now_);
    std::ignore = harness_received();
    inject(aloe::frames::arp_frame(aloe::frames::arp_reply(harness_mac, harness_ip, stack_mac, stack_ip), stack_mac));
    ASSERT_EQ(ip_.resolved().size(), 1);
    EXPECT_EQ(ip_.resolved()[0], (aloe::net::ArpResolution{.address = harness_ip, .mac = harness_mac}));
    EXPECT_EQ(ip_.counters().arp_replies_received, 1);
    EXPECT_EQ(ip_.resolve(harness_ip, now_), harness_mac);
    EXPECT_TRUE(harness_received().empty());
    EXPECT_TRUE(burst_empty());
}

TEST_P(NetArp, AnUnsolicitedReplyIsDroppedAndTeachesNothing) {
    inject(aloe::frames::arp_frame(aloe::frames::arp_reply(stranger_mac, stranger, stack_mac, stack_ip), stack_mac));
    EXPECT_EQ(ip_.counters().dropped_arp_unsolicited, 1);
    EXPECT_EQ(ip_.counters().arp_replies_received, 1);
    EXPECT_TRUE(ip_.resolved().empty());
    EXPECT_FALSE(ip_.resolve(stranger, now_).has_value()) << "nothing was learned";
    EXPECT_EQ(harness_received().size(), 1) << "so a request goes out";
}

TEST_P(NetArp, WithAcceptUnsolicitedRepliesAReplyAddressedToUsIsLearned) {
    // A sibling shard asked; its reply landed on this queue. The shared address makes it ours to learn and forward.
    aloe::net::Ipv4Config config      = aloe::testing::stack_config();
    config.accept_unsolicited_replies = true;
    Ipv4 accepting{queue_, config};
    const auto process_one = [&](const std::vector<std::byte>& frame) {
        auto packet = harness_.allocate(0);
        ASSERT_TRUE(packet.has_value());
        ASSERT_TRUE(aloe::frames::fill(*packet, frame));
        std::array<Packet, 1> out{std::move(*packet)};
        ASSERT_EQ(harness_.transmit(0, out), 1);
        const std::size_t count = port_.receive(0, burst_);
        accepting.process(std::span<Packet>{burst_}.first(count), now_);
    };

    process_one(
        aloe::frames::arp_frame(aloe::frames::arp_reply(stranger_mac, stranger, stack_mac, stack_ip), stack_mac));
    EXPECT_EQ(accepting.counters().dropped_arp_unsolicited, 0);
    EXPECT_EQ(accepting.counters().arp_replies_received, 1);
    EXPECT_EQ(accepting.counters().resolutions, 1);
    ASSERT_EQ(accepting.resolved().size(), 1);
    EXPECT_EQ(accepting.resolved()[0].address, stranger);
    EXPECT_EQ(accepting.resolved()[0].mac, stranger_mac);
    EXPECT_EQ(accepting.resolve(stranger, now_), stranger_mac);
    EXPECT_TRUE(harness_received().empty()) << "learned, so no request goes out";

    // A reply addressed to another host is still not ours to learn.
    process_one(aloe::frames::arp_frame(
        aloe::frames::arp_reply(harness_mac, harness_ip, stack_mac, aloe::wire::Ipv4Address{10, 0, 0, 99}), stack_mac));
    EXPECT_EQ(accepting.counters().dropped_arp_unsolicited, 1);
    EXPECT_TRUE(accepting.resolved().empty());
    EXPECT_EQ(accepting.counters().resolutions, 1);
}

TEST_P(NetArp, AFrameClaimingOurOwnAddressIsAConflict) {
    inject(aloe::frames::arp_frame(aloe::frames::arp_request(harness_mac, stack_ip, stack_ip), broadcast));
    inject(aloe::frames::arp_frame(aloe::frames::arp_reply(harness_mac, stack_ip, stack_mac, stack_ip), stack_mac));
    EXPECT_EQ(ip_.counters().dropped_arp_conflict, 2);
    EXPECT_TRUE(harness_received().empty());
    EXPECT_TRUE(ip_.resolved().empty());
    EXPECT_EQ(ip_.counters().resolutions, 0);
}

TEST_P(NetArp, AMalformedArpFrameIsDropped) {
    auto frame = aloe::frames::arp_frame(aloe::frames::arp_request(harness_mac, harness_ip, stack_ip), broadcast);
    frame[15]  = std::byte{6};  // hardware type: token ring
    inject(frame);
    EXPECT_EQ(ip_.counters().dropped_arp_malformed, 1);
    EXPECT_TRUE(harness_received().empty());
}

TEST_P(NetArp, LearnSeedsTheCacheAndReportsNothing) {
    ip_.learn(gateway_ip, gateway_mac, now_);
    EXPECT_TRUE(ip_.resolved().empty());
    EXPECT_EQ(ip_.counters().resolutions, 0);
    EXPECT_EQ(ip_.resolve(gateway_ip, now_), gateway_mac);
    EXPECT_TRUE(harness_received().empty());
}

TEST_P(NetArp, AStaleEntryRefreshesByUnicastWhileStillInUse) {
    // The harness plays the peer: the fabric delivers a unicast frame only to the port that owns the MAC.
    ip_.learn(harness_ip, harness_mac, now_);
    now_ += 61s;
    EXPECT_EQ(ip_.resolve(harness_ip, now_), harness_mac) << "stale, still usable";
    auto frames = harness_received();
    ASSERT_EQ(frames.size(), 1);
    const auto refresh = aloe::frames::parse_frame(frames[0]).value();
    EXPECT_EQ(refresh.ethernet.destination, harness_mac) << "unicast to the MAC we have";
    ASSERT_TRUE(refresh.arp.has_value());
    EXPECT_EQ(*refresh.arp, aloe::frames::arp_request(stack_mac, stack_ip, harness_ip));

    now_ += 500ms;
    EXPECT_EQ(ip_.resolve(harness_ip, now_), harness_mac);
    EXPECT_TRUE(harness_received().empty()) << "one refresh per interval";

    now_ += 60s;  // 121.5 s after the confirmation: expired
    EXPECT_FALSE(ip_.resolve(harness_ip, now_).has_value());
    frames = harness_received();
    ASSERT_EQ(frames.size(), 1);
    EXPECT_EQ(aloe::frames::parse_frame(frames[0]).value().ethernet.destination, broadcast) << "back to broadcast";
}

TEST_P(NetArp, ARefreshLearnedFromTheWireIsReportedAgain) {
    ip_.learn(harness_ip, harness_mac, now_);  // seeded: no event
    inject(aloe::frames::arp_frame(aloe::frames::arp_request(harness_mac, harness_ip, stack_ip), broadcast));
    EXPECT_EQ(ip_.resolved().size(), 1) << "every mapping from the wire is reported, so other shards stay fresh";
    std::ignore = harness_received();
}
