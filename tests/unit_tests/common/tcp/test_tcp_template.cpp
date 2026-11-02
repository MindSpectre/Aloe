#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>

namespace {

    using namespace std::chrono_literals;
    using TcpTemplate = aloe::testing::TcpFixture;

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpTemplate,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

}  // namespace

// The fabric delivers unicast by destination MAC, so a frame to a MAC no port owns lands nowhere and `collect` sees
// nothing: the test reads the MAC through the entry's packet instead, which the loan allows.
TEST_P(TcpTemplate, TheFrameFollowsALearnedMacChange) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ip_.learn(aloe::testing::harness_ip, aloe::testing::gateway_mac, now_);
    ASSERT_EQ(c->send(aloe::frames::pattern(10)), 10U);
    const auto& node    = tcp_->sent_segment(*c, 0);
    const auto ethernet = aloe::wire::EthernetHeader::parse(node.packet.data());  // reading a lent packet is allowed
    ASSERT_TRUE(ethernet.has_value());
    EXPECT_EQ(ethernet->destination, aloe::testing::gateway_mac);
    EXPECT_EQ(collect().size(), 0U) << "nobody on the fabric has that MAC";
}

TEST_P(TcpTemplate, AStaleEntryIsRefreshedOnceAndTheFrameStillGoes) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const auto requests = ip_.counters().arp_requests_sent;
    advance(61s);  // past arp_reachable; nothing is armed on an idle connection in this task
    ASSERT_EQ(c->send(aloe::frames::pattern(10)), 10U);
    EXPECT_EQ(ip_.counters().arp_requests_sent, requests + 1) << "the rebuild asked for a refresh";
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 2U) << "the request and the segment";
}

TEST_P(TcpTemplate, ARebuildThatFindsNoNextHopRefusesTheCommitAndRecovers) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    advance(121s);  // past arp_expire: the entry is gone
    const auto room = c->prepare(10);
    ASSERT_TRUE(room.has_value());
    EXPECT_FALSE(c->commit(10));
    EXPECT_EQ(tcp_->counters().send_unresolved, 1U);
    EXPECT_EQ(c->queued_segments(), 0U);
    tcp_->process({}, now_);
    EXPECT_TRUE(poll()->events.writable()) << "the retry hint";
    ip_.learn(aloe::testing::harness_ip, aloe::testing::harness_mac, now_);
    ASSERT_EQ(c->send(aloe::frames::pattern(10)), 10U);
    EXPECT_EQ(collect().back().ethernet.destination, aloe::testing::harness_mac);
}
