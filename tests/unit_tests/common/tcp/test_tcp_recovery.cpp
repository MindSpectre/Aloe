#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <tuple>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>

// Loss recovery, the one machine of three states: RFC 5681's duplicate ACK; what is in flight, which a duplicate lowers
// by a segment and so lets new data out (RFC 3042's limited transmit); the third duplicate, which marks the head lost
// under RFC 6582's guard and begins recovery, with PRR (RFC 9937) for the window; the partial ACK, which marks the new
// head; the loss state after a timeout, where the marked entries leave as slow start opens the window; and the window
// in writable(); and the window's growth, only while it limits what is sent. The peer offers no timestamps and no
// SACK, so a segment carries 1460 bytes, NewReno's initial window is ten of them, and the duplicates stand for what the
// peer holds; a handshake with a zero round trip leaves the RTO at rto_min, 200 ms. The recorder's tests check which
// hook the stack calls, when, with what.
namespace {

    using namespace std::chrono_literals;
    using aloe::tcp::RecoveryState;
    using TcpRecovery         = aloe::testing::TcpFixture;
    using TcpRecoveryRefusing = aloe::testing::TcpRefusingFixture;
    using Recovery            = aloe::tcp::detail::LossRecovery<aloe::tcp::NewReno>;
    constexpr auto unbounded  = std::numeric_limits<std::uint32_t>::max();

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

    /// The brick with no congestion control: the policy has no target, so recovery has no window of its own.
    class TcpUnbounded : public ::testing::TestWithParam<aloe::fabric::EmulatedOffloads>,
                         protected aloe::testing::detail::FabricPair,
                         protected aloe::testing::TcpHarness<aloe::fabric::Port, aloe::tcp::NoCongestionControl> {
    protected:
        TcpUnbounded()
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
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpRecoveryRefusing,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpUnbounded,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

}  // namespace

TEST(TcpLossRecovery, ADuplicateStandsForOneSegmentExceptInTheLossState) {
    Recovery recovery;
    recovery.open(aloe::wire::TcpSequence{1000});
    recovery.establish(1460);
    EXPECT_EQ(recovery.state(), RecoveryState::Open);
    EXPECT_EQ(recovery.duplicate_bytes(1460), 0U);
    EXPECT_EQ(recovery.count_duplicate(), 1U);
    EXPECT_EQ(recovery.count_duplicate(), 2U);
    EXPECT_EQ(recovery.duplicate_bytes(1460), 2920U);
    recovery.on_timeout(aloe::wire::TcpSequence{1001}, aloe::wire::TcpSequence{5381}, 4380, 0, 1460, true);
    EXPECT_EQ(recovery.state(), RecoveryState::Loss);
    EXPECT_EQ(recovery.duplicates(), 0U) << "the timeout clears the count";
    EXPECT_EQ(recovery.count_duplicate(), 1U);
    EXPECT_EQ(recovery.duplicate_bytes(1460), 0U) << "after a timeout a duplicate says nothing of what the peer holds";
}

TEST(TcpLossRecovery, AnAdvanceInRecoveryDeliversWhatTheDuplicatesDidNotAndKeepsTheRest) {
    Recovery recovery;
    recovery.open(aloe::wire::TcpSequence{1000});
    recovery.establish(1460);
    for (int duplicate = 0; duplicate < 8; ++duplicate) {
        std::ignore = recovery.count_duplicate();
    }
    recovery.begin_recovery(aloe::wire::TcpSequence{15601}, 14600, 14600, 1460);
    ASSERT_TRUE(recovery.in_recovery());
    // Five segments acknowledged after eight duplicates: the duplicates had counted all five, so only the hole that
    // was filled is new, and four of the eight still stand for segments above the new SND.UNA.
    EXPECT_EQ(recovery.remove_duplicates(7300, 1460), 1460U);
    EXPECT_EQ(recovery.duplicates(), 4U);
    // Six segments after four duplicates: two of them are new, and no duplicate is left.
    EXPECT_EQ(recovery.remove_duplicates(8760, 1460), 2920U);
    EXPECT_EQ(recovery.duplicates(), 0U);
    // A part of a segment counts as one: nothing is taken from the count, and a segment is delivered.
    std::ignore = recovery.count_duplicate();
    EXPECT_EQ(recovery.remove_duplicates(500, 1460), 1460U);
    EXPECT_EQ(recovery.duplicates(), 1U);
}

TEST(TcpLossRecovery, TheEpisodeCountsNoMoreThanTheFlightItRecorded) {
    Recovery recovery;
    recovery.open(aloe::wire::TcpSequence{1000});
    recovery.establish(1460);
    recovery.begin_recovery(aloe::wire::TcpSequence{5381}, 4380, 4380, 1460);
    EXPECT_EQ(recovery.undelivered(), 4380U);
    recovery.on_recovery_ack(1460, 0, false, 1460);
    EXPECT_EQ(recovery.undelivered(), 2920U);
    recovery.on_recovery_ack(2920, 0, false, 1460);
    EXPECT_EQ(recovery.undelivered(), 0U) << "RFC 9937 6.2: a duplicate past this delivers nothing";
}

TEST(TcpLossRecovery, TheTargetLeavesOutWhatWasSentBeyondTheWindow) {
    Recovery recovery;
    recovery.open(aloe::wire::TcpSequence{1000});
    recovery.establish(1460);
    ASSERT_EQ(recovery.window(), 14600U);
    // Two segments went out beyond the window while duplicates lowered what was in flight: 17520 outstanding.
    recovery.begin_recovery(aloe::wire::TcpSequence{18521}, 17520, 17520, 1460);
    EXPECT_EQ(recovery.congestion().ssthresh, 7300U) << "RFC 5681 3.2: half of 14600, not of 17520";
    EXPECT_EQ(recovery.undelivered(), 17520U) << "RecoverFS is the whole flight";

    Recovery within;
    within.open(aloe::wire::TcpSequence{1000});
    within.establish(1460);
    within.begin_recovery(aloe::wire::TcpSequence{5381}, 4380, 4380, 1460);
    EXPECT_EQ(within.congestion().ssthresh, 2920U) << "a flight inside the window is halved as it is: two segments";

    Recovery timed_out;
    timed_out.open(aloe::wire::TcpSequence{1000});
    timed_out.establish(1460);
    timed_out.on_timeout(aloe::wire::TcpSequence{1001}, aloe::wire::TcpSequence{18521}, 17520, 0, 1460, true);
    EXPECT_EQ(timed_out.congestion().ssthresh, 8760U) << "a timeout halves the flight it found";
}

TEST(TcpLossRecovery, TheGuardAdmitsOnlyAnAckAboveWhatWasOutstandingWhenTheLastEpisodeBegan) {
    using Sequence = aloe::wire::TcpSequence;
    Recovery recovery;
    recovery.open(Sequence{1000});
    recovery.establish(1460);
    EXPECT_TRUE(recovery.admits(Sequence{1001}))
        << "a fresh connection: the loss of its first segment is an ordinary one";
    recovery.on_timeout(Sequence{1001}, Sequence{5381}, 4380, 0, 1460, true);  // 5380 is the highest sequence out
    EXPECT_FALSE(recovery.recovered(Sequence{5380}));
    EXPECT_TRUE(recovery.recovered(Sequence{5381})) << "the episode ends at the ACK that covers recover";
    EXPECT_FALSE(recovery.admits(Sequence{5381})) << "RFC 6582 3.2: ack_number - 1 > recover";
    EXPECT_TRUE(recovery.admits(Sequence{5382}));
    recovery.end(Sequence{5381});
    EXPECT_EQ(recovery.state(), RecoveryState::Open);
    EXPECT_FALSE(recovery.admits(Sequence{5381})) << "an episode that ends exactly there leaves the guard in place";
    recovery.on_ack(aloe::tcp::AckInfo{.acked = 1, .flight = 1460, .mss = 1460}, Sequence{5382});  // one byte above
    EXPECT_TRUE(recovery.admits(Sequence{5382}));

    recovery.on_timeout(Sequence{5382}, Sequence{8302}, 2920, 0, 1460, true);
    recovery.end(Sequence{9000});  // new data sent in the episode was acknowledged with it: more than recover
    EXPECT_TRUE(recovery.admits(Sequence{9000}));
}

TEST(TcpLossRecovery, TheGuardWrapsAcrossTheTopOfTheSequenceSpace) {
    using Sequence = aloe::wire::TcpSequence;
    Recovery recovery;
    recovery.open(Sequence{0xfffff000U});
    recovery.establish(1460);
    recovery.on_timeout(Sequence{0xfffff001U}, Sequence{0}, 4095, 0, 1460, true);  // the last byte sent is 0xffffffff
    EXPECT_FALSE(recovery.recovered(Sequence{0xffffffffU}));
    EXPECT_TRUE(recovery.recovered(Sequence{0}));
    EXPECT_FALSE(recovery.admits(Sequence{0}));
    EXPECT_TRUE(recovery.admits(Sequence{1}));
}

TEST(TcpLossRecovery, RoomIsWhatTheWindowAllowsBeyondWhatIsInFlight) {
    Recovery recovery;
    recovery.open(aloe::wire::TcpSequence{1000});
    recovery.establish(1460);
    EXPECT_EQ(recovery.window(), 14600U);
    EXPECT_EQ(recovery.room(13140), 1460U);
    EXPECT_EQ(recovery.room(14600), 0U);
    EXPECT_EQ(recovery.room(20000), 0U);
    EXPECT_TRUE(recovery.may_send(14599));
    EXPECT_FALSE(recovery.may_send(14600)) << "RFC 9937: nothing leaves once what is in flight matches the window";
}

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

TEST_P(TcpRecovery, AnApplicationLimitedFlowKeepsItsInitialWindow) {
    // A venue client's orders, one at a time: 100 bytes in flight at each send, never the window, and slow start
    // grows only below twice the most in flight, 200. Growing on every ACK would have made it 14600 + 100 * 100.
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    for (int order = 0; order < 100; ++order) {
        ASSERT_EQ(c->send(aloe::frames::pattern(100)), 100U);
        const auto sent = collect();
        ASSERT_EQ(sent.size(), 1U);
        receive(peer_.ack(sent[0]));
        advance(0ms);  // the step brings the packet back from the device, so a hundred fit the port's pool
    }
    EXPECT_EQ(c->congestion_window(), 14600U);
}

TEST_P(TcpRecovery, AWindowLimitedFlowGrows) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(20000)), 14600U) << "the tenth segment fills the window";
    const auto sent = collect();
    ASSERT_EQ(sent.size(), 10U);
    receive(peer_.ack(sent[9]));
    EXPECT_EQ(c->congestion_window(), 16060U) << "slow start: one segment for the ACK of the window";
}

TEST_P(TcpRecovery, AnOfferTheCongestionWindowCutShortCountsAsTheWindowInUse) {
    // Congestion avoidance at the target of a recovery, 2920. Sends of 1460 and 1000 leave 2460 in flight, below the
    // window. A prepare of 3000 is offered 460, what the window has left, where the segment and the peer's window
    // would allow 1460: the window held the send back, so the round counts as limited however little is committed.
    // Of the 460, 200 are: 2660 in flight, still below the window. The ACK of the round then adds its 2660 to the
    // avoidance count; a round seen as unlimited would add nothing.
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(4380)), 4380U);
    const auto sent = collect();
    const auto dup  = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    receive(dup());
    receive(dup());
    ASSERT_EQ(collect().size(), 1U);
    receive(peer_.ack(sent[2]));
    ASSERT_EQ(c->recovery_state(), RecoveryState::Open);
    ASSERT_EQ(c->congestion_window(), 2920U);
    ASSERT_EQ(c->congestion_state().ssthresh, 2920U) << "congestion avoidance";
    ASSERT_EQ(c->send(aloe::frames::pattern(1460)), 1460U);
    ASSERT_EQ(c->send(aloe::frames::pattern(1000)), 1000U);
    const auto span = c->prepare(3000);
    ASSERT_TRUE(span.has_value());
    ASSERT_EQ(span->size(), 460U);
    ASSERT_TRUE(c->commit(200));
    ASSERT_EQ(c->in_flight(), 2660U);
    const auto round = collect();
    ASSERT_EQ(round.size(), 3U);
    receive(peer_.ack(round[2]));
    EXPECT_EQ(c->congestion_state().acked_bytes, 2660U);
    EXPECT_EQ(c->congestion_window(), 2920U) << "a window of acknowledged bytes is not reached yet";
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
    EXPECT_EQ(c->writable(), 0U) << "each duplicate took one segment out of what is in flight, and one went back in";
    EXPECT_EQ(c->in_flight(), 14600U);
    receive(dup());
    EXPECT_TRUE(c->in_recovery());
    EXPECT_EQ(c->recovery_state(), RecoveryState::Recovery);
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 3U) << "two limited-transmit segments, then the head";
    EXPECT_EQ(resent[2].tcp->sequence, sent[0].tcp->sequence);
    EXPECT_EQ(tcp_->counters().fast_retransmits, 1U);
    EXPECT_EQ(tcp_->counters().recoveries, 1U);
    // The flight is 17520, of which three duplicates stand for 4380 and the head, 1460, is lost: 11680 in flight. The
    // target leaves out the two segments sent beyond the window (RFC 5681 3.2): half of 14600, 7300. RecoverFS is
    // the whole flight, 17520, so this ACK's 1460 delivered allow ceil(1460 * 7300 / 17520) = 609 above what is in
    // flight, and the head leaves.
    EXPECT_EQ(c->congestion_state().ssthresh, 7300U);
    EXPECT_EQ(c->congestion_window(), 12289U) << "11680 in flight and 609";
    EXPECT_EQ(c->in_flight(), 13140U) << "the head, resent, is in flight again";
    receive(dup());
    // Four duplicates: 11680 in flight; 2920 delivered allow 1217, less than the head's resend already used.
    EXPECT_EQ(c->congestion_window(), 11680U);
    EXPECT_EQ(c->writable(), 0U);
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
    EXPECT_EQ(c->congestion_state().ssthresh, 2920U) << "max(4380 / 2, 2 * 1460)";
    receive(peer_.ack(sent[0]));  // partial: covers the first segment only
    EXPECT_TRUE(c->in_recovery());
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U) << "the new head, marked lost by the partial ACK, within what the ACK delivered";
    EXPECT_EQ(resent[0].tcp->sequence, sent[1].tcp->sequence);
    EXPECT_EQ(tcp_->counters().fast_retransmits, 2U);
    EXPECT_EQ(tcp_->counters().recoveries, 1U) << "one episode";
    receive(peer_.ack(sent[2]));  // the full ACK
    EXPECT_FALSE(c->in_recovery());
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open);
    EXPECT_EQ(c->congestion_window(), 2920U) << "the target: half of the flight at the third duplicate, two segments";
    EXPECT_EQ(c->duplicate_acks(), 0U);
}

TEST_P(TcpRecovery, TwoLossesInOneWindowAreRepairedWithoutATimeout) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(14600)), 14600U);  // ten segments; the first and the sixth are lost
    const auto sent = collect();
    ASSERT_EQ(sent.size(), 10U);
    const auto dup = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    for (int duplicate = 0; duplicate < 8; ++duplicate) {  // one for each of the eight segments that arrived
        receive(dup());
    }
    ASSERT_EQ(collect().size(), 1U) << "the head, at the third";
    EXPECT_EQ(c->in_flight(), 2920U) << "14600 less the eight segments the duplicates stand for";
    receive(peer_.ack(sent[4]));  // the head's resend arrived: everything below the sixth segment is acknowledged
    // Five segments acknowledged, which the duplicates had already counted but for the hole: one segment delivered.
    // Four duplicates still stand for the four segments above the new head, which is marked lost: nothing in flight.
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U) << "the second hole goes at once, clocked by the partial ACK";
    EXPECT_EQ(resent[0].tcp->sequence, sent[5].tcp->sequence);
    EXPECT_EQ(c->duplicate_acks(), 4U);
    EXPECT_EQ(c->in_flight(), 1460U) << "the resend alone";
    EXPECT_EQ(c->congestion_window(), 7300U) << "the conservative bound reached the target, 14600 / 2";
    EXPECT_EQ(tcp_->counters().fast_retransmits, 2U);
    receive(peer_.ack(sent[9]));
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open);
    EXPECT_EQ(c->congestion_window(), 7300U);
    EXPECT_EQ(tcp_->counters().retransmits, 0U);
    EXPECT_EQ(tcp_->counters().recoveries, 1U);
}

TEST_P(TcpRecovery, WithSackWhatTheBlocksReportIsWhatIsDelivered) {
    peer_.set_sack_permitted(true);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(14600)), 14600U);
    const auto sent = collect();
    ASSERT_EQ(sent.size(), 10U);
    const aloe::wire::TcpSequence first = sent[0].tcp->sequence;
    // A duplicate ACK whose one block reports everything from the second segment to `end` bytes past the first.
    const auto report                   = [&](const std::uint32_t end) {
        const std::array blocks{
            aloe::wire::TcpSackBlock{.first = first + 1460U, .end = first + end}
        };
        return peer_.sack(first, blocks);
    };
    receive(report(2920));
    EXPECT_EQ(c->in_flight(), 13140U);
    EXPECT_EQ(c->writable(), 1460U) << "a report takes what it covers out of what is in flight: limited transmit";
    receive(report(4380));
    receive(report(5840));
    // The third duplicate marks the head: of 14600, 4380 are reported and 1460 lost, 8760 in flight. The target is
    // 7300. RecoverFS is the flight less what is reported, plus the 1460 this ACK newly reported: 11680. PRR allows
    // ceil(1460 * 7300 / 11680) = 913.
    EXPECT_EQ(c->recovery_state(), RecoveryState::Recovery);
    EXPECT_EQ(c->congestion_window(), 9673U);
    ASSERT_EQ(collect().size(), 1U);
    EXPECT_EQ(c->in_flight(), 10220U);
    receive(peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), first));
    EXPECT_EQ(c->duplicate_acks(), 4U);
    EXPECT_EQ(c->congestion_window(), 9673U) << "a duplicate that reports nothing new delivered nothing: no step";
    receive(report(8760));
    // Two segments newly reported: 7300 in flight, which is the target, so the conservative bound holds the window.
    EXPECT_EQ(c->in_flight(), 7300U);
    EXPECT_EQ(c->congestion_window(), 7300U);
    EXPECT_EQ(tcp_->counters().fast_retransmits, 1U);
    EXPECT_EQ(tcp_->counters().recoveries, 1U);
}

TEST_P(TcpRecovery, WithSackAThirdDuplicateThatReportsNothingStillLetsTheHeadOut) {
    peer_.set_sack_permitted(true);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(14600)), 14600U);
    const auto sent = collect();
    const auto dup  = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    EXPECT_EQ(c->in_flight(), 14600U) << "with SACK only a block takes anything out of what is in flight";
    EXPECT_EQ(c->writable(), 0U);
    receive(dup());
    receive(dup());
    EXPECT_EQ(c->recovery_state(), RecoveryState::Recovery);
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, sent[0].tcp->sequence);
    EXPECT_EQ(c->congestion_window(), 14600U) << "nothing delivered, so no step: the 13140 in flight and one segment";
}

TEST_P(TcpRecovery, ATimeoutInRecoveryKeepsTheTarget) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(14600)), 14600U);
    std::ignore    = collect();
    const auto dup = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    ASSERT_EQ(c->send(aloe::frames::pattern(1460)), 1460U);
    receive(dup());
    ASSERT_EQ(c->send(aloe::frames::pattern(1460)), 1460U);
    receive(dup());
    ASSERT_EQ(c->recovery_state(), RecoveryState::Recovery);
    ASSERT_EQ(c->congestion_state().ssthresh, 7300U) << "half of the window the 17520 outstanding had passed";
    receive(dup());
    receive(dup());
    // Five duplicates: 4380 delivered allow ceil(4380 * 7300 / 17520) = 1825, of which the head took 1460. The 365
    // left go as new data, so the flight a timeout would halve is no longer the one recovery halved.
    ASSERT_EQ(c->writable(), 365U);
    ASSERT_EQ(c->send(aloe::frames::pattern(365)), 365U);
    ASSERT_EQ(c->unacknowledged(), 17885U);
    advance(200ms);  // the RTO, restarted by the fast retransmit, before any ACK of new data
    EXPECT_EQ(c->recovery_state(), RecoveryState::Loss);
    EXPECT_EQ(c->congestion_state().ssthresh, 7300U) << "this window was reduced already: not 17885 / 2";
    EXPECT_EQ(c->congestion_window(), 1460U) << "everything was marked, so one segment";
    EXPECT_EQ(c->duplicate_acks(), 0U);
    EXPECT_EQ(tcp_->counters().retransmits, 1U);
    EXPECT_EQ(tcp_->counters().recoveries, 1U);
}

TEST_P(TcpRecovery, ASynTimeoutLeavesTheEstablishedConnectionOpen) {
    const auto connected =
        tcp_->connect({.address = aloe::testing::harness_ip, .port = peer_.spec().source_port}, now_);
    ASSERT_TRUE(connected.has_value());
    auto* c = *connected;
    ASSERT_EQ(collect().size(), 1U);
    advance(1s);
    const auto again = collect();
    ASSERT_EQ(again.size(), 1U) << "the SYN again";
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open) << "the handshake's timer is not the policy's concern";
    receive(peer_.syn_ack(again[0]));
    ASSERT_EQ(c->state(), aloe::tcp::State::Established);
    EXPECT_EQ(c->congestion_window(), 14600U) << "the initial window, taken at Established";
    std::ignore = collect();
    ASSERT_EQ(c->send(aloe::frames::pattern(2920)), 2920U);
    const auto sent = collect();
    ASSERT_EQ(sent.size(), 2U);
    const auto dup = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    receive(dup());
    receive(dup());
    EXPECT_TRUE(c->in_recovery()) << "the first loss after a resent SYN is repaired by duplicates like any other";
    EXPECT_EQ(tcp_->counters().fast_retransmits, 1U);
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

TEST_P(TcpRecovery, AFastRetransmitIsAdmittedOnceAByteAboveWhatATimeoutFoundOutstandingIsAcknowledged) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(2920)), 2920U);
    const auto sent = collect();
    ASSERT_EQ(sent.size(), 2U);
    advance(200ms);
    ASSERT_EQ(collect().size(), 1U) << "the timeout's resend of the head; recover is the last byte sent before it";
    receive(peer_.ack(sent[1]));  // exactly snd_nxt as it was at the timeout: recover + 1
    EXPECT_TRUE(collect().empty()) << "the ACK that covers recover ends the loss state";
    ASSERT_EQ(c->send(aloe::frames::pattern(1460)), 1460U);
    const auto third = collect();
    ASSERT_EQ(third.size(), 1U);
    receive(peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), third[0].tcp->sequence + 1U));
    const auto dup = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    receive(dup());
    receive(dup());
    EXPECT_TRUE(c->in_recovery()) << "RFC 6582: one byte more than recover is acknowledged";
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, third[0].tcp->sequence + 1U) << "what is left of the head";
    EXPECT_EQ(tcp_->counters().fast_retransmits, 1U);
}

TEST_P(TcpRecovery, DuplicatesAtTheOldSndNxtAfterANeedlessTimeoutStartNoRecovery) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(5840)), 5840U);  // four segments, all of which arrive, late
    const auto sent = collect();
    ASSERT_EQ(sent.size(), 4U);
    advance(200ms);
    ASSERT_EQ(collect().size(), 1U) << "a needless resend of the head, which the peer will answer with a duplicate";
    receive(peer_.ack(sent[3]));  // everything, exactly the SND.NXT of the timeout
    ASSERT_EQ(c->recovery_state(), RecoveryState::Open);
    ASSERT_EQ(c->send(aloe::frames::pattern(1460)), 1460U);
    const auto fresh = collect();
    ASSERT_EQ(fresh.size(), 1U);
    const auto dup = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    receive(dup());
    receive(dup());
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open) << "RFC 6582 3.2: the ACK covers no more than recover";
    EXPECT_TRUE(collect().empty()) << "nothing is resent for duplicates the timeout's own resends can explain";
    EXPECT_EQ(tcp_->counters().recoveries, 0U);
    EXPECT_EQ(tcp_->counters().fast_retransmits, 0U);
    EXPECT_EQ(c->duplicate_acks(), 3U) << "refused, and counted all the same";
    EXPECT_EQ(tcp_->counters().duplicate_acks, 3U);
    EXPECT_EQ(c->in_flight(), 1460U) << "the hole of one segment the duplicates leave";
    receive(peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), fresh[0].tcp->sequence + 1U));
    receive(dup());
    receive(dup());
    receive(dup());
    EXPECT_TRUE(c->in_recovery()) << "a byte above the old SND.NXT was acknowledged: an ordinary loss again";
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, fresh[0].tcp->sequence + 1U);
    EXPECT_EQ(tcp_->counters().recoveries, 1U);
    EXPECT_EQ(tcp_->counters().fast_retransmits, 1U);
}

TEST_P(TcpRecovery, TheGuardHoldsAfterARecoveryThatEndedExactlyAtRecover) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(4380)), 4380U);
    const auto sent = collect();
    const auto dup  = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    receive(dup());
    receive(dup());
    ASSERT_EQ(collect().size(), 1U);
    receive(peer_.ack(sent[2]));  // the full ACK, of exactly what was outstanding when recovery began
    ASSERT_EQ(c->recovery_state(), RecoveryState::Open);
    ASSERT_EQ(c->send(aloe::frames::pattern(1460)), 1460U);
    const auto fresh = collect();
    ASSERT_EQ(fresh.size(), 1U);
    receive(dup());
    receive(dup());
    receive(dup());
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open) << "duplicates at the ACK that ended recovery begin no second";
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(tcp_->counters().recoveries, 1U);
    EXPECT_EQ(tcp_->counters().fast_retransmits, 1U);
    receive(peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), fresh[0].tcp->sequence + 1U));
    receive(dup());
    receive(dup());
    receive(dup());
    EXPECT_TRUE(c->in_recovery());
    EXPECT_EQ(tcp_->counters().recoveries, 2U);
    ASSERT_EQ(collect().size(), 1U);
}

TEST_P(TcpRecovery, FastRetransmitStillFiresWhenRecoverIsHalfTheSpaceBehind) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    // As after 2 GiB sent with no loss: `recover` more than half the sequence space behind `snd_una`, which the
    // serial comparison reads as ahead of it, so without the slide the guard would refuse every third duplicate.
    const aloe::wire::TcpSequence behind{c->acknowledged().value - 0x80000001U};
    ASSERT_TRUE(behind.after(c->acknowledged()));
    force_recover(*c, behind);
    ASSERT_EQ(c->send(aloe::frames::pattern(2920)), 2920U);
    const auto sent = collect();
    ASSERT_EQ(sent.size(), 2U);
    receive(peer_.ack(sent[0]));  // new data acknowledged outside recovery: recover slides behind snd_una
    const auto dup = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    receive(dup());
    receive(dup());
    EXPECT_TRUE(c->in_recovery());
    EXPECT_EQ(tcp_->counters().fast_retransmits, 1U);
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, sent[1].tcp->sequence);
}

TEST_P(TcpRecovery, AFastRetransmitRestartsTheRetransmissionTimer) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->rto(), 200ms);
    ASSERT_EQ(c->send(aloe::frames::pattern(2920)), 2920U);
    const auto sent = collect();  // t0: the timer is armed for t0 + 200 ms
    advance(150ms);
    const auto dup = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    receive(dup());
    receive(dup());
    ASSERT_EQ(collect().size(), 1U) << "the fast retransmit at t = t0 + 150 ms";
    EXPECT_EQ(tcp_->counters().fast_retransmits, 1U);
    advance(199ms);
    EXPECT_TRUE(collect().empty()) << "the timer restarted at t: nothing expires before t + 200 ms";
    EXPECT_EQ(tcp_->counters().retransmits, 0U);
    advance(1ms);
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, sent[0].tcp->sequence);
    EXPECT_EQ(tcp_->counters().retransmits, 1U);
}

TEST_P(TcpRecovery, AfterATimeoutTheMarkedEntriesAreResentUnderSlowStart) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(14600)), 14600U);  // ten segments, none acknowledged
    const auto sent = collect();
    ASSERT_EQ(sent.size(), 10U);
    advance(200ms);
    const auto first = collect();
    ASSERT_EQ(first.size(), 1U) << "the expiry resends the head and nothing else";
    EXPECT_EQ(first[0].tcp->sequence, sent[0].tcp->sequence);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Loss);
    EXPECT_EQ(c->lost_bytes(), 13140U) << "everything was marked; the head has left again";
    EXPECT_EQ(c->in_flight(), 1460U);
    EXPECT_EQ(c->congestion_window(), 1460U) << "nothing in flight after the marking, plus one segment";
    EXPECT_EQ(c->congestion_state().ssthresh, 7300U);
    receive(peer_.ack(sent[0]));
    EXPECT_EQ(c->congestion_window(), 2920U) << "slow start";
    const auto next = collect();
    ASSERT_EQ(next.size(), 2U) << "two marked entries fill the window";
    EXPECT_EQ(next[0].tcp->sequence, sent[1].tcp->sequence);
    EXPECT_EQ(next[1].tcp->sequence, sent[2].tcp->sequence);
    EXPECT_EQ(tcp_->counters().fast_retransmits, 2U);
    EXPECT_EQ(tcp_->counters().retransmits, 1U) << "one expiry, one count";
    receive(peer_.ack(sent[2]));
    EXPECT_EQ(c->congestion_window(), 4380U);
    const auto more = collect();
    ASSERT_EQ(more.size(), 3U);
    EXPECT_EQ(more[0].tcp->sequence, sent[3].tcp->sequence);
    EXPECT_EQ(tcp_->counters().fast_retransmits, 5U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Loss);
    receive(peer_.ack(sent[9]));  // everything sent before the timeout
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open);
    EXPECT_EQ(c->congestion_window(), 5840U) << "slow start once more, on the ACK that ends the loss state";
    EXPECT_EQ(c->lost_bytes(), 0U);
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(tcp_->counters().retransmits, 1U);
    EXPECT_EQ(tcp_->counters().recoveries, 0U) << "a timeout's episode is not a recovery";
}

TEST_P(TcpRecovery, AfterATimeoutNothingNewIsOfferedWhileMarkedEntriesWaitForTheWindow) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    for (int segment = 0; segment < 5; ++segment) {
        ASSERT_EQ(c->send(aloe::frames::pattern(100)), 100U);
    }
    const auto sent = collect();
    ASSERT_EQ(sent.size(), 5U);
    advance(200ms);
    ASSERT_EQ(collect().size(), 1U) << "the expiry's one segment, 100 bytes of a window of 1460";
    EXPECT_EQ(c->congestion_window(), 1460U);
    EXPECT_EQ(c->in_flight(), 100U);
    EXPECT_EQ(c->lost_bytes(), 400U);
    EXPECT_EQ(c->writable(), 0U) << "the rest of the window is the marked entries': a resend goes before anything new";
    EXPECT_EQ(c->send(aloe::frames::pattern(100)), 0U);
    // The head's ACK grows nothing: no send before the timeout was limited by the window, and the most in flight was
    // 500, so slow start would grow only a window below 1000. Linux, counting packets, grows here: five against one.
    // The four marked entries leave all the same, within the 1460.
    receive(peer_.ack(sent[0]));
    EXPECT_EQ(c->congestion_window(), 1460U);
    EXPECT_EQ(collect().size(), 4U);
    EXPECT_EQ(c->lost_bytes(), 0U);
    EXPECT_EQ(c->writable(), 1060U) << "nothing owes a resend any more: 1460 less the 400 in flight";
}

TEST_P(TcpRecovery, TheAckThatEndsTheLossStateGrowsTheWindowToo) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(1460)), 1460U);  // a lone segment, lost
    const auto sent = collect();
    advance(200ms);
    ASSERT_EQ(collect().size(), 1U);
    ASSERT_EQ(c->recovery_state(), RecoveryState::Loss);
    ASSERT_EQ(c->congestion_window(), 1460U);
    ASSERT_EQ(c->congestion_state().ssthresh, 2920U);
    receive(peer_.ack(sent[0]));  // covers everything sent before the timeout
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open);
    EXPECT_EQ(c->congestion_window(), 2920U) << "an ACK of new data in the loss state: slow start, as Linux grows too";
    EXPECT_EQ(c->writable(), 1460U);
}

TEST_P(TcpRecoveryRefusing, AResendTheDeviceRefusedIsTriedAgainByTheNextPassAndCountedOnce) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(4380)), 4380U);
    const auto sent = collect();
    ASSERT_EQ(sent.size(), 3U);
    const auto dup = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    receive(dup());
    refuse();
    receive(dup());  // the third marks the head; the device refuses its resend
    EXPECT_TRUE(c->in_recovery());
    EXPECT_EQ(tcp_->counters().fast_retransmits, 1U);
    EXPECT_EQ(c->lost_bytes(), 1460U) << "still owed";
    allow();
    EXPECT_TRUE(collect().empty()) << "a refused resend does not wait for the flush";
    receive(dup());  // the next ACK runs the pass again
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, sent[0].tcp->sequence);
    EXPECT_EQ(tcp_->counters().fast_retransmits, 1U) << "one mark, one count, however many tries";
    EXPECT_EQ(c->lost_bytes(), 0U);
}

TEST_P(TcpUnbounded, NoPolicyTargetMeansNoRateReduction) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(4380)), 4380U);
    const auto sent = collect();
    const auto dup  = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    receive(dup());
    receive(dup());
    EXPECT_EQ(c->recovery_state(), RecoveryState::Recovery);
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, sent[0].tcp->sequence);
    EXPECT_EQ(tcp_->counters().fast_retransmits, 1U);
    EXPECT_EQ(c->congestion_window(), unbounded) << "the policy's window stands through recovery";
    EXPECT_EQ(c->writable(), 1460U) << "the peer's window alone: 65535 less the 4380 outstanding";
    peer_.set_window(4380);
    receive(peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()));
    EXPECT_EQ(c->writable(), 0U) << "and nothing once the peer's window is full";
    receive(peer_.ack(sent[2]));
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open);
    EXPECT_EQ(c->congestion_window(), unbounded);
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
    EXPECT_FALSE(s.last_window_limited) << "and limited none";
    EXPECT_EQ(s.last_largest_flight, 2920U) << "both segments were in flight after the second send";
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

TEST_P(TcpRecorded, ThreeDuplicatesReachOnCongestion) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(2920)), 2920U);
    std::ignore    = collect();
    const auto dup = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    receive(dup());
    EXPECT_EQ(c->congestion_state().calls, 0U) << "what a duplicate lets out is the stack's: the policy hears nothing";
    receive(dup());
    const auto& s = c->congestion_state();
    EXPECT_EQ(s.last_hook, "on_congestion");
    EXPECT_EQ(s.last_flight, 2920U) << "the flight, all of it within the recorder's unbounded window";
    EXPECT_EQ(s.calls, 1U);
    receive(dup());
    EXPECT_EQ(s.calls, 1U) << "a duplicate in recovery is the rate reduction's, when there is one";
    EXPECT_EQ(c->duplicate_acks(), 4U);
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
    EXPECT_EQ(s.last_hook, "on_recovery_end") << "the ACK that ends the episode is not also an ACK for on_ack";
    EXPECT_EQ(s.calls, 2U) << "on_congestion, then on_recovery_end";
    EXPECT_FALSE(c->in_recovery());
}

TEST_P(TcpRecorded, APartialAckInRecoveryReachesNoHook) {
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
    EXPECT_EQ(s.last_hook, "on_congestion") << "the window in recovery is not the policy's to grow";
    EXPECT_EQ(s.calls, 1U);
    EXPECT_TRUE(c->in_recovery());
}

TEST_P(TcpRecorded, ATimeoutSetsTheTargetOnceAndRestartsTheWindowAtEachExpiry) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(1460)), 1460U);
    std::ignore = collect();
    advance(200ms);
    const auto& s = c->congestion_state();
    EXPECT_EQ(s.last_hook, "on_timeout");
    EXPECT_EQ(s.calls, 2U) << "on_congestion, then on_timeout";
    EXPECT_EQ(s.last_flight, 1460U) << "on_congestion heard the flight";
    EXPECT_EQ(s.last_in_flight, 0U) << "on_timeout what is in flight after the marking";
    advance(400ms);
    EXPECT_EQ(c->retransmit_count(), 2U) << "two expiries";
    EXPECT_EQ(s.last_hook, "on_timeout");
    EXPECT_EQ(s.calls, 3U) << "one episode, one target: on_timeout alone";
}

TEST_P(TcpRecorded, InTheLossStateEveryAckOfNewDataReachesOnAckTheOneThatEndsItToo) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(2920)), 2920U);
    const auto sent = collect();
    advance(200ms);
    const auto& s = c->congestion_state();
    ASSERT_EQ(s.calls, 2U);
    receive(peer_.ack(sent[0]));
    EXPECT_EQ(s.last_hook, "on_ack");
    EXPECT_EQ(s.calls, 3U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Loss);
    receive(peer_.ack(sent[1]));
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open);
    EXPECT_EQ(s.last_hook, "on_ack");
    EXPECT_EQ(s.last_acked, 1460U);
    EXPECT_EQ(s.calls, 4U) << "the ACK that ends the loss state is an ACK of new data like the one before it";
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

TEST_P(TcpRecovery, ADuplicateAckIsJudgedByTheScaledWindow) {
    peer_.set_window_scale(7);
    peer_.set_window(512);  // 65536 bytes once scaled
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(14600)), 14600U);
    std::ignore    = collect();
    const auto dup = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    for (int i = 0; i < 3; ++i) {
        receive(dup());
    }
    EXPECT_EQ(c->duplicate_acks(), 3U) << "a field of 512 states the 65536 bytes on record";
    EXPECT_EQ(tcp_->counters().fast_retransmits, 1U);
}
