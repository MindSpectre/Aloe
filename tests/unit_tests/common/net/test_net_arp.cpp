#include <aloe/core>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/net>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <net_fixture.hpp>

namespace {

    using namespace std::chrono_literals;
    using aloe::net::SendError;
    using aloe::testing::gateway_ip;
    using aloe::testing::gateway_mac;
    using aloe::testing::harness_ip;
    using aloe::testing::harness_mac;
    using aloe::testing::stack_ip;
    using aloe::testing::stack_mac;
    using Brick  = aloe::net::Ipv4<aloe::fabric::Port>;
    using Taught = aloe::net::ArpCache::Taught;

    constexpr aloe::wire::Ipv4Address stranger{10, 0, 0, 77};
    constexpr aloe::wire::MacAddress stranger_mac{0x02, 0, 0, 0, 0, 0x77};
    const aloe::wire::MacAddress broadcast = aloe::wire::MacAddress::broadcast();

    /// Eight bytes of UDP to `destination` through `brick` at `now`.
    [[nodiscard]] std::expected<void, SendError>
    send_to(Brick& brick, const aloe::wire::Ipv4Address destination, const aloe::core::TimePoint now) {
        auto packet = brick.allocate();
        if (!packet || !packet->append(8)) {
            ADD_FAILURE() << "no packet to send";
            return std::unexpected{SendError::Refused};
        }
        return brick.send(std::move(*packet), {.destination = destination}, now);
    }

    /// The fixture's configuration with the switch a multi-queue loop sets.
    [[nodiscard]] aloe::net::Ipv4Config reporting_config() {
        aloe::net::Ipv4Config config      = aloe::testing::stack_config();
        config.report_unsolicited_replies = true;
        return config;
    }

    class NetArp : public aloe::testing::NetFixture {};

    /// The brick of a shard whose queue receives the replies to requests its siblings sent.
    class NetArpReporting : public aloe::testing::NetFixture {
    protected:
        NetArpReporting()
            : NetFixture{reporting_config()} {
        }
    };

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             NetArp,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             NetArpReporting,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

}  // namespace

TEST_P(NetArp, ARequestForOurAddressFromAnUnknownSenderIsAnsweredAndTeachesNothing) {
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
    EXPECT_TRUE(ip_.resolved().empty());
    EXPECT_EQ(ip_.counters().arp_requests_received, 1);
    EXPECT_EQ(ip_.counters().arp_replies_sent, 1);
    EXPECT_EQ(ip_.counters().resolutions, 0);

    EXPECT_FALSE(ip_.resolve(harness_ip, now_).has_value()) << "the sender was not stored";
    const auto asked = harness_received();
    ASSERT_EQ(asked.size(), 1) << "so the next lookup asks for it";
    const auto request = aloe::frames::parse_frame(asked[0]).value();
    EXPECT_EQ(request.ethernet.destination, broadcast);
    ASSERT_TRUE(request.arp.has_value());
    EXPECT_EQ(*request.arp, aloe::frames::arp_request(stack_mac, stack_ip, harness_ip));
}

TEST_P(NetArp, ARequestFromASenderWeHoldUpdatesIt) {
    EXPECT_FALSE(ip_.resolve(harness_ip, now_).has_value());  // the brick's own lookup makes the entry
    ASSERT_EQ(harness_received().size(), 1) << "its request";
    inject(aloe::frames::arp_frame(aloe::frames::arp_request(harness_mac, harness_ip, stack_ip), broadcast));
    ASSERT_EQ(harness_received().size(), 1) << "the answer to the harness's request";

    ASSERT_EQ(ip_.resolved().size(), 1);
    EXPECT_EQ(ip_.resolved()[0], (aloe::net::ArpResolution{.address = harness_ip, .mac = harness_mac}));
    EXPECT_EQ(ip_.counters().resolutions, 1);
    EXPECT_EQ(ip_.resolve(harness_ip, now_), harness_mac);
    EXPECT_TRUE(harness_received().empty()) << "no request for what is known";
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

// Derived again by the design's rule 1, as amended: "It extends the entry's life only when it answers a request the
// brick itself has outstanding for that address: a frame nobody asked for never moves the confirmation stamp". The
// frame is reported and gives the entry its MAC; the entry still expires 120 s after the seed.
TEST_P(NetArp, ARequestForAnotherHostChangesTheMacOfAnEntryWeHoldAndNotItsLife) {
    const TimePoint seeded = now_;
    ASSERT_EQ(ip_.learn(harness_ip, stranger_mac, seeded), Taught::Created);  // a stale MAC
    now_ += 100s;
    inject(aloe::frames::arp_frame(aloe::frames::arp_request(harness_mac, harness_ip, {10, 0, 0, 9}), broadcast));

    EXPECT_TRUE(harness_received().empty());
    ASSERT_EQ(ip_.resolved().size(), 1);
    EXPECT_EQ(ip_.resolved()[0], (aloe::net::ArpResolution{.address = harness_ip, .mac = harness_mac}));
    EXPECT_EQ(ip_.resolve(harness_ip, now_), harness_mac);
    auto frames = harness_received();
    ASSERT_EQ(frames.size(), 1) << "stale since the seed, so the lookup asks for a refresh";
    EXPECT_EQ(aloe::frames::parse_frame(frames[0]).value().ethernet.destination, harness_mac)
        << "unicast, to the MAC the frame gave it";

    now_ = seeded + 120s;
    EXPECT_FALSE(ip_.resolve(harness_ip, now_).has_value()) << "expired: the frame did not extend its life";
    frames = harness_received();
    ASSERT_EQ(frames.size(), 1);
    EXPECT_EQ(aloe::frames::parse_frame(frames[0]).value().ethernet.destination, broadcast);
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

TEST_P(NetArp, AReplyForAnAddressWeDidNotAskAboutIsDropped) {
    inject(aloe::frames::arp_frame(aloe::frames::arp_reply(stranger_mac, stranger, stack_mac, stack_ip), stack_mac));
    EXPECT_EQ(ip_.counters().dropped_arp_unsolicited, 1);
    EXPECT_EQ(ip_.counters().arp_replies_received, 1);
    EXPECT_TRUE(ip_.resolved().empty());
    EXPECT_FALSE(ip_.resolve(stranger, now_).has_value()) << "nothing was learned";
    EXPECT_EQ(harness_received().size(), 1) << "so a request goes out";
}

// Replaces the test that the switch's old name stored the reply. The design's "Several queues": with the switch on,
// `resolved()` "also reports a reply addressed to us about an address this shard does not hold, which is not stored".
TEST_P(NetArpReporting, AReplyAboutAnAddressNotHeldIsReportedAndNotStored) {
    // A sibling shard asked; its reply landed on this queue, which reports it for the loop to forward.
    inject(aloe::frames::arp_frame(aloe::frames::arp_reply(stranger_mac, stranger, stack_mac, stack_ip), stack_mac));
    ASSERT_EQ(ip_.resolved().size(), 1U);
    EXPECT_EQ(ip_.resolved()[0], (aloe::net::ArpResolution{.address = stranger, .mac = stranger_mac}));
    EXPECT_EQ(ip_.counters().arp_replies_received, 1U);
    EXPECT_EQ(ip_.counters().resolutions, 1U) << "one mapping reported";
    EXPECT_EQ(ip_.counters().dropped_arp_unsolicited, 0U) << "counted there only with the switch off";
    EXPECT_EQ(ip_.generation(), 0U) << "no MAC was stored";

    const auto sent = send_to(ip_, stranger, now_);
    ASSERT_FALSE(sent.has_value());
    EXPECT_EQ(sent.error(), SendError::Unresolved) << "not stored, so a send to it still asks";
    const auto frames = harness_received();
    ASSERT_EQ(frames.size(), 1U);
    const auto request = aloe::frames::parse_frame(frames[0]).value();
    EXPECT_EQ(request.ethernet.destination, broadcast);
    ASSERT_TRUE(request.arp.has_value());
    EXPECT_EQ(*request.arp, aloe::frames::arp_request(stack_mac, stack_ip, stranger));
}

TEST_P(NetArpReporting, AReplyNotAddressedToUsIsNotReported) {
    inject(aloe::frames::arp_frame(
        aloe::frames::arp_reply(harness_mac, harness_ip, stack_mac, aloe::wire::Ipv4Address{10, 0, 0, 99}), stack_mac));
    EXPECT_TRUE(ip_.resolved().empty());
    EXPECT_EQ(ip_.counters().resolutions, 0U);
    EXPECT_EQ(ip_.counters().dropped_arp_unsolicited, 1U) << "no sibling asked for another host's address";
}

TEST_P(NetArp, AppliedResolutionFillsAnEntryThatWasAskedFor) {
    EXPECT_FALSE(ip_.resolve(harness_ip, now_).has_value());
    ASSERT_EQ(harness_received().size(), 1U) << "its request";
    ip_.apply_resolution(harness_ip, harness_mac, now_);
    EXPECT_TRUE(ip_.resolved().empty()) << "a forwarded resolution is not reported again, so shards never echo";
    EXPECT_EQ(ip_.counters().resolutions, 0U);

    ASSERT_TRUE(send_to(ip_, harness_ip, now_).has_value());
    const auto frames = harness_received();
    ASSERT_EQ(frames.size(), 1U) << "the datagram, and no request before it";
    EXPECT_EQ(aloe::frames::parse_frame(frames[0]).value().ethernet.destination, harness_mac);
}

TEST_P(NetArp, AppliedResolutionDoesNothingOnABrickThatDidNotAsk) {
    ip_.apply_resolution(harness_ip, harness_mac, now_);
    EXPECT_EQ(ip_.generation(), 0U) << "no MAC was stored";

    const auto sent = send_to(ip_, harness_ip, now_);
    ASSERT_FALSE(sent.has_value());
    EXPECT_EQ(sent.error(), SendError::Unresolved);
    const auto frames = harness_received();
    ASSERT_EQ(frames.size(), 1U) << "so a send asks for it";
    const auto request = aloe::frames::parse_frame(frames[0]).value();
    EXPECT_EQ(request.ethernet.destination, broadcast);
    ASSERT_TRUE(request.arp.has_value());
    EXPECT_EQ(*request.arp, aloe::frames::arp_request(stack_mac, stack_ip, harness_ip));
}

TEST_P(NetArp, AppliedResolutionCannotChangeAStaticEntry) {
    aloe::net::Ipv4Config config = aloe::testing::stack_config();
    config.static_arp            = {
        {.address = harness_ip, .mac = harness_mac}
    };
    Brick brick{queue_, config};  // over the same queue: the fixture's brick stays idle meanwhile
    brick.apply_resolution(harness_ip, stranger_mac, now_);

    ASSERT_TRUE(send_to(brick, harness_ip, now_).has_value());
    const auto frames = harness_received();
    ASSERT_EQ(frames.size(), 1U) << "to the configured MAC: a frame to the other would land nowhere";
    EXPECT_EQ(aloe::frames::parse_frame(frames[0]).value().ethernet.destination, harness_mac);
    EXPECT_EQ(brick.counters().arp_requests_sent, 0U);
    EXPECT_EQ(brick.generation(), 0U);
}

// A forwarded resolution takes the design's rule 1 for a frame: "It extends the entry's life only when it answers a
// request the brick itself has outstanding for that address".
TEST_P(NetArp, AnAppliedResolutionDoesNotExtendTheLifeOfAFreshEntry) {
    const TimePoint resolved_at = now_;
    EXPECT_FALSE(ip_.resolve(harness_ip, now_).has_value());
    ASSERT_EQ(harness_received().size(), 1U) << "its request";
    inject(aloe::frames::arp_frame(aloe::frames::arp_reply(harness_mac, harness_ip, stack_mac, stack_ip), stack_mac));
    ASSERT_EQ(ip_.resolve(harness_ip, now_), harness_mac) << "resolved by the answer to its own request";

    ip_.apply_resolution(harness_ip, harness_mac, resolved_at + 100s);  // no request of its own outstanding

    now_            = resolved_at + 120s;
    const auto sent = send_to(ip_, harness_ip, now_);
    ASSERT_FALSE(sent.has_value());
    EXPECT_EQ(sent.error(), SendError::Unresolved) << "expired 120 s after the answer to its own request";
    const auto frames = harness_received();
    ASSERT_EQ(frames.size(), 1U) << "so the send asks again";
    EXPECT_EQ(aloe::frames::parse_frame(frames[0]).value().ethernet.destination, broadcast);
}

// The design's rule 1: a frame "extends the entry's life only when it answers a request the brick itself has
// outstanding for that address", and the refresh a stale entry's send asks for is such a request.
TEST_P(NetArp, AnAnsweredRefreshKeepsTheEntryPastItsFirstExpiry) {
    const TimePoint resolved_at = now_;
    EXPECT_FALSE(ip_.resolve(harness_ip, now_).has_value());
    ASSERT_EQ(harness_received().size(), 1U) << "its request";
    inject(aloe::frames::arp_frame(aloe::frames::arp_reply(harness_mac, harness_ip, stack_mac, stack_ip), stack_mac));

    now_ = resolved_at + 61s;  // past arp_reachable
    ASSERT_TRUE(send_to(ip_, harness_ip, now_).has_value()) << "stale, still usable";
    auto frames = harness_received();
    ASSERT_EQ(frames.size(), 2U) << "the refresh, then the datagram";
    const auto refresh = aloe::frames::parse_frame(frames[0]).value();
    EXPECT_EQ(refresh.ethernet.destination, harness_mac) << "unicast to the MAC it has";
    ASSERT_TRUE(refresh.arp.has_value());
    EXPECT_EQ(*refresh.arp, aloe::frames::arp_request(stack_mac, stack_ip, harness_ip));
    inject(aloe::frames::arp_frame(aloe::frames::arp_reply(harness_mac, harness_ip, stack_mac, stack_ip), stack_mac));
    ASSERT_EQ(ip_.resolved().size(), 1U) << "the refresh's answer";

    now_ = resolved_at + 120s + 500ms;  // past the moment the first confirmation expires
    ASSERT_TRUE(send_to(ip_, harness_ip, now_).has_value()) << "confirmed again at 61 s";
    frames = harness_received();
    ASSERT_EQ(frames.size(), 1U) << "the datagram alone: fresh until 121 s";
    EXPECT_EQ(aloe::frames::parse_frame(frames[0]).value().ethernet.destination, harness_mac);
}

TEST_P(NetArp, ALookupAtAFullTableIsCountedAndTheSendIsUnresolved) {
    aloe::net::Ipv4Config config = aloe::testing::stack_config();
    config.arp_capacity          = 8;
    Ipv4 small{queue_, config};  // over the same queue: the fixture's brick stays idle meanwhile
    for (std::uint8_t index = 0; index < 8; ++index) {
        const aloe::wire::Ipv4Address neighbour{10, 0, 0, static_cast<std::uint8_t>(10 + index)};
        ASSERT_EQ(small.learn(neighbour, stranger_mac, now_), Taught::Created);
        ASSERT_EQ(small.resolve(neighbour, now_), stranger_mac);
    }
    ASSERT_EQ(small.counters().arp_table_full, 0);

    auto packet = small.allocate();
    ASSERT_TRUE(packet.has_value());
    ASSERT_TRUE(packet->append(8).has_value());
    const auto sent = small.send(std::move(*packet), {.destination = stranger}, now_);
    ASSERT_FALSE(sent.has_value());
    EXPECT_EQ(sent.error(), aloe::net::SendError::Unresolved) << "as for any address not resolved yet";
    EXPECT_EQ(small.counters().arp_table_full, 1);
    EXPECT_EQ(small.counters().send_unresolved, 1);
    EXPECT_EQ(small.counters().arp_requests_sent, 0) << "no slot to wait in, so no request";
    EXPECT_TRUE(harness_received().empty());
    for (std::uint8_t index = 0; index < 8; ++index) {
        EXPECT_EQ(small.resolve({10, 0, 0, static_cast<std::uint8_t>(10 + index)}, now_), stranger_mac);
    }
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
    EXPECT_EQ(ip_.learn(gateway_ip, gateway_mac, now_), Taught::Created);
    EXPECT_TRUE(ip_.resolved().empty());
    EXPECT_EQ(ip_.counters().resolutions, 0);
    EXPECT_EQ(ip_.resolve(gateway_ip, now_), gateway_mac);
    EXPECT_TRUE(harness_received().empty());
}

TEST_P(NetArp, AStaleEntryRefreshesByUnicastWhileStillInUse) {
    // The harness plays the peer: the fabric delivers a unicast frame only to the port that owns the MAC.
    ASSERT_EQ(ip_.learn(harness_ip, harness_mac, now_), Taught::Created);
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

// Renamed by the design's rule 1, as amended: a frame nobody asked for "never moves the confirmation stamp", so this
// one refreshes nothing; it is reported all the same, since a sibling shard may be the one that asked.
TEST_P(NetArp, AFrameAboutAnEntryWeHoldIsReportedEvenWhenItChangesNothing) {
    ASSERT_EQ(ip_.learn(harness_ip, harness_mac, now_), Taught::Created);  // seeded: no event
    inject(aloe::frames::arp_frame(aloe::frames::arp_request(harness_mac, harness_ip, stack_ip), broadcast));
    EXPECT_EQ(ip_.resolved().size(), 1) << "every mapping from the wire is reported, so other shards stay fresh";
    std::ignore = harness_received();
}
