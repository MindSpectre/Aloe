#include <algorithm>
#include <aloe/core>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>
#include <tcp_sack_brick.hpp>

// The tail loss probe, RFC 8985 section 7: the probe's wait, its flight and its verdict (`TailProbe`); then the brick:
// the probe's timer and what an expiry sends, the RTO after it, what the probe's ACK means, and the probe through the
// busy rule. The brick's peer offers timestamps and SACK-permitted, so a segment carries 1448 bytes and NewReno's
// initial window is 14480, and its handshake takes 10 ms: srtt 10 ms, rttvar 5 ms, an RTO of 210 ms. The sends follow
// the handshake at 10 ms. A probe waits two smoothed round trips, 20 ms, plus the 40 ms allowance when one segment is
// in flight: 60 ms; after it the RTO runs 210 ms from the probe. Times in the comments count from the handshake's SYN.
namespace {

    using namespace std::chrono_literals;
    using aloe::tcp::RecoveryState;
    using aloe::wire::TcpSequence;
    using Blocks    = std::vector<aloe::wire::TcpSackBlock>;
    using TailProbe = aloe::tcp::detail::TailProbe;
    using Verdict   = TailProbe::Verdict;
    using Frames    = std::vector<aloe::frames::ParsedFrame>;

    using TcpTailProbeBrick    = aloe::testing::SackBrick<aloe::testing::TcpFixture>;
    using TcpTailProbeHeld     = aloe::testing::SackBrick<aloe::testing::TcpHeldFixture>;
    using TcpTailProbeRefusing = aloe::testing::SackBrick<aloe::testing::TcpRefusingFixture>;
    using TcpTailProbeRecorded = aloe::testing::SackBrick<aloe::testing::TcpRecordedFixture>;

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpTailProbeBrick,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpTailProbeHeld,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpTailProbeRefusing,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpTailProbeRecorded,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

    /// A probe that left at 2000 and was sampled before it: in flight, its verdict open.
    [[nodiscard]] TailProbe probed() {
        TailProbe probe;
        probe.sampled();
        probe.sent(TcpSequence{2000});
        return probe;
    }

}  // namespace

TEST(TcpTailProbe, TheWaitIsTwoRoundTripsAndTheAllowanceForOneSegment) {
    EXPECT_EQ(TailProbe::wait(10ms, false, 40ms, 1s), 20ms);
    EXPECT_EQ(TailProbe::wait(10ms, true, 40ms, 1s), 60ms);
    EXPECT_EQ(TailProbe::wait(10ms, true, 0ms, 1s), 20ms);
    EXPECT_EQ(TailProbe::wait(std::nullopt, false, 40ms, 1s), 1s) << "no sample yet: the initial RTO";
    EXPECT_EQ(TailProbe::wait(std::nullopt, true, 40ms, 1s), 1040ms);
}

TEST(TcpTailProbe, NoProbeBeforeASample) {
    TailProbe probe;
    EXPECT_FALSE(probe.may_probe());
    EXPECT_FALSE(probe.in_flight());
    EXPECT_FALSE(probe.verdict_open());
    EXPECT_FALSE(probe.end().has_value());
    probe.sampled();
    EXPECT_TRUE(probe.may_probe());
}

TEST(TcpTailProbe, NoSecondProbeWhileOneIsInFlight) {
    TailProbe probe = probed();
    probe.sampled();
    EXPECT_FALSE(probe.may_probe()) << "a sample since, but the probe is still in flight";
    EXPECT_TRUE(probe.in_flight());
    EXPECT_TRUE(probe.verdict_open());
    EXPECT_EQ(probe.end(), TcpSequence{2000});
}

TEST(TcpTailProbe, AnAckOnTheProbesEndLeavesTheVerdictOpen) {
    TailProbe probe = probed();
    probe.sampled();
    EXPECT_EQ(probe.on_ack(TcpSequence{2000}, false, false), Verdict::Open) << "the original's ACK or the probe's";
    EXPECT_FALSE(probe.in_flight());
    EXPECT_TRUE(probe.verdict_open());
    EXPECT_TRUE(probe.may_probe());
}

TEST(TcpTailProbe, ADuplicateReportForTheProbeIsNeedless) {
    TailProbe probe = probed();
    EXPECT_EQ(probe.on_ack(TcpSequence{2000}, true, false), Verdict::Needless);
    EXPECT_FALSE(probe.verdict_open());
    EXPECT_FALSE(probe.in_flight());
    EXPECT_FALSE(probe.end().has_value());
}

TEST(TcpTailProbe, AnAckPastTheEndIsRepaired) {
    TailProbe probe = probed();
    EXPECT_EQ(probe.on_ack(TcpSequence{2100}, false, false), Verdict::Repaired);
    EXPECT_FALSE(probe.verdict_open());
    EXPECT_FALSE(probe.in_flight());
}

TEST(TcpTailProbe, APlainDuplicateAckIsNeedless) {
    TailProbe probe = probed();
    ASSERT_EQ(probe.on_ack(TcpSequence{2000}, false, false), Verdict::Open);
    EXPECT_EQ(probe.on_ack(TcpSequence{2000}, false, true), Verdict::Needless)
        << "both copies arrived at a peer that sends no duplicate report";
    EXPECT_FALSE(probe.verdict_open());
}

TEST(TcpTailProbe, AnAckShortOfTheEndSettlesNothing) {
    TailProbe probe = probed();
    EXPECT_EQ(probe.on_ack(TcpSequence{1900}, true, true), Verdict::Open);
    EXPECT_TRUE(probe.in_flight());
    EXPECT_TRUE(probe.verdict_open());
}

TEST(TcpTailProbe, ForgetClearsBoth) {
    TailProbe probe = probed();
    probe.forget();
    EXPECT_FALSE(probe.in_flight());
    EXPECT_FALSE(probe.verdict_open());
    EXPECT_FALSE(probe.end().has_value());
}

TEST(TcpTailProbe, WrapsAcrossTheTopOfTheSequenceSpace) {
    TailProbe probe;
    probe.sampled();
    probe.sent(TcpSequence{0x00000040U});
    EXPECT_EQ(probe.on_ack(TcpSequence{0xffffffc0U}, false, false), Verdict::Open);
    EXPECT_TRUE(probe.in_flight()) << "0xffffffc0 is before 0x40 across the wrap";
    EXPECT_TRUE(probe.verdict_open());
    EXPECT_EQ(probe.on_ack(TcpSequence{0x00000040U}, false, false), Verdict::Open);
    EXPECT_FALSE(probe.in_flight());
    EXPECT_TRUE(probe.verdict_open());
}

TEST_P(TcpTailProbeBrick, ALoneSegmentIsProbedAtTwoRoundTripsPlusTheAllowance) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    Frames one;
    ASSERT_TRUE(send(*c, 1, one));  // at 10 ms
    advance(59ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);  // 2 * 10 + 40 = 60 ms after the send
    const auto probe = collect();
    ASSERT_EQ(probe.size(), 1U);
    EXPECT_EQ(probe[0].tcp->sequence, one[0].tcp->sequence);
    EXPECT_TRUE(std::ranges::equal(aloe::frames::tcp_payload(probe[0]), aloe::frames::tcp_payload(one[0])));
    EXPECT_EQ(counters().tail_probes, 1U);
    EXPECT_EQ(counters().retransmits, 0U) << "a probe is no timeout";
    EXPECT_EQ(counters().fast_retransmits, 0U) << "and no resend of a marked entry";
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open);
    EXPECT_EQ(c->retransmit_count(), 0U);
    EXPECT_EQ(c->rto(), 210ms) << "no backoff";
    EXPECT_TRUE(c->tail_probe_in_flight());
    EXPECT_TRUE(c->tail_probe_verdict_open());
}

TEST_P(TcpTailProbeBrick, TheTailOfABurstIsProbedAtTwoRoundTrips) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    Frames three;
    ASSERT_TRUE(send(*c, 3, three));
    advance(19ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);  // several segments in flight: no allowance
    const auto probe = collect();
    ASSERT_EQ(probe.size(), 1U) << "the last segment sent, and nothing else";
    EXPECT_EQ(probe[0].tcp->sequence, three[2].tcp->sequence);
    EXPECT_EQ(counters().tail_probes, 1U);
}

TEST_P(TcpTailProbeBrick, AnAllowanceOfZeroProbesALoneSegmentAtTwoRoundTrips) {
    configure_tcp({.tail_probe_ack_delay = 0ms});
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    Frames one;
    ASSERT_TRUE(send(*c, 1, one));
    advance(19ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);
    const auto probe = collect();
    ASSERT_EQ(probe.size(), 1U);
    EXPECT_EQ(probe[0].tcp->sequence, one[0].tcp->sequence);
    EXPECT_EQ(counters().tail_probes, 1U);
}

TEST_P(TcpTailProbeBrick, TheProbeNeverWaitsPastTheRto) {
    configure_tcp({.tail_probe_ack_delay = 500ms});
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    Frames one;
    ASSERT_TRUE(send(*c, 1, one));
    // 20 + 500 ms would pass the RTO's moment, 210 ms after the send: the probe goes then, as a probe.
    advance(209ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);
    auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(counters().tail_probes, 1U);
    EXPECT_EQ(counters().retransmits, 0U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open);
    // The RTO, armed from the probe, 210 ms after it, resends as a timeout.
    advance(209ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);
    resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, one[0].tcp->sequence);
    EXPECT_EQ(counters().retransmits, 1U);
    EXPECT_EQ(counters().tail_probes, 1U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Loss);
}

TEST_P(TcpTailProbeBrick, AfterAProbeTheRtoIsTheLastResort) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    Frames one;
    ASSERT_TRUE(send(*c, 1, one));
    advance(60ms);
    ASSERT_EQ(collect().size(), 1U) << "the probe";
    advance(209ms);
    EXPECT_TRUE(collect().empty()) << "the RTO runs from the probe: 210 ms";
    advance(1ms);
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, one[0].tcp->sequence);
    EXPECT_EQ(counters().retransmits, 1U);
    EXPECT_EQ(counters().tail_probes, 1U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Loss);
    EXPECT_EQ(c->rto(), 420ms) << "the RTO doubled, by its own expiry only";
    EXPECT_FALSE(c->tail_probe_in_flight()) << "a timeout forgets the probe";
    EXPECT_FALSE(c->tail_probe_verdict_open());
}

TEST_P(TcpTailProbeBrick, AProbeFoundNeedlessLeavesTheWindowAlone) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(send(*c, 1, sent));
    advance(60ms);
    const auto probe = collect();
    ASSERT_EQ(probe.size(), 1U);
    advance(10ms);
    receive(peer_.ack(probe[0]));  // the ACK of everything: the original's or the probe's
    EXPECT_FALSE(c->tail_probe_in_flight());
    EXPECT_TRUE(c->tail_probe_verdict_open()) << "an ACK on the probe's end does not say which arrived";
    // The peer had both: it reports the second copy as a duplicate, of one segment, ending at the probe's end.
    receive(peer_.sack(c->committed(), Blocks{range(0, 100)}));
    EXPECT_EQ(counters().duplicate_sacks_received, 1U);
    EXPECT_EQ(counters().tail_probes_needless, 1U);
    EXPECT_EQ(counters().tail_probe_recoveries, 0U);
    EXPECT_FALSE(c->tail_probe_verdict_open());
    EXPECT_EQ(c->congestion_window(), 14480U) << "the initial window, untouched";
}

TEST_P(TcpTailProbeBrick, AProbeThatRepairedALossCutsTheWindowOnce) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    Frames one;
    ASSERT_TRUE(send(*c, 1, one));
    advance(60ms);
    const auto probe = collect();
    ASSERT_EQ(probe.size(), 1U);
    advance(10ms);
    receive(peer_.ack(probe[0]));  // at the probe's end: the verdict stays open
    EXPECT_TRUE(c->tail_probe_verdict_open());
    EXPECT_EQ(c->congestion_window(), 14480U);
    EXPECT_EQ(counters().tail_probe_recoveries, 0U);
    Frames second;
    ASSERT_TRUE(send(*c, 1, second));
    advance(10ms);
    // Past the probe's end with no duplicate report: the probe repaired a loss. The policy is told of one congestion,
    // with the flight as a recovery's beginning reads it, after the ACK's cumulative part, and its window becomes its
    // target at once: max(0 / 2, 2 * 1448) = 2896; the 100 the ACK covered would give the same.
    receive(peer_.ack(second[0]));
    EXPECT_EQ(counters().tail_probe_recoveries, 1U);
    EXPECT_EQ(counters().tail_probes_needless, 0U);
    EXPECT_FALSE(c->tail_probe_verdict_open());
    EXPECT_EQ(c->congestion_state().ssthresh, 2896U);
    EXPECT_EQ(c->congestion_window(), 2896U);
    EXPECT_EQ(counters().recoveries, 0U) << "no episode";
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open);
    EXPECT_EQ(counters().fast_retransmits, 0U);
}

// RFC 8985 7.3's TLP_send_probe sends no probe while TLP.end_seq is set, and Linux's tcp_send_loss_probe returns at
// `tp->tlp_high_seq` the same way: the first probe's ACK lands on its end, so its verdict stays open, and the second
// order, lost too, gets no probe and waits for the RTO. Aloe sends one once the first probe's end is acknowledged and a
// sample was taken, closing the first verdict as a loss: the departure, counted in `tail_probes_early`.
TEST_P(TcpTailProbeBrick, TwoLoneSegmentsLostInARowAreBothProbed) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    Frames one;
    ASSERT_TRUE(send(*c, 1, one));
    advance(60ms);
    const auto first = collect();
    ASSERT_EQ(first.size(), 1U);
    advance(10ms);
    receive(peer_.ack(first[0]));  // echoes the probe's timestamp: a sample, 10 ms
    ASSERT_TRUE(c->tail_probe_verdict_open());
    ASSERT_FALSE(c->tail_probe_in_flight());
    ASSERT_TRUE(c->srtt().has_value());
    Frames two;
    ASSERT_TRUE(send(*c, 1, two));
    const auto wait = 2 * *c->srtt() + 40ms;
    advance(wait - 1ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);
    const auto second = collect();
    ASSERT_EQ(second.size(), 1U);
    EXPECT_EQ(second[0].tcp->sequence, two[0].tcp->sequence);
    EXPECT_EQ(counters().tail_probes, 2U);
    EXPECT_EQ(counters().tail_probes_early, 1U);
    EXPECT_EQ(counters().tail_probe_recoveries, 1U) << "the first verdict, closed as a loss";
    EXPECT_EQ(counters().retransmits, 0U);
    EXPECT_EQ(c->congestion_window(), 2896U) << "max(100 / 2, 2 * 1448): the first probe's cut, once";
    EXPECT_TRUE(c->tail_probe_in_flight());
    EXPECT_TRUE(c->tail_probe_verdict_open()) << "the second probe's";
}

TEST_P(TcpTailProbeBrick, WithoutTimestampsTheSecondOfTwoWaitsForTheRto) {
    peer_.set_timestamps(std::nullopt);
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->sack());
    ASSERT_FALSE(c->timestamps());
    Frames one;
    ASSERT_TRUE(send(*c, 1, one));
    advance(60ms);
    const auto first = collect();
    ASSERT_EQ(first.size(), 1U);
    advance(10ms);
    receive(peer_.ack(first[0]));  // at 80 ms: an ACK of a segment sent twice gives no sample (Karn)
    ASSERT_TRUE(c->tail_probe_verdict_open());
    Frames two;
    ASSERT_TRUE(send(*c, 1, two));  // into the empty queue: the RTO for 290 ms
    advance(60ms);
    EXPECT_TRUE(collect().empty()) << "no sample since the first probe: no second";
    EXPECT_EQ(counters().tail_probes, 1U);
    advance(149ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, two[0].tcp->sequence);
    EXPECT_EQ(counters().retransmits, 1U);
    EXPECT_EQ(counters().tail_probes_early, 0U);
    EXPECT_FALSE(c->tail_probe_verdict_open()) << "the timeout forgot it";
}

TEST_P(TcpTailProbeBrick, TheProbesAckThatReportsTheTailStartsRecoveryForTheRest) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(send(*c, 3, sent));  // at 10 ms; the first two will never arrive
    advance(20ms);
    const auto probe = collect();
    ASSERT_EQ(probe.size(), 1U);
    EXPECT_EQ(probe[0].tcp->sequence, at(200));
    advance(10ms);
    // At 40 ms the probe's ACK reports the third: RACK takes the probe, sent at 30 ms, and its round trip of 10 ms. One
    // segment reported, the reordering window is 2.5 ms, and the first two, sent at 10 ms, were due at 22.5 ms: lost.
    // Recovery begins; nothing is in flight and the ACK delivered 100, so PRR lets the first go at once.
    receive(peer_.sack(at(0), Blocks{range(200, 300)}));
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, at(0));
    EXPECT_EQ(counters().rack_marked, 2U);
    EXPECT_EQ(counters().recoveries, 1U);
    EXPECT_EQ(counters().retransmits, 0U);
    EXPECT_EQ(counters().tail_probes, 1U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Recovery);
    EXPECT_FALSE(c->tail_probe_in_flight()) << "a recovery forgets the probe";
    EXPECT_FALSE(c->tail_probe_verdict_open());
}

TEST_P(TcpTailProbeBrick, NoProbeWhileSomethingIsReportedOrInRecovery) {
    auto* c = open_and_send();  // five at 10 ms: a probe for 30 ms
    ASSERT_NE(c, nullptr);
    advance(10ms);
    // At 20 ms the third is reported: the second waits for the reordering timer, at 23 ms, and no probe is armed.
    receive(peer_.sack(at(100), Blocks{range(200, 300)}));
    advance(1ms);
    Frames sixth;
    ASSERT_TRUE(send(*c, 1, sixth));  // a send while something is reported leaves the reordering timer as it is
    advance(1ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, at(100));
    EXPECT_EQ(counters().reorder_timeouts, 1U);
    ASSERT_EQ(c->recovery_state(), RecoveryState::Recovery);
    // In recovery the timer is the RTO, from the head's resend at 23 ms: 233 ms.
    advance(7ms);
    EXPECT_TRUE(collect().empty()) << "nothing at 30 ms";
    advance(202ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);
    const auto timed_out = collect();
    ASSERT_EQ(timed_out.size(), 1U);
    EXPECT_EQ(timed_out[0].tcp->sequence, at(100));
    EXPECT_EQ(counters().retransmits, 1U);
    EXPECT_EQ(counters().tail_probes, 0U);
}

TEST_P(TcpTailProbeBrick, NoProbeWithoutSack) {
    peer_.set_sack_permitted(false);
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    ASSERT_FALSE(c->sack());
    Frames one;
    ASSERT_TRUE(send(*c, 1, one));
    advance(60ms);
    EXPECT_TRUE(collect().empty());
    advance(149ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);  // the RTO, 210 ms after the send
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, one[0].tcp->sequence);
    EXPECT_EQ(counters().retransmits, 1U);
    EXPECT_EQ(counters().tail_probes, 0U);
}

TEST_P(TcpTailProbeBrick, AFinThatIsTheTailIsProbed) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    Frames one;
    ASSERT_TRUE(send(*c, 1, one));
    advance(10ms);
    receive(peer_.ack(one[0]));
    ASSERT_EQ(c->queued_segments(), 0U);
    c->close();  // at 20 ms: the FIN alone in flight
    const auto fin = collect();
    ASSERT_EQ(fin.size(), 1U);
    ASSERT_TRUE(fin[0].tcp->flags.has(aloe::wire::TcpFlag::Fin));
    advance(59ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);
    const auto probe = collect();
    ASSERT_EQ(probe.size(), 1U);
    EXPECT_TRUE(probe[0].tcp->flags.has(aloe::wire::TcpFlag::Fin));
    EXPECT_EQ(probe[0].tcp->sequence, fin[0].tcp->sequence);
    EXPECT_EQ(counters().tail_probes, 1U);
    EXPECT_EQ(counters().retransmits, 0U);
}

TEST_P(TcpTailProbeBrick, AFinWaitingForTheWindowIsNotTheProbe) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    peer_.set_window(100);
    receive(peer_.ack());  // the peer's window: 100 bytes, which the data fills
    Frames one;
    ASSERT_TRUE(send(*c, 1, one));
    c->close();
    EXPECT_TRUE(collect().empty()) << "the FIN waits for the window";
    ASSERT_EQ(c->queued_segments(), 2U);
    advance(59ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);  // one segment transmitted: the allowance applies
    const auto probe = collect();
    ASSERT_EQ(probe.size(), 1U);
    EXPECT_FALSE(probe[0].tcp->flags.has(aloe::wire::TcpFlag::Fin));
    EXPECT_EQ(probe[0].tcp->sequence, one[0].tcp->sequence) << "the last segment transmitted, not the FIN";
    EXPECT_EQ(counters().tail_probes, 1U);
    advance(10ms);
    receive(peer_.ack(probe[0]));  // the data's ACK, one short of SND.NXT, which counts the FIN
    EXPECT_FALSE(c->tail_probe_in_flight()) << "the probe ends with the data it sent again, not at SND.NXT";
}

TEST_P(TcpTailProbeBrick, AProbeTakesNoRetry) {
    configure_tcp({.data_retries = 1});
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    Frames one;
    ASSERT_TRUE(send(*c, 1, one));
    advance(60ms);
    ASSERT_EQ(collect().size(), 1U) << "the probe, at 70 ms";
    EXPECT_EQ(c->retransmit_count(), 0U);
    advance(210ms);
    ASSERT_EQ(collect().size(), 1U) << "the one retry data_retries allows, at 280 ms";
    EXPECT_EQ(counters().retransmits, 1U);
    EXPECT_EQ(c->state(), aloe::tcp::State::Established);
    std::ignore = poll();
    advance(419ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);  // the backed-off RTO, 420 ms on: the cap
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.timed_out());
    EXPECT_EQ(counters().connections_timed_out, 1U);
    EXPECT_EQ(counters().tail_probes, 1U);
}

TEST_P(TcpTailProbeHeld, AProbeWhosePacketIsLentWaitsThenCopies) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    Frames one;
    ASSERT_TRUE(send(*c, 1, one));
    advance(60ms);
    // The probe's packet is still lent: it waits for the flush, and it is the probe from now on.
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(counters().retransmits_deferred, 1U);
    EXPECT_EQ(counters().tail_probes, 1U);
    EXPECT_TRUE(c->tail_probe_in_flight());
    advance(1ms);
    // The next flush: still lent, the deferral bound reached, the copy goes.
    const auto copy = collect();
    ASSERT_EQ(copy.size(), 1U);
    EXPECT_EQ(copy[0].tcp->sequence, one[0].tcp->sequence);
    EXPECT_TRUE(std::ranges::equal(aloe::frames::tcp_payload(copy[0]), aloe::frames::tcp_payload(one[0])));
    EXPECT_EQ(counters().retransmits_copied, 1U);
    EXPECT_EQ(counters().tail_probes, 1U);
    EXPECT_EQ(counters().retransmits, 0U);
    EXPECT_EQ(counters().fast_retransmits, 0U);
    const auto original = aloe::frames::parse_frame(tcp_->sent_segment(*c, 0).packet.data());
    ASSERT_TRUE(original.has_value());
    EXPECT_EQ(original->l4, one[0].l4) << "nothing was written into the packet the device held";
    advance(5ms);
    EXPECT_TRUE(collect().empty()) << "the probe left: the flush has nothing more to send";
    return_loans();
}

// A probe whose packet is still lent waits for the flush; when the original's ACK frees its entry first, the probe
// never left. It was not sent, so nothing of it is in flight and it has no verdict: the next ACK past its end repairs
// nothing and cuts no window. Linux sends no probe while the segment is in a host queue (`skb_still_in_host_queue`).
TEST_P(TcpTailProbeHeld, AProbeThatNeverLeftOpensNoVerdict) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    Frames one;
    ASSERT_TRUE(send(*c, 1, one));  // at 10 ms: the probe for 70
    advance(60ms);
    EXPECT_TRUE(collect().empty()) << "the probe's packet is still lent: it waits for the flush";
    ASSERT_EQ(counters().retransmits_deferred, 1U);
    ASSERT_TRUE(c->tail_probe_in_flight());
    advance(1ms);
    // At 71 ms, before the flush could send the copy, the original's ACK frees the entry the probe waits on.
    receive(peer_.ack(one[0]));
    EXPECT_FALSE(c->tail_probe_in_flight());
    EXPECT_FALSE(c->tail_probe_verdict_open()) << "a probe that never left has no verdict";
    EXPECT_EQ(counters().tail_probes, 0U) << "nor is it counted as sent";
    return_loans();
    EXPECT_TRUE(collect().empty()) << "nothing goes for the first segment once the loans return";
    Frames second;
    ASSERT_TRUE(send(*c, 1, second));
    advance(10ms);
    receive(peer_.ack(second[0]));  // past the first segment's end
    EXPECT_EQ(counters().tail_probe_recoveries, 0U);
    EXPECT_EQ(counters().tail_probes_needless, 0U);
    EXPECT_FALSE(c->tail_probe_verdict_open());
    EXPECT_EQ(c->congestion_window(), 14480U) << "the initial window, untouched";
    EXPECT_EQ(counters().retransmits_copied, 0U);
    return_loans();
}

TEST_P(TcpTailProbeRefusing, AProbeIpRefusesIsNoProbeAndTheRtoKeepsItsMoment) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    Frames one;
    ASSERT_TRUE(send(*c, 1, one));  // at 10 ms: the probe for 70, the RTO for 220
    refuse();
    advance(60ms);
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(counters().tail_probes, 0U) << "refused: no probe left";
    EXPECT_FALSE(c->tail_probe_in_flight());
    EXPECT_FALSE(c->tail_probe_verdict_open());
    allow();
    advance(149ms);
    EXPECT_TRUE(collect().empty()) << "the RTO keeps the moment it had";
    advance(1ms);
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, one[0].tcp->sequence);
    EXPECT_EQ(counters().retransmits, 1U);
}

TEST_P(TcpTailProbeBrick, ATerminalEventWithAProbeOutLeavesNothing) {
    // The packets of the stack's port that are free: each one taken, counted and given back.
    const auto free_packets = [this] {
        std::vector<aloe::fabric::Packet> taken;
        for (auto packet = port.allocate(0); packet; packet = port.allocate(0)) {
            taken.push_back(std::move(*packet));
        }
        return taken.size();
    };
    const std::size_t nodes   = tcp_->send_nodes_available();
    const std::size_t packets = free_packets();
    auto* c                   = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    const std::uint32_t slot = c->index();
    Frames one;
    ASSERT_TRUE(send(*c, 1, one));
    advance(60ms);
    ASSERT_EQ(collect().size(), 1U);
    ASSERT_TRUE(c->tail_probe_in_flight());
    Frames two;
    ASSERT_TRUE(send(*c, 1, two));  // the probe's timer armed again, for 90 ms, with the first probe out
    c->abort();
    ASSERT_EQ(c->state(), aloe::tcp::State::Closed);
    std::ignore = collect();  // the RST
    advance(300ms);
    EXPECT_TRUE(collect().empty()) << "nothing fires";
    EXPECT_EQ(counters().retransmits, 0U);
    EXPECT_EQ(counters().tail_probes, 1U);
    c->release();
    EXPECT_EQ(tcp_->send_nodes_available(), nodes);
    EXPECT_EQ(free_packets(), packets);
    aloe::frames::TcpPeer next{aloe::testing::peer_spec(aloe::testing::tcp_peer_port + 1), TcpSequence{5000}};
    next.set_timestamps(100);
    next.set_sack_permitted(true);
    auto* fresh = open_passive(next, 10ms);
    ASSERT_NE(fresh, nullptr);
    EXPECT_EQ(fresh->index(), slot);
    EXPECT_FALSE(fresh->tail_probe_in_flight());
    EXPECT_FALSE(fresh->tail_probe_verdict_open());
}

TEST_P(TcpTailProbeBrick, ASecondOrderSentWhileTheFirstIsOutSchedulesTheProbeAgain) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    Frames first;
    ASSERT_TRUE(send(*c, 1, first));  // at 10 ms: a probe for 70 ms
    advance(10ms);
    Frames second;
    ASSERT_TRUE(send(*c, 1, second));  // at 20 ms: two in flight, the probe for 20 + 20 = 40 ms
    advance(19ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);
    const auto probe = collect();
    ASSERT_EQ(probe.size(), 1U);
    EXPECT_EQ(probe[0].tcp->sequence, second[0].tcp->sequence);
    EXPECT_EQ(counters().tail_probes, 1U);
}

// Linux flags a probe as ever resent (`TCPCB_EVER_RETRANS`) and not as a resend that is out (`TCPCB_SACKED_RETRANS`,
// which `tcp_mark_skb_lost` counts as a lost retransmission): RACK marking a probed segment later finds a first loss,
// while Karn's rule and RACK's test of an ACK that cannot be for the resend still see it sent twice.
TEST_P(TcpTailProbeBrick, AProbedSegmentRackMarksLaterIsAFirstLoss) {
    auto* c = open_and_send();  // five at 10 ms
    ASSERT_NE(c, nullptr);
    advance(20ms);
    ASSERT_EQ(collect().size(), 1U) << "the fifth, probed at 30 ms";
    Frames more;
    ASSERT_TRUE(send(*c, 3, more));  // after the probe, at the same stamp
    advance(10ms);
    // At 40 ms the three new segments are reported: the reordering window is zero, RACK's segment is the last of them
    // and its round trip 10 ms, so all five before it are lost, the probed fifth too, sent at 30 + 10 <= 40 ms.
    receive(peer_.sack(at(0), Blocks{range(500, 800)}));
    EXPECT_EQ(counters().rack_marked, 5U);
    EXPECT_EQ(counters().lost_retransmits, 0U) << "a probe is not a resend that is out";
    EXPECT_EQ(tcp_->sent_segment(*c, 4).transmits, 2U) << "sent twice all the same";
    EXPECT_EQ(counters().recoveries, 1U);
}

// The probe's own duplicate report says both copies arrived, which is no evidence of reordering: RACK's reordering
// window does not grow for it, as Linux's `tcp_dsack_seen` sets `rack.dsack_seen` only when it is not `FLAG_DSACK_TLP`.
TEST_P(TcpTailProbeBrick, TheProbesDuplicateReportGrowsNoReorderingWindow) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(send(*c, 3, sent));
    advance(10ms);
    // At 20 ms the third is reported, then the second arrives late: reordering seen, and the window a quarter of the
    // minimum round trip, 2.5 ms. Both ACKs sample 10 ms, so srtt stays 10 ms.
    receive(peer_.sack(at(100), Blocks{range(200, 300)}));
    receive(ack_of(at(300)));
    ASSERT_EQ(counters().reordering_seen, 1U);
    ASSERT_EQ(c->reordering_window(), 2500us);
    Frames one;
    ASSERT_TRUE(send(*c, 1, one));
    advance(60ms);
    const auto probe = collect();
    ASSERT_EQ(probe.size(), 1U);
    advance(10ms);
    receive(peer_.ack(probe[0]));  // its echo samples 10 ms: the minimum round trip stays 10 ms
    receive(peer_.sack(c->committed(), Blocks{range(300, 400)}));
    EXPECT_EQ(counters().duplicate_sacks_received, 1U);
    EXPECT_EQ(counters().tail_probes_needless, 1U);
    EXPECT_EQ(c->reordering_window(), 2500us) << "any other duplicate report would have grown it to 5 ms";
}

// RFC 8985 7.4.2 reads a duplicate report matching the probe's end with no test of belief, and Linux's `tcp_dsack_seen`
// sets `FLAG_DSACK_TLP` before its count test: the probe's report closes its verdict as needless even when the count of
// duplicates reported has run ahead of the resends, here by a duplicate of the first order that the network made.
TEST_P(TcpTailProbeBrick, TheProbesDuplicateReportClosesItsVerdictBelievedOrNot) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(send(*c, 1, sent));  // at 10 ms
    advance(10ms);
    // At 20 ms the ACK of the first order reports it twice, though nothing was resent: one duplicate counted against no
    // resend, so not believed.
    receive(peer_.sack(at(100), Blocks{range(0, 100)}));
    ASSERT_EQ(c->acknowledged(), at(100));
    EXPECT_EQ(counters().duplicate_sacks_received, 0U);
    Frames second;
    ASSERT_TRUE(send(*c, 1, second));  // at 20 ms, alone: probed at 20 + 2 * 10 + 40 = 80
    advance(60ms);
    const auto probe = collect();
    ASSERT_EQ(probe.size(), 1U);
    advance(10ms);
    receive(peer_.ack(probe[0]));  // on the probe's end: open
    ASSERT_TRUE(c->tail_probe_verdict_open());
    // Both copies arrived, and the peer reports the second: two duplicates counted against one resend, not believed,
    // and the probe's all the same.
    receive(peer_.sack(c->committed(), Blocks{range(100, 200)}));
    EXPECT_EQ(counters().duplicate_sacks_received, 0U);
    EXPECT_EQ(counters().tail_probes_needless, 1U);
    EXPECT_FALSE(c->tail_probe_verdict_open());
    Frames third;
    ASSERT_TRUE(send(*c, 1, third));
    advance(10ms);
    receive(peer_.ack(third[0]));  // past the probe's end: nothing is left to repair
    EXPECT_EQ(counters().tail_probe_recoveries, 0U);
    EXPECT_EQ(c->congestion_window(), 14480U);
}

// Linux's first test of a duplicate report stands: one longer than the largest window the peer ever offered is no
// report at all (`tcp_dsack_seen`), and the probe's verdict reads nothing of it.
TEST_P(TcpTailProbeBrick, AReportLongerThanTheLargestWindowClosesNothing) {
    peer_.set_window(100);  // every window the peer states, its SYN's too: 100 bytes
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(send(*c, 1, sent));  // at 10 ms: probed at 70
    advance(60ms);
    const auto probe = collect();
    ASSERT_EQ(probe.size(), 1U);
    advance(10ms);
    receive(peer_.ack(probe[0]));
    // 101 bytes ending at the probe's end: within one segment, and past the 100 the peer ever offered.
    const aloe::wire::TcpSackBlock too_long{.first = TcpSequence{at(0).value - 1U}, .end = at(100)};
    receive(peer_.sack(c->committed(), Blocks{too_long}));
    EXPECT_EQ(counters().tail_probes_needless, 0U);
    EXPECT_TRUE(c->tail_probe_verdict_open());
    Frames second;
    ASSERT_TRUE(send(*c, 1, second));
    advance(10ms);
    receive(peer_.ack(second[0]));  // past the probe's end, the verdict still open: a loss repaired
    EXPECT_EQ(counters().tail_probe_recoveries, 1U);
    EXPECT_EQ(c->congestion_window(), 2896U);
}

// The ACK that ends an episode can leave RACK waiting with nothing reported: it acknowledges a resend sent after an
// order above `recover`, which then waits RACK's window. A send meets a reordering timer pending while a probe may be
// scheduled, and the order of the spec's last step stands: the reordering timer, when RACK asked for one, before the
// probe. The recorder's window is unbounded, so every order and resend goes when it is due.
TEST_P(TcpTailProbeRecorded, ASendKeepsAPendingReorderingTimer) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(send(*c, 3, sent));  // the first, second and third at 10 ms
    advance(3ms);
    Frames later;
    ASSERT_TRUE(send(*c, 2, later));  // the fourth and fifth at 13 ms
    advance(7ms);
    // At 20 ms the second is reported, then the first arrives late: reordering is seen, so RACK's window is a quarter
    // of the minimum round trip, 2.5 ms, in recovery too. Both ACKs sample 10 ms.
    receive(peer_.sack(at(0), Blocks{range(100, 200)}));
    receive(ack_of(at(200)));
    ASSERT_EQ(counters().reordering_seen, 1U);
    advance(10ms);
    // At 30 ms the fifth is reported: RACK's round trip 30 - 13 = 17 ms. The third, sent at 10 ms, was due at
    // 10 + 17 + 2.5 = 29.5: lost, and recovery begins, `recover` at the fifth's end; the fourth, sent at 13 ms below
    // the fifth, waits until 32.5 ms.
    receive(peer_.sack(at(200), Blocks{range(400, 500)}));
    ASSERT_EQ(c->recovery_state(), RecoveryState::Recovery);
    ASSERT_EQ(collect().size(), 1U) << "the third";
    advance(1ms);
    Frames order;
    ASSERT_TRUE(send(*c, 1, order));  // the sixth at 31 ms, above `recover`
    advance(2ms);
    const auto fourth = collect();  // the reordering timer at 33 ms marks the fourth
    ASSERT_EQ(fourth.size(), 1U);
    ASSERT_EQ(fourth[0].tcp->sequence, at(300));
    advance(10ms);
    // At 43 ms the ACK of everything below the sixth echoes the fourth's resend and ends the episode. RACK takes that
    // resend, sent at 33 ms, and its round trip of 10 ms; the sixth, sent at 31 ms before it and reported by nothing,
    // waits until 31 + 10 + 2.5 = 43.5 ms, 44 on the wheel. Open, nothing reported: a probe could be scheduled.
    peer_.see(fourth[0]);
    receive(ack_of(at(500)));
    ASSERT_EQ(c->recovery_state(), RecoveryState::Open);
    ASSERT_EQ(c->reported_bytes(), 0U);
    Frames next;
    ASSERT_TRUE(send(*c, 1, next));  // the seventh at 43 ms
    advance(1ms);
    const auto marked = collect();
    ASSERT_EQ(marked.size(), 1U) << "the sixth, by the reordering timer, which the send left pending";
    EXPECT_EQ(marked[0].tcp->sequence, at(500));
    EXPECT_EQ(counters().reorder_timeouts, 2U);
    EXPECT_EQ(counters().tail_probes, 0U);
}

// The user timeout counts from the head's first transmission, and a probe is no retry: with one between, a connection
// times out at the same stamp as one to a peer without SACK, whose timer alone runs.
TEST_P(TcpTailProbeBrick, TheUserTimeoutIsUnmovedByAProbe) {
    configure_tcp({.user_timeout = 500ms});
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    aloe::frames::TcpPeer plain{aloe::testing::peer_spec(aloe::testing::tcp_peer_port + 1), TcpSequence{5000}};
    plain.set_timestamps(100);
    auto* timer_alone = open_passive(plain, 10ms);
    ASSERT_NE(timer_alone, nullptr);
    ASSERT_FALSE(timer_alone->sack());
    ASSERT_EQ(c->send(aloe::frames::pattern(100)), 100U);  // both at 20 ms: the deadline is 20 + 500 = 520
    ASSERT_EQ(timer_alone->send(aloe::frames::pattern(100)), 100U);
    ASSERT_EQ(collect().size(), 2U);
    advance(60ms);
    ASSERT_EQ(collect().size(), 1U) << "the probe at 80 ms; the RTO now runs from it, to 290";
    advance(150ms);
    ASSERT_EQ(collect().size(), 1U) << "the other's RTO at 230 ms";
    advance(60ms);
    ASSERT_EQ(collect().size(), 1U) << "this one's at 290 ms";
    EXPECT_EQ(counters().tail_probes, 1U);
    EXPECT_EQ(counters().retransmits, 2U);
    // Each RTO backed off to 420 ms, past the deadline, and is clamped to it.
    advance(229ms);
    EXPECT_EQ(c->state(), aloe::tcp::State::Established);
    EXPECT_EQ(timer_alone->state(), aloe::tcp::State::Established);
    advance(1ms);
    EXPECT_EQ(c->state(), aloe::tcp::State::Closed);
    EXPECT_EQ(timer_alone->state(), aloe::tcp::State::Closed);
    EXPECT_EQ(counters().connections_user_timed_out, 2U);
}

// A reordering expiry is no retry either: the deadline stays the head's first transmission plus the user timeout,
// 10 + 500 = 510 ms, where a peer without SACK would have its timer alone time the connection out too.
TEST_P(TcpTailProbeBrick, TheUserTimeoutIsUnmovedByAReorderingExpiry) {
    configure_tcp({.user_timeout = 500ms});
    auto* c = open_and_send();  // five at 10 ms
    ASSERT_NE(c, nullptr);
    advance(10ms);
    receive(peer_.sack(at(100), Blocks{range(200, 300)}));  // the second waits for the reordering timer, at 23 ms
    advance(3ms);
    ASSERT_EQ(collect().size(), 1U);
    EXPECT_EQ(counters().reorder_timeouts, 1U);
    EXPECT_EQ(c->retransmit_count(), 0U);
    advance(210ms);  // the RTO from the head's resend at 23 ms
    ASSERT_EQ(collect().size(), 1U);
    EXPECT_EQ(counters().retransmits, 1U);
    advance(276ms);  // backed off to 420 ms, past the deadline, and clamped to it
    EXPECT_EQ(c->state(), aloe::tcp::State::Established);
    advance(1ms);
    EXPECT_EQ(c->state(), aloe::tcp::State::Closed);
    EXPECT_EQ(counters().connections_user_timed_out, 1U);
}

// The user timeout bounds the probe through the RTO's moment, which the probe's timer never passes: a probe due at the
// deadline does not go, and the connection times out there.
TEST_P(TcpTailProbeBrick, AProbeDueAtTheUserTimeoutTimesTheConnectionOut) {
    configure_tcp({.user_timeout = 50ms});
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    Frames one;
    ASSERT_TRUE(send(*c, 1, one));  // at 10 ms: the RTO's moment clamped to 10 + 50 = 60, the probe's, 70, to it
    advance(49ms);
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(c->state(), aloe::tcp::State::Established);
    advance(1ms);
    EXPECT_TRUE(collect().empty()) << "no probe at the deadline";
    EXPECT_EQ(c->state(), aloe::tcp::State::Closed);
    EXPECT_EQ(counters().tail_probes, 0U);
    EXPECT_EQ(counters().connections_user_timed_out, 1U);
}

// The same for the reordering timer: clamped to the deadline, its expiry there times the connection out.
TEST_P(TcpTailProbeBrick, AReorderingExpiryAtTheUserTimeoutTimesTheConnectionOut) {
    configure_tcp({.user_timeout = 12ms});
    auto* c = open_and_send();  // five at 10 ms: the deadline is 22 ms
    ASSERT_NE(c, nullptr);
    advance(10ms);
    // At 20 ms the third is reported: the second would wait until 22.5 ms, past the deadline, which bounds the timer.
    receive(peer_.sack(at(100), Blocks{range(200, 300)}));
    advance(1ms);
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(c->state(), aloe::tcp::State::Established);
    advance(1ms);
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(c->state(), aloe::tcp::State::Closed);
    EXPECT_EQ(counters().reorder_timeouts, 0U);
    EXPECT_EQ(counters().connections_user_timed_out, 1U);
}
