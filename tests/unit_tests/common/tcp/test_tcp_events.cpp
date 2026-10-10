#include <aloe/frames>
#include <aloe/stream>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>
#include <cstdint>
#include <tuple>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>

namespace {

    using namespace std::chrono_literals;
    using aloe::stream::Events;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpSequence;
    using TcpEvents = aloe::testing::TcpFixture;

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpEvents,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

}  // namespace

TEST_P(TcpEvents, PollConsumesSnapshot) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_FALSE(c->events().any());
    receive(peer_.fin());
    EXPECT_TRUE(c->events().peer_closed()) << "inspection sees the flag";
    EXPECT_TRUE(c->events().peer_closed()) << "and does not consume it";
    EXPECT_EQ(tcp_->pending_events(), 1U);
    receive(peer_.rst());
    EXPECT_EQ(tcp_->pending_events(), 1U) << "two raises, one entry";
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->connection, c);
    EXPECT_EQ(event->events, (Events{aloe::stream::Event::PeerClosed, aloe::stream::Event::Reset}))
        << "coalesced flags in one snapshot";
    EXPECT_FALSE(c->events().any());
    EXPECT_FALSE(poll().has_value());
    EXPECT_EQ(tcp_->pending_events(), 0U);
}

TEST_P(TcpEvents, EventsSurviveProcessingAndFlush) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.fin());
    advance(5ms);             // an empty process and a wheel advance
    std::ignore = collect();  // a flush
    receive(peer_.segment({TcpFlag::Ack}, {}, TcpSequence{5U}, TcpSequence{5U}));  // an unrelated stray segment
    std::ignore      = collect();
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.peer_closed());
}

TEST_P(TcpEvents, TimerSurvivesNextProcess) {
    const auto connected =
        tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    std::ignore = collect();
    // Every retry, then the timeout, raised from inside the wheel; one-second steps, as the wheel fires a timer
    // once per advance.
    for (std::int64_t second = 1; second <= 63; ++second) {
        advance(1s);
        const auto frames = collect();
        const bool retry  = second == 1 || second == 3 || second == 7 || second == 15 || second == 31;
        EXPECT_EQ(frames.size(), retry ? 1U : 0U) << "at " << second << " s";
    }
    EXPECT_EQ(tcp_->counters().control_segments_sent, 6U);
    EXPECT_EQ(tcp_->counters().connections_timed_out, 1U);
    EXPECT_EQ(tcp_->pending_events(), 1U);
    receive(peer_.segment({TcpFlag::Ack}, {}, TcpSequence{5U}, TcpSequence{5U}));  // a later receive pass
    std::ignore      = collect();
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.timed_out());
    EXPECT_EQ(event->connection, *connected);
}

TEST_P(TcpEvents, RaiseAfterPollQueuesAgain) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.fin());
    ASSERT_TRUE(poll().has_value());
    receive(peer_.rst());
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->events, Events{aloe::stream::Event::Reset}) << "a fresh notification with only the new flag";
    EXPECT_EQ(event->connection, c);
}

TEST_P(TcpEvents, ReleaseRemovesPendingEntryAndReuseIsClean) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.fin());
    EXPECT_EQ(tcp_->pending_events(), 1U);
    c->release();
    EXPECT_EQ(tcp_->pending_events(), 0U);
    EXPECT_FALSE(poll().has_value());
    std::ignore = collect();  // the RST release sent
    aloe::frames::TcpPeer next{aloe::testing::peer_spec(40001), TcpSequence{2000U}};
    auto* reused = open_passive(next);
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(reused, c) << "the same slot";
    EXPECT_EQ(reused->remote().port, 40001U);
    EXPECT_FALSE(reused->peer_closed()) << "nothing stale survived the release";
    EXPECT_FALSE(reused->events().any());
    EXPECT_FALSE(poll().has_value());
}

TEST_P(TcpEvents, ReleasingAnotherPendingConnectionLeavesTheRest) {
    auto* first = open_passive(peer_);
    ASSERT_NE(first, nullptr);
    aloe::frames::TcpPeer other{aloe::testing::peer_spec(40001), TcpSequence{2000U}};
    auto* second = open_passive(other);
    ASSERT_NE(second, nullptr);
    receive(peer_.fin());
    receive(other.fin());
    EXPECT_EQ(tcp_->pending_events(), 2U);
    first->release();
    EXPECT_EQ(tcp_->pending_events(), 1U);
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->connection, second);
    EXPECT_FALSE(poll().has_value());
}
