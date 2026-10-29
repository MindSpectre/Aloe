#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>
#include <cstdint>
#include <tuple>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>

// Zero-window probes: a pure ACK at snd_una - 1 after one RTO, then at doubling intervals, while the peer's window is
// zero and nothing is queued; unanswered probes count against data_retries, the user timeout bounds probing from its
// start, and a window that opens ends it. A handshake with a zero round trip leaves the RTO at rto_min, 200 ms.
namespace {

    using namespace std::chrono_literals;
    using aloe::wire::TcpFlag;
    using TcpProbes         = aloe::testing::TcpFixture;
    using TcpProbesRefusing = aloe::testing::TcpRefusingFixture;

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpProbes,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpProbesRefusing,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

}  // namespace

TEST_P(TcpProbes, AZeroWindowWithAnEmptyQueueStartsProbingAtTheRto) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const aloe::wire::TcpSequence iss{c->committed().value - 1U};
    peer_.set_window(0);
    receive(peer_.ack());
    EXPECT_TRUE(c->probing());
    EXPECT_EQ(c->writable(), 0U);
    advance(199ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);
    const auto probe = collect();
    ASSERT_EQ(probe.size(), 1U);
    EXPECT_EQ(probe[0].tcp->flags, aloe::wire::TcpFlags{aloe::wire::TcpFlag::Ack});
    EXPECT_EQ(probe[0].tcp->sequence, iss) << "snd_una - 1";
    EXPECT_EQ(aloe::frames::tcp_payload(probe[0]).size(), 0U);
    EXPECT_EQ(tcp_->counters().zero_window_probes, 1U);
    advance(400ms);
    EXPECT_EQ(collect().size(), 1U) << "doubling";
    advance(800ms);
    EXPECT_EQ(collect().size(), 1U);
    EXPECT_EQ(c->queued_segments(), 0U) << "a probe takes no entry";
}

TEST_P(TcpProbes, AnAckResetsTheCountAndAnOpenedWindowEndsProbing) {
    configure_tcp({.data_retries = 2});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    peer_.set_window(0);
    receive(peer_.ack());
    advance(200ms);
    std::ignore = collect();
    receive(peer_.ack());  // the peer answers the probe, window still zero
    advance(400ms);
    std::ignore = collect();
    receive(peer_.ack());
    advance(800ms);
    std::ignore = collect();
    EXPECT_EQ(c->state(), aloe::tcp::State::Established) << "every probe was answered: the count never reached the cap";
    peer_.set_window(65535);
    receive(peer_.ack());
    EXPECT_FALSE(c->probing());
    EXPECT_TRUE(poll()->events.writable());
    EXPECT_GT(c->writable(), 0U);
    advance(5s);
    EXPECT_TRUE(collect().empty());
}

TEST_P(TcpProbes, UnansweredProbesEndInTimedOut) {
    configure_tcp({.data_retries = 2});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    peer_.set_window(0);
    receive(peer_.ack());
    advance(200ms);
    advance(400ms);
    EXPECT_EQ(tcp_->counters().zero_window_probes, 2U);
    advance(800ms);
    EXPECT_TRUE(poll()->events.timed_out());
    EXPECT_EQ(c->state(), aloe::tcp::State::Closed);
    EXPECT_EQ(tcp_->counters().connections_timed_out, 1U);
}

TEST_P(TcpProbes, TheUserTimeoutBoundsProbing) {
    configure_tcp({.user_timeout = std::chrono::milliseconds{500}});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    peer_.set_window(0);
    receive(peer_.ack());
    advance(200ms);
    std::ignore = collect();
    receive(peer_.ack());
    advance(400ms);  // 600 ms since probing started
    EXPECT_TRUE(poll()->events.timed_out());
    EXPECT_EQ(tcp_->counters().connections_user_timed_out, 1U);
}

TEST_P(TcpProbes, ASendWhileTheWindowOpenedTakesTheTimerBack) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    peer_.set_window(0);
    receive(peer_.ack());
    peer_.set_window(100);
    receive(peer_.ack());
    EXPECT_FALSE(c->probing());
    ASSERT_EQ(c->send(aloe::frames::pattern(10)), 10U);
    std::ignore = collect();
    advance(200ms);
    EXPECT_EQ(collect().size(), 1U) << "the RTO, not a probe";
    EXPECT_EQ(tcp_->counters().zero_window_probes, 0U);
}

TEST_P(TcpProbes, TheUserTimeoutClampsTheProbeTimerToItsDeadline) {
    configure_tcp({.user_timeout = std::chrono::milliseconds{500}});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    peer_.set_window(0);
    receive(peer_.ack());
    advance(200ms);
    ASSERT_EQ(collect().size(), 1U);
    receive(peer_.ack());
    advance(299ms);
    EXPECT_EQ(c->state(), aloe::tcp::State::Established) << "the next probe was due at 600 ms";
    advance(1ms);
    EXPECT_EQ(c->state(), aloe::tcp::State::Closed) << "the deadline, 500 ms after probing started, came first";
    EXPECT_TRUE(collect().empty()) << "no probe at the deadline";
    EXPECT_EQ(tcp_->counters().connections_user_timed_out, 1U);
}

TEST_P(TcpProbes, AZeroWindowInTheSynAckStartsProbing) {
    peer_.set_window(0);
    auto* c = open_active(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_TRUE(c->probing()) << "the SYN-ACK is the ACK that emptied the queue";
    advance(200ms);
    const auto probe = collect();
    ASSERT_EQ(probe.size(), 1U);
    EXPECT_EQ(probe[0].tcp->sequence, aloe::wire::TcpSequence{c->acknowledged().value - 1U});
    EXPECT_EQ(tcp_->counters().zero_window_probes, 1U);
}

TEST_P(TcpProbes, ACloseWhileProbingGivesTheTimerToTheFin) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    peer_.set_window(0);
    receive(peer_.ack());
    c->close();
    EXPECT_FALSE(c->probing());
    const auto fin = collect();
    ASSERT_EQ(fin.size(), 1U);
    EXPECT_TRUE(fin[0].tcp->flags.has(TcpFlag::Fin));
    advance(200ms);
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_TRUE(resent[0].tcp->flags.has(TcpFlag::Fin)) << "the RTO resends the FIN";
    EXPECT_EQ(tcp_->counters().zero_window_probes, 0U);
}

TEST_P(TcpProbesRefusing, ARefusedProbeCountsAndTheIntervalDoublesAllTheSame) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    peer_.set_window(0);
    receive(peer_.ack());
    const std::uint64_t refused = tcp_->counters().send_refused;
    refuse();
    advance(200ms);
    EXPECT_EQ(tcp_->counters().send_refused, refused + 1U);
    EXPECT_EQ(tcp_->counters().zero_window_probes, 0U);
    EXPECT_TRUE(c->probing());
    allow();
    std::ignore = collect();
    advance(399ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);
    EXPECT_EQ(collect().size(), 1U) << "400 ms after the refused probe";
    EXPECT_EQ(tcp_->counters().zero_window_probes, 1U);
}

TEST_P(TcpProbesRefusing, ARefusedFinWhileProbingStillTakesTheTimer) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    peer_.set_window(0);
    receive(peer_.ack());
    refuse();
    c->close();
    EXPECT_FALSE(c->probing()) << "the FIN's entry waits for the RTO";
    allow();
    std::ignore = collect();
    advance(200ms);
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_TRUE(resent[0].tcp->flags.has(TcpFlag::Fin));
    EXPECT_EQ(tcp_->counters().zero_window_probes, 0U);
}
