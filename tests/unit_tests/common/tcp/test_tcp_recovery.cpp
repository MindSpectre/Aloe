#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>
#include <cstdint>
#include <tuple>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>

// Loss recovery: RFC 5681's duplicate ACK, RFC 3042's limited transmit, the fast retransmit of the head on the third
// duplicate with RFC 6582's guard, NewReno's partial and full ACKs, the loss episode after a timeout, and the
// congestion window in writable(). The peer offers no timestamps, so a segment carries 1460 bytes and NewReno's
// initial window is ten of them; a handshake with a zero round trip leaves the RTO at rto_min, 200 ms. The recorder's
// tests check which hook the stack calls, when, with what.
namespace {

    using namespace std::chrono_literals;
    using TcpRecovery = aloe::testing::TcpFixture;

    class TcpRecorded
        : public ::testing::TestWithParam<aloe::fabric::EmulatedOffloads>,
          protected aloe::testing::detail::FabricPair,
          protected aloe::testing::TcpHarness<aloe::fabric::Port, aloe::testing::RecordingCongestionControl> {
    protected:
        TcpRecorded()
            : FabricPair{GetParam()},
              TcpHarness{fabric, harness, port} {
        }
    };

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpRecovery,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpRecorded,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

}  // namespace

TEST_P(TcpRecovery, TheDuplicateAckDefinitionAgainstEachCondition) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(10)), 10U);
    const auto sent = collect();
    const auto dup  = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    EXPECT_EQ(c->duplicate_acks(), 1U);
    peer_.set_window(65534);
    receive(dup());
    EXPECT_EQ(c->duplicate_acks(), 1U) << "a window that differs is not a duplicate";
    peer_.set_window(65535);
    receive(peer_.data(aloe::frames::pattern(1)));  // takes its sequence space, so the ACKs after it are in order
    EXPECT_EQ(c->duplicate_acks(), 1U) << "data is not a duplicate";
    receive(peer_.ack(sent[0]));
    EXPECT_EQ(c->duplicate_acks(), 0U) << "new data acknowledged resets the count";
    receive(dup());
    EXPECT_EQ(c->duplicate_acks(), 0U) << "an empty queue: nothing to be a duplicate of";
    EXPECT_EQ(tcp_->counters().duplicate_acks, 1U);
}

TEST_P(TcpRecovery, WritableIsBoundedByTheCongestionWindow) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->send(aloe::frames::pattern(20000)), 14600U) << "ten segments of 1460: RFC 6928";
    EXPECT_EQ(c->writable(), 0U);
}

TEST_P(TcpRecovery, LimitedTransmitAllowsTwoSegmentsBeyondTheWindowThenTheThirdDuplicateResendsTheHead) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(14600)), 14600U);
    const auto sent = collect();
    const auto dup  = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    EXPECT_TRUE(poll()->events.writable());
    EXPECT_EQ(c->writable(), 1460U);
    ASSERT_EQ(c->send(aloe::frames::pattern(1460)), 1460U);
    receive(dup());
    ASSERT_EQ(c->send(aloe::frames::pattern(1460)), 1460U);
    EXPECT_EQ(c->writable(), 0U) << "two segments is the allowance";
    receive(dup());
    EXPECT_TRUE(c->in_recovery());
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 3U) << "two limited-transmit segments, then the head";
    EXPECT_EQ(resent[2].tcp->sequence, sent[0].tcp->sequence);
    EXPECT_EQ(tcp_->counters().fast_retransmits, 1U);
    EXPECT_EQ(tcp_->counters().recoveries, 1U);
    EXPECT_EQ(c->congestion_window(), 7300U + 3U * 1460U) << "ssthresh = 14600 / 2, inflated by three";
    receive(dup());
    EXPECT_EQ(c->congestion_window(), 7300U + 4U * 1460U);
}

TEST_P(TcpRecovery, LimitedTransmitStaysWithinThePeersWindow) {
    peer_.set_window(4380);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(14600)), 4380U) << "the peer's window is the smaller one";
    std::ignore    = collect();
    const auto dup = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    EXPECT_EQ(c->duplicate_acks(), 1U);
    EXPECT_EQ(c->writable(), 0U) << "RFC 3042: new data only where the peer's window allows it";
}

TEST_P(TcpRecovery, APartialAckResendsTheNewHeadAndTheFullAckEndsRecovery) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(4380)), 4380U);  // three segments
    const auto sent = collect();
    const auto dup  = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    receive(dup());
    receive(dup());
    ASSERT_EQ(collect().size(), 1U);
    receive(peer_.ack(sent[0]));  // partial: covers the first segment only
    EXPECT_TRUE(c->in_recovery());
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, sent[1].tcp->sequence);
    EXPECT_EQ(tcp_->counters().fast_retransmits, 2U);
    receive(peer_.ack(sent[2]));  // the full ACK
    EXPECT_FALSE(c->in_recovery());
    EXPECT_EQ(c->congestion_window(), 2920U) << "min(ssthresh 2920, max(0, 1460) + 1460)";
}

TEST_P(TcpRecovery, TheRecoverGuardBlocksASecondFastRetransmitAfterATimeout) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(2920)), 2920U);
    std::ignore = collect();
    advance(200ms);
    ASSERT_EQ(collect().size(), 1U) << "the timeout's resend";
    const auto dup = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    receive(dup());
    receive(dup());
    EXPECT_FALSE(c->in_recovery());
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(tcp_->counters().fast_retransmits, 0U);
}

TEST_P(TcpRecovery, AfterATimeoutEachAckOfNewDataResendsTheNextHead) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(4380)), 4380U);
    const auto sent = collect();
    advance(200ms);
    ASSERT_EQ(collect().size(), 1U);
    EXPECT_EQ(c->congestion_window(), 1460U);
    receive(peer_.ack(sent[0]));
    const auto second = collect();
    ASSERT_EQ(second.size(), 1U);
    EXPECT_EQ(second[0].tcp->sequence, sent[1].tcp->sequence);
    EXPECT_EQ(c->congestion_window(), 2920U) << "slow start";
    receive(peer_.ack(sent[1]));
    ASSERT_EQ(collect().size(), 1U);
    receive(peer_.ack(sent[2]));
    EXPECT_TRUE(collect().empty()) << "the episode ended with the ACK past recover";
    EXPECT_EQ(tcp_->counters().retransmits, 3U);
}

TEST_P(TcpRecorded, TheHooksFireInOrderWithTheirValues) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->congestion_state().last_hook, "reset");
    ASSERT_EQ(c->send(aloe::frames::pattern(2920)), 2920U);
    const auto sent = collect();
    advance(7ms);
    receive(peer_.ack(sent[0]));
    const auto& s = c->congestion_state();
    EXPECT_EQ(s.last_hook, "on_ack");
    EXPECT_EQ(s.last_acked, 1460U);
    EXPECT_EQ(s.last_flight, 2920U);
    EXPECT_EQ(s.last_rtt, 7ms);
    EXPECT_EQ(s.last_delivered, 1460U);
    EXPECT_EQ(s.last_delivered_at_send, 0U);
    EXPECT_TRUE(s.last_app_limited) << "an unbounded window: every send left window unused";
}

TEST_P(TcpRecorded, AnAckInsideAnEntryCountsItsBytesAsDeliveredAtOnce) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(1460)), 1460U);
    const auto sent = collect();
    ASSERT_EQ(sent.size(), 1U);
    receive(peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), sent[0].tcp->sequence + 500U));
    const auto& s = c->congestion_state();
    EXPECT_EQ(s.last_hook, "on_ack");
    EXPECT_EQ(s.last_acked, 500U);
    EXPECT_EQ(s.last_delivered, 500U) << "the part of the entry the ACK covers";
    EXPECT_EQ(c->queued_segments(), 1U) << "the entry stays whole";
    receive(peer_.ack(sent[0]));
    EXPECT_EQ(s.last_acked, 960U);
    EXPECT_EQ(s.last_delivered, 1460U) << "the rest, once";
    EXPECT_EQ(c->delivered(), 1460U);
}

TEST_P(TcpRecorded, ThreeDuplicatesReachOnLoss) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(2920)), 2920U);
    std::ignore    = collect();
    const auto dup = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    receive(dup());
    EXPECT_EQ(c->congestion_state().calls, 0U) << "limited transmit is the stack's: the policy hears nothing";
    receive(dup());
    const auto& s = c->congestion_state();
    EXPECT_EQ(s.last_hook, "on_loss");
    EXPECT_EQ(s.last_flight, 2920U);
    EXPECT_EQ(s.calls, 1U);
}

TEST_P(TcpRecorded, AFourthReachesOnDupack) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(2920)), 2920U);
    std::ignore    = collect();
    const auto dup = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    receive(dup());
    receive(dup());
    receive(dup());
    const auto& s = c->congestion_state();
    EXPECT_EQ(s.last_hook, "on_dupack");
    EXPECT_EQ(s.calls, 2U) << "on_loss, then on_dupack";
    EXPECT_EQ(c->duplicate_acks(), 4U);
}

TEST_P(TcpRecorded, APartialAckReachesOnPartialAck) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(4380)), 4380U);
    const auto sent = collect();
    const auto dup  = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    receive(dup());
    receive(dup());
    receive(peer_.ack(sent[0]));
    const auto& s = c->congestion_state();
    EXPECT_EQ(s.last_hook, "on_partial_ack");
    EXPECT_EQ(s.last_acked, 1460U);
    EXPECT_TRUE(c->in_recovery());
}

TEST_P(TcpRecorded, AFullAckReachesOnRecoveryEnd) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(2920)), 2920U);
    const auto sent = collect();
    const auto dup  = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    receive(dup());
    receive(dup());
    receive(peer_.ack(sent[1]));
    const auto& s = c->congestion_state();
    EXPECT_EQ(s.last_hook, "on_recovery_end");
    EXPECT_EQ(s.last_flight, 0U) << "the flight after the ACK";
    EXPECT_FALSE(c->in_recovery());
}

TEST_P(TcpRecorded, ATimeoutReachesOnTimeoutOnceForTwoExpiries) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(1460)), 1460U);
    std::ignore = collect();
    advance(200ms);
    EXPECT_EQ(c->congestion_state().last_hook, "on_timeout");
    EXPECT_EQ(c->congestion_state().last_flight, 1460U);
    EXPECT_EQ(c->congestion_state().calls, 1U);
    advance(400ms);
    EXPECT_EQ(c->retransmit_count(), 2U) << "two expiries";
    EXPECT_EQ(c->congestion_state().calls, 1U) << "one episode, one call";
}

TEST_P(TcpRecorded, ASendAfterAnIdleLongerThanTheRtoReachesOnIdle) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->rto(), 200ms);
    ASSERT_EQ(c->send(aloe::frames::pattern(10)), 10U);
    receive(peer_.ack(collect()[0]));
    advance(100ms);
    ASSERT_EQ(c->send(aloe::frames::pattern(10)), 10U);
    EXPECT_EQ(c->congestion_state().last_hook, "on_ack") << "idle 100 ms, within the RTO";
    receive(peer_.ack(collect()[0]));
    advance(300ms);
    ASSERT_EQ(c->send(aloe::frames::pattern(10)), 10U);
    EXPECT_EQ(c->congestion_state().last_hook, "on_idle") << "idle 300 ms, past the RTO of 200";
}
