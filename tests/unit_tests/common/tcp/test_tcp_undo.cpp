#include <aloe/core>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>
#include <tcp_sack_brick.hpp>

// The undo of a needless reduction: the record `LossRecovery` keeps from the start of an episode (SND.UNA then, the
// policy's state then, the first resend's stamp, the resends that may still be reported), its two signs, and what an
// undo takes back; then the brick: the duplicate report that undoes after the episode, the ACK that answers an original
// at the end of recovery, on a partial ACK and in the loss state, and what is left to detection afterwards. The
// record's tests are at an MSS of 1000, where NewReno's window is 10000, in recovery with 10000 outstanding from 1001:
// the episode starts at 1001 and the target is max(10000 / 2, 2 * 1000) = 5000. The brick's peer offers timestamps and
// SACK-permitted, so a segment carries 1448 bytes and NewReno's initial window is 14480, and its handshake takes 10 ms:
// srtt 10 ms, an RTO of 210 ms, a minimum round trip of 10 ms. The sends follow the handshake at 10 ms; a resend in a
// later millisecond carries a later timestamp than its original, one in the same millisecond the same.
namespace {

    using namespace std::chrono_literals;
    using aloe::tcp::RecoveryState;
    using aloe::wire::TcpSequence;
    using Blocks             = std::vector<aloe::wire::TcpSackBlock>;
    using Frames             = std::vector<aloe::frames::ParsedFrame>;
    using Recovery           = aloe::tcp::detail::LossRecovery<aloe::tcp::NewReno>;
    using TimePoint          = aloe::core::TimePoint;
    constexpr auto unbounded = std::numeric_limits<std::uint32_t>::max();

    /// The stamp `count` milliseconds after the epoch.
    [[nodiscard]] TimePoint ms(const std::int64_t count) {
        return TimePoint{} + std::chrono::milliseconds{count};
    }

    /// Established at an MSS of 1000, and in recovery with 10000 outstanding from 1001 to 11000.
    [[nodiscard]] Recovery in_recovery() {
        Recovery recovery;
        recovery.open(TcpSequence{1000});
        recovery.establish(1000);
        recovery.begin_recovery(TcpSequence{11001}, 10000, 10000, 1000);
        return recovery;
    }

    /// The brick tests' shared helpers (`aloe::testing::SackBrick`), and the loss of the head that most of them begin
    /// with.
    template <typename Fixture>
    class UndoBrick : public aloe::testing::SackBrick<Fixture> {
    protected:
        using Connection = aloe::testing::SackBrick<Fixture>::Connection;

        /**
         * Five sends at 10 ms; `wait` later, an ACK of none of them whose block reports the second to the fourth.
         * Three reported and no reordering seen make RACK's window zero, so the head, sent before them, is lost on that
         * ACK. Recovery begins with a target of max(500 / 2, 2 * 1448) = 2896; of 500 outstanding 300 are reported and
         * 100 lost, so 100 is in flight, and PRR allows the 300 the block delivered: the head is resent at once, into
         * `resend`. Null, with a failure added, when it did not go so.
         */
        [[nodiscard]] Connection* mark_the_head(const aloe::core::Duration wait, Frames& resend) {
            Connection* c = this->open_and_send();
            if (c == nullptr) {
                return nullptr;
            }
            if (wait > aloe::core::Duration::zero()) {
                this->advance(wait);
            }
            this->receive(this->peer_.sack(this->at(0), Blocks{this->range(100, 400)}));
            resend = this->collect();
            if (resend.size() != 1 || resend[0].tcp->sequence != this->at(0) ||
                c->recovery_state() != RecoveryState::Recovery) {
                ADD_FAILURE() << "expected the head resent in recovery, got " << resend.size() << " frames";
                return nullptr;
            }
            return c;
        }
    };

    using TcpUndoBrick = UndoBrick<aloe::testing::TcpFixture>;
    using TcpUndoHeld  = UndoBrick<aloe::testing::TcpHeldFixture>;

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpUndoBrick,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpUndoHeld,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

}  // namespace

TEST(TcpUndo, EveryResendReportedIsTheFirstSign) {
    Recovery recovery = in_recovery();
    ASSERT_TRUE(recovery.undo_open());
    recovery.on_resend(ms(20));
    recovery.on_resend(ms(21));
    recovery.on_duplicate_report(TcpSequence{2001});
    EXPECT_FALSE(recovery.every_resend_reported()) << "one of two";
    recovery.on_duplicate_report(TcpSequence{3001});
    EXPECT_TRUE(recovery.every_resend_reported());
}

TEST(TcpUndo, NoResendCountedIsNotTheFirstSign) {
    Recovery recovery = in_recovery();
    EXPECT_FALSE(recovery.every_resend_reported()) << "nothing resent: no count to reach zero";
    recovery.on_duplicate_report(TcpSequence{2001});
    EXPECT_FALSE(recovery.every_resend_reported()) << "a report with nothing counted brings nothing to zero";
}

TEST(TcpUndo, AnEchoOlderThanTheFirstResendAnswersAnOriginal) {
    Recovery recovery = in_recovery();
    EXPECT_FALSE(recovery.first_resend().has_value());
    EXPECT_TRUE(recovery.answers_original(50U, 60U)) << "nothing of the episode resent yet: whatever the echo";
    EXPECT_TRUE(recovery.answers_original(70U, 60U));
    EXPECT_TRUE(recovery.answers_original(std::nullopt, std::nullopt));
    recovery.on_resend(ms(60));
    recovery.on_resend(ms(70));
    EXPECT_EQ(recovery.first_resend(), ms(60)) << "the first resend's stamp is kept";
    EXPECT_TRUE(recovery.answers_original(50U, 60U));
    EXPECT_FALSE(recovery.answers_original(60U, 60U)) << "an original in the first resend's millisecond: no sign";
    EXPECT_FALSE(recovery.answers_original(std::nullopt, 60U)) << "no echo";
    EXPECT_FALSE(recovery.answers_original(50U, std::nullopt)) << "no timestamps";
    EXPECT_TRUE(recovery.answers_original(0xfffffff0U, 0x10U)) << "older in serial arithmetic, across the wrap";
}

TEST(TcpUndo, ADuplicateReportBelowTheEpisodesStartCountsNothing) {
    Recovery recovery = in_recovery();  // the episode starts at 1001
    recovery.on_resend(ms(20));
    recovery.on_duplicate_report(TcpSequence{1001});
    EXPECT_FALSE(recovery.every_resend_reported()) << "a report that ends at the start is of data before the episode";
    recovery.on_duplicate_report(TcpSequence{900});
    EXPECT_FALSE(recovery.every_resend_reported());
    recovery.on_duplicate_report(TcpSequence{1002});
    EXPECT_TRUE(recovery.every_resend_reported());
}

TEST(TcpUndo, UndoRestoresThePolicyAndClosesOnce) {
    Recovery recovery = in_recovery();
    ASSERT_EQ(recovery.congestion().ssthresh, 5000U);
    recovery.undo(true);
    EXPECT_EQ(recovery.window(), 10000U);
    EXPECT_EQ(recovery.congestion().ssthresh, unbounded) << "the target copied at the start: none yet";
    EXPECT_EQ(recovery.state(), RecoveryState::Open);
    EXPECT_FALSE(recovery.undo_open());
    // A probe repairs a loss, which opens no undo: the window is cut to max(10000 / 2, 2000), and the closed record
    // restores nothing.
    recovery.on_repaired(10000, 1000);
    ASSERT_EQ(recovery.window(), 5000U);
    recovery.undo(false);
    EXPECT_EQ(recovery.window(), 5000U) << "an episode is undone once";
}

TEST(TcpUndo, TheRecordSurvivesTheEndOfTheEpisode) {
    Recovery recovery = in_recovery();
    recovery.on_resend(ms(20));
    recovery.end(TcpSequence{11001});
    ASSERT_EQ(recovery.window(), 5000U) << "the episode ended at its target";
    EXPECT_TRUE(recovery.undo_open()) << "the reports trail the resends";
    recovery.on_duplicate_report(TcpSequence{2001});
    EXPECT_TRUE(recovery.every_resend_reported());
    recovery.undo(false);
    EXPECT_EQ(recovery.window(), 10000U);
    EXPECT_EQ(recovery.state(), RecoveryState::Open);
    EXPECT_FALSE(recovery.undo_open());
}

TEST(TcpUndo, AProbeInFlightIsOneMoreResend) {
    Recovery recovery = in_recovery();
    recovery.count_probe();
    EXPECT_FALSE(recovery.first_resend().has_value()) << "a probe is no resend of the episode";
    recovery.on_resend(ms(20));
    recovery.on_duplicate_report(TcpSequence{2001});
    EXPECT_FALSE(recovery.every_resend_reported()) << "the probe's copy is still to be reported";
    recovery.on_duplicate_report(TcpSequence{11001});
    EXPECT_TRUE(recovery.every_resend_reported());

    Recovery alone = in_recovery();
    alone.count_probe();
    EXPECT_FALSE(alone.every_resend_reported());
    alone.on_duplicate_report(TcpSequence{11001});
    EXPECT_TRUE(alone.every_resend_reported()) << "a probe alone, reported, is the first sign too";
}

TEST(TcpUndo, ANewEpisodeReplacesTheRecord) {
    Recovery recovery = in_recovery();
    recovery.on_resend(ms(20));
    recovery.end(TcpSequence{11001});
    ASSERT_EQ(recovery.window(), 5000U);
    // A timeout from the open state sets a new target, so it opens a new record, from SND.UNA at 11001.
    recovery.on_timeout(TcpSequence{11001}, TcpSequence{14001}, 3000, 0, 1000, true);
    EXPECT_TRUE(recovery.undo_open());
    EXPECT_FALSE(recovery.first_resend().has_value());
    EXPECT_FALSE(recovery.every_resend_reported()) << "the first episode's count is gone";
    recovery.on_resend(ms(300));
    recovery.on_duplicate_report(TcpSequence{2001});
    EXPECT_FALSE(recovery.every_resend_reported()) << "a report of the first episode's data is below the new start";
    recovery.on_duplicate_report(TcpSequence{12001});
    EXPECT_TRUE(recovery.every_resend_reported());
    recovery.undo(true);
    EXPECT_EQ(recovery.window(), 5000U) << "the state copied at the timeout: the first episode's target";
}

TEST(TcpUndo, ALaterExpiryInTheLossStateKeepsTheRecord) {
    Recovery recovery;
    recovery.open(TcpSequence{1000});
    recovery.establish(1000);
    recovery.on_timeout(TcpSequence{1001}, TcpSequence{11001}, 10000, 0, 1000, true);
    recovery.on_resend(ms(200));
    // The next expiry with no ACK of new data between: the window was reduced already, so no new target and no new
    // record; its resend counts in the first expiry's.
    recovery.on_timeout(TcpSequence{1001}, TcpSequence{11001}, 10000, 0, 1000, false);
    recovery.on_resend(ms(600));
    EXPECT_EQ(recovery.first_resend(), ms(200));
    recovery.on_duplicate_report(TcpSequence{2001});
    EXPECT_FALSE(recovery.every_resend_reported());
    recovery.on_duplicate_report(TcpSequence{2001});
    EXPECT_TRUE(recovery.every_resend_reported());
    recovery.undo(true);
    EXPECT_EQ(recovery.window(), 10000U) << "the state copied at the first expiry";
}

// Linux's tcp_end_cwnd_reduction sets the window to the target only for an episode that was not undone: here the
// undo made the target unbounded again, and the window would follow it.
TEST(TcpUndo, AnEpisodeUndoneBeforeItsEndKeepsItsWindowAtTheEnd) {
    Recovery recovery = in_recovery();
    recovery.undo(false);
    ASSERT_EQ(recovery.state(), RecoveryState::Recovery);
    recovery.end(TcpSequence{11001});
    EXPECT_EQ(recovery.window(), 10000U);
    EXPECT_EQ(recovery.state(), RecoveryState::Open);
}

// RFC 6582's guard after an undo that reopens short of `recover`: it stays where the episode set it, so duplicates the
// needless resends draw begin no recovery before an ACK covers more than it, and an ACK of new data short of it does
// not move it back.
TEST(TcpUndo, AReopenKeepsTheGuardOfTheEpisode) {
    Recovery recovery = in_recovery();  // recover is 11000
    std::ignore       = recovery.count_duplicate();
    recovery.undo(true);
    EXPECT_EQ(recovery.duplicates(), 0U) << "the duplicate count clears with the episode";
    EXPECT_FALSE(recovery.admits(TcpSequence{5001}));
    recovery.on_ack(aloe::tcp::AckInfo{.acked = 4000, .flight = 10000, .mss = 1000}, TcpSequence{5001});
    EXPECT_FALSE(recovery.admits(TcpSequence{5001})) << "an ACK of new data short of recover leaves the guard";
    EXPECT_TRUE(recovery.admits(TcpSequence{11002}));
    recovery.on_ack(aloe::tcp::AckInfo{.acked = 7001, .flight = 6000, .mss = 1000}, TcpSequence{12002});
    EXPECT_TRUE(recovery.admits(TcpSequence{12002})) << "past it the guard slides with each ACK, as before";
}

// The resend arrived first: the ACK of everything echoes it and answers no original, so recovery ends at its target.
// The late original then reaches the peer, which reports it as a duplicate: the one resend is reported, and the window
// comes back.
TEST_P(TcpUndoBrick, ANeedlessResendGivesTheWindowBackOnItsDuplicateReport) {
    Frames resend;
    auto* c = mark_the_head(10ms, resend);  // at 20 ms
    ASSERT_NE(c, nullptr);
    advance(10ms);
    peer_.see(resend[0]);
    receive(ack_of(at(500)));
    ASSERT_EQ(c->recovery_state(), RecoveryState::Open);
    EXPECT_EQ(c->congestion_window(), 2896U);
    EXPECT_EQ(counters().undos_original_ack, 0U);
    advance(5ms);
    receive(peer_.sack(c->committed(), Blocks{range(0, 100)}));
    EXPECT_EQ(counters().duplicate_sacks_received, 1U);
    EXPECT_EQ(counters().undos_duplicate_sack, 1U);
    EXPECT_EQ(counters().undos_original_ack, 0U);
    EXPECT_EQ(c->congestion_window(), 14480U);
    EXPECT_EQ(c->congestion_state().ssthresh, unbounded);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open);
}

// The timestamp clock ticks in milliseconds: a resend in its original's millisecond carries the original's value, so
// the ACK the original draws cannot be told from the resend's, and no undo follows from it (spec, "Undo"). The
// duplicate report of the resend is what gives the window back.
TEST_P(TcpUndoBrick, AnOriginalAndItsResendInOneMillisecondAreNotToldApart) {
    Frames resend;
    auto* c = mark_the_head(0ms, resend);  // at 10 ms, the originals' millisecond
    ASSERT_NE(c, nullptr);
    advance(10ms);
    peer_.see(sent[0]);  // the original arrived: its ACK echoes the original's timestamp
    receive(ack_of(at(500)));
    EXPECT_EQ(counters().undos_original_ack, 0U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open);
    EXPECT_EQ(c->congestion_window(), 2896U) << "the target, as for a loss";
    EXPECT_EQ(c->congestion_state().ssthresh, 2896U);
    advance(5ms);
    receive(peer_.sack(c->committed(), Blocks{range(0, 100)}));
    EXPECT_EQ(counters().undos_duplicate_sack, 1U);
    EXPECT_EQ(c->congestion_window(), 14480U);
}

TEST_P(TcpUndoBrick, AnAckThatAnswersTheOriginalUndoesAtTheEndOfRecovery) {
    Frames resend;
    auto* c = mark_the_head(10ms, resend);  // the resend at 20 ms
    ASSERT_NE(c, nullptr);
    advance(10ms);
    // The original was only late: the ACK of everything echoes its timestamp, 10 ms, older than the resend's, and
    // covers `recover`. The window is restored in place of the target.
    peer_.see(sent[0]);
    receive(ack_of(at(500)));
    EXPECT_EQ(counters().undos_original_ack, 1U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open);
    EXPECT_EQ(c->congestion_window(), 14480U);
    EXPECT_EQ(c->congestion_state().ssthresh, unbounded);
    EXPECT_EQ(counters().reordering_seen, 0U) << "the partial ACK records reordering, not the one that ends recovery";
    EXPECT_EQ(counters().recoveries, 1U);
    // The resend reaches the peer too and is reported as a duplicate: believed, but the episode is undone already.
    advance(5ms);
    receive(peer_.sack(c->committed(), Blocks{range(0, 100)}));
    EXPECT_EQ(counters().duplicate_sacks_received, 1U);
    EXPECT_EQ(counters().undos_duplicate_sack, 0U);
    EXPECT_EQ(c->congestion_window(), 14480U);
}

TEST_P(TcpUndoBrick, ARealLossIsNotUndone) {
    Frames resend;
    auto* c = mark_the_head(10ms, resend);  // the resend at 20 ms
    ASSERT_NE(c, nullptr);
    advance(10ms);
    peer_.see(resend[0]);  // the original was lost: the ACK of everything echoes the resend
    receive(ack_of(at(500)));
    EXPECT_EQ(counters().undos_original_ack, 0U);
    EXPECT_EQ(counters().undos_duplicate_sack, 0U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open);
    EXPECT_EQ(c->congestion_window(), 2896U) << "the target";
    EXPECT_EQ(c->congestion_state().ssthresh, 2896U);
}

TEST_P(TcpUndoBrick, APartialAckThatAnswersAnOriginalRecordsReorderingAndWaitsForTheResend) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(send(*c, 8, sent));  // at 10 ms
    advance(10ms);
    // At 20 ms the fifth to the seventh are reported: the window is zero and the first four, due at 10 + 10 = 20 ms,
    // are lost. The target is max(800 / 2, 2 * 1448) = 2896; of 800 outstanding 300 are reported and 400 lost, so 100
    // is in flight, and PRR allows the 300 delivered: three resends leave and the fourth waits for the window.
    receive(peer_.sack(at(0), Blocks{range(400, 700)}));
    ASSERT_EQ(collect().size(), 3U);
    ASSERT_EQ(c->lost_bytes(), 100U);
    // The first original arrives; the ACK echoes its timestamp, older than the resends'. Reordering is recorded, but
    // the second and third resends are out: nothing more is marked or resent on this ACK, though PRR's step leaves room
    // for the fourth. PRR: 400 delivered, 300 sent, 300 in flight below the target, so max(400 - 300, 100) plus a
    // segment for a safe ACK, 1548; the window is 300 + 1548 = 1848.
    peer_.see(sent[0]);
    receive(ack_of(at(100)));
    EXPECT_EQ(counters().reordering_seen, 1U);
    EXPECT_EQ(counters().undos_original_ack, 0U);
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(c->lost_bytes(), 100U);
    EXPECT_TRUE(tcp_->sent_segment(*c, 2).lost) << "the fourth";
    EXPECT_EQ(counters().rack_marked, 4U);
    EXPECT_EQ(counters().fast_retransmits, 3U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Recovery);
    EXPECT_EQ(c->congestion_window(), 1848U);
    EXPECT_EQ(c->congestion_state().ssthresh, 2896U);
    // The second and third originals: no resend is out any more. The undo clears the fourth's mark and opens.
    receive(ack_of(at(300)));
    EXPECT_EQ(counters().undos_original_ack, 1U);
    EXPECT_EQ(counters().reordering_seen, 2U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open);
    EXPECT_EQ(c->lost_bytes(), 0U);
    EXPECT_FALSE(tcp_->sent_segment(*c, 0).lost);
    EXPECT_EQ(c->congestion_window(), 14480U);
    EXPECT_EQ(c->congestion_state().ssthresh, unbounded);
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(counters().recoveries, 1U);
}

// The ACK that undoes moves none of RACK's state (both resends it frees are skipped, their echo being older), so it is
// the undo that makes RACK look at the queue again.
TEST_P(TcpUndoBrick, ALossBehindAReorderingStartsANewEpisodeAfterTheUndo) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(send(*c, 8, sent));
    advance(10ms);
    receive(peer_.sack(at(0), Blocks{range(400, 700)}));  // as above: three resends, the fourth marked
    ASSERT_EQ(collect().size(), 3U);
    peer_.see(sent[0]);
    receive(ack_of(at(100)));
    ASSERT_EQ(counters().undos_original_ack, 0U);
    advance(10ms);
    // At 30 ms the second and third originals: the undo, then detection. Open, with reordering seen, the window is a
    // quarter of the minimum round trip, 2.5 ms; the fourth, never resent and unmarked by the undo, was due at
    // 10 + 10 + 2.5 = 22.5 ms, so it is lost, and the next episode begins on this ACK: target max(500 / 2, 2896) =
    // 2896, RecoverFS 500 - 300 + 200 = 400, PRR's first step 200 above the 100 in flight, and the hole goes.
    receive(ack_of(at(300)));
    EXPECT_EQ(counters().undos_original_ack, 1U);
    EXPECT_EQ(counters().rack_marked, 5U);
    EXPECT_EQ(counters().recoveries, 2U);
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, at(300));
    EXPECT_EQ(c->recovery_state(), RecoveryState::Recovery);
    EXPECT_EQ(c->congestion_state().ssthresh, 2896U);
}

// A peer that dropped nothing, a needless timeout: an ACK that echoes the original's timestamp, sent before the
// timeout's resend, says so. No SACK, so no probe goes before the RTO, and the sign needs no blocks.
TEST_P(TcpUndoBrick, ANeedlessTimeoutIsUndoneByTheEcho) {
    peer_.set_sack_permitted(false);
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    ASSERT_FALSE(c->sack());
    ASSERT_TRUE(c->timestamps());
    ASSERT_TRUE(send(*c, 1, sent));  // at 10 ms: the RTO for 220
    advance(209ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);
    ASSERT_EQ(collect().size(), 1U);
    ASSERT_EQ(c->recovery_state(), RecoveryState::Loss);
    EXPECT_EQ(c->congestion_window(), 1448U) << "nothing in flight after the marking, plus a segment";
    EXPECT_EQ(c->congestion_state().ssthresh, 2896U);
    EXPECT_EQ(c->retransmit_count(), 1U);
    advance(10ms);
    peer_.see(sent[0]);
    receive(ack_of(at(100)));
    EXPECT_EQ(counters().undos_original_ack, 1U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open);
    EXPECT_EQ(c->congestion_window(), 14480U);
    EXPECT_EQ(c->congestion_state().ssthresh, unbounded);
    EXPECT_EQ(c->retransmit_count(), 0U);
    // A later RTO starts its retries from one. The ACK's echo, 220 ms old, was a sample: rttvar (3 * 5 + 210) / 4 =
    // 56.25 ms, srtt (7 * 10 + 220) / 8 = 36.25 ms, an RTO of 36.25 + 225 = 261.25 ms.
    ASSERT_EQ(c->rto(), 261250us);
    Frames next;
    ASSERT_TRUE(send(*c, 1, next));  // at 230 ms: the RTO for 491.25 ms, 492 on the wheel
    advance(261ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);
    ASSERT_EQ(collect().size(), 1U);
    EXPECT_EQ(c->retransmit_count(), 1U);
    EXPECT_EQ(counters().retransmits, 2U);
}

// A first undo by a duplicate report, then a second believed report: the connection resent two segments by then, a
// probe before the episode and the episode's own resend, so the count of duplicates believes both. The record is
// closed.
TEST_P(TcpUndoBrick, AnUndoHappensOnce) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(send(*c, 1, sent));  // at 10 ms: the probe for 10 + 2 * 10 + 40 = 70 ms
    advance(60ms);
    const auto probe = collect();
    ASSERT_EQ(probe.size(), 1U);
    advance(10ms);
    peer_.see(probe[0]);
    receive(ack_of(at(100)));  // on the probe's end: its flight is over, its verdict open
    ASSERT_FALSE(c->tail_probe_in_flight());
    Frames more;
    ASSERT_TRUE(send(*c, 5, more));  // at 80 ms, from 100
    // The third to the fifth of them reported at once: the first is lost and resent in its original's millisecond. The
    // probe is out of flight, so it is no resend of this episode.
    receive(peer_.sack(at(100), Blocks{range(200, 500)}));
    ASSERT_EQ(collect().size(), 1U);
    ASSERT_EQ(c->recovery_state(), RecoveryState::Recovery);
    advance(10ms);
    peer_.see(more[0]);
    receive(ack_of(at(600)));
    ASSERT_EQ(c->congestion_window(), 2896U);
    advance(5ms);
    receive(peer_.sack(c->committed(), Blocks{range(100, 200)}));
    EXPECT_EQ(counters().undos_duplicate_sack, 1U);
    EXPECT_EQ(c->congestion_window(), 14480U);
    receive(peer_.sack(c->committed(), Blocks{range(0, 100)}));  // the probe's copy
    EXPECT_EQ(counters().duplicate_sacks_received, 2U) << "believed: two segments were resent";
    EXPECT_EQ(counters().undos_duplicate_sack, 1U);
    EXPECT_EQ(counters().undos_original_ack, 0U);
    EXPECT_EQ(c->congestion_window(), 14480U);
}

// A probe out when recovery begins may draw a duplicate report of its own: it is one more resend the episode waits for.
TEST_P(TcpUndoBrick, AProbeOutWhenRecoveryBeginsIsOneMoreResend) {
    auto* c = open_and_send();  // five at 10 ms: the probe for 30
    ASSERT_NE(c, nullptr);
    advance(20ms);
    ASSERT_EQ(collect().size(), 1U) << "the fifth, probed";
    ASSERT_TRUE(c->tail_probe_in_flight());
    advance(10ms);
    // At 40 ms the second to the fourth reported: RACK's round trip is 30 ms and the head, due at 10 + 30 = 40 ms, is
    // lost; the probed fifth was sent after RACK's segment. Recovery begins with the probe in flight.
    receive(peer_.sack(at(0), Blocks{range(100, 400)}));
    const auto resend = collect();
    ASSERT_EQ(resend.size(), 1U);
    ASSERT_EQ(resend[0].tcp->sequence, at(0));
    advance(10ms);
    peer_.see(resend[0]);
    receive(ack_of(at(500)));
    ASSERT_EQ(c->congestion_window(), 2896U);
    advance(5ms);
    receive(peer_.sack(c->committed(), Blocks{range(400, 500)}));  // the probe's copy
    EXPECT_EQ(counters().duplicate_sacks_received, 1U);
    EXPECT_EQ(counters().undos_duplicate_sack, 0U) << "the head's resend is still to be reported";
    EXPECT_EQ(c->congestion_window(), 2896U);
    receive(peer_.sack(c->committed(), Blocks{range(0, 100)}));  // the head's late original
    EXPECT_EQ(counters().undos_duplicate_sack, 1U);
    EXPECT_EQ(c->congestion_window(), 14480U);
}

// Linux's tcp_fastretrans_alert follows tcp_try_undo_dsack with tcp_try_to_open in recovery: the episode ends there, so
// a loss found after it begins an episode, and a reduction, of its own.
TEST_P(TcpUndoBrick, ADuplicateReportInRecoveryEndsTheEpisode) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(send(*c, 7, sent));  // at 10 ms
    // At once the second to the fourth reported: the head is lost and resent in its original's millisecond.
    receive(peer_.sack(at(0), Blocks{range(100, 400)}));
    ASSERT_EQ(collect().size(), 1U);
    advance(10ms);
    // The original: an ACK of four, short of `recover` at the seventh, that the echo cannot tell from the resend's.
    peer_.see(sent[0]);
    receive(ack_of(at(400)));
    ASSERT_EQ(c->recovery_state(), RecoveryState::Recovery);
    ASSERT_EQ(counters().undos_original_ack, 0U);
    // The resend, reported as a duplicate: every resend of the episode is reported.
    receive(peer_.sack(at(400), Blocks{range(0, 100)}));
    EXPECT_EQ(counters().undos_duplicate_sack, 1U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open);
    EXPECT_EQ(c->congestion_window(), 14480U);
    advance(10ms);
    // At 30 ms the sixth and seventh reported, the fifth not. Open with two reported and no reordering seen, the window
    // is a quarter of the minimum round trip: the fifth waits until 10 + 20 + 2.5 = 32.5 ms, 33 on the wheel, and the
    // reordering timer marks it. Had recovery gone on, the window would be zero and it would be lost at once, within
    // the undone episode.
    receive(peer_.sack(at(400), Blocks{range(500, 700)}));
    EXPECT_TRUE(collect().empty());
    advance(2ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, at(400));
    EXPECT_EQ(counters().recoveries, 2U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Recovery);
}

// Linux runs tcp_try_undo_dsack in the open state and in recovery, not in the loss state, where the duplicate reports'
// sign is read by tcp_try_undo_loss on an ACK of new data. No timestamps, so the echo says nothing; the probe sent
// before the timeout is out when it fires, and is one more resend to report.
TEST_P(TcpUndoBrick, ADuplicateReportInTheLossStateUndoesAtTheNextAckOfNewData) {
    peer_.set_timestamps(std::nullopt);
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    ASSERT_FALSE(c->timestamps());
    ASSERT_TRUE(c->sack());
    ASSERT_TRUE(send(*c, 1, sent));  // at 10 ms: the probe for 10 + 2 * 10 + 40 = 70 ms
    advance(60ms);
    ASSERT_EQ(collect().size(), 1U) << "the probe: the RTO from it, for 280 ms";
    advance(209ms);
    Frames second;
    ASSERT_TRUE(send(*c, 1, second));  // at 279 ms
    advance(1ms);
    // At 280 ms the RTO, the probe still in flight: the head is lost, the second, 1 ms old, is RACK's to judge. The
    // window restarts at the 100 in flight plus a segment, 1560, and the head is resent: two resends to report, the
    // probe and the head's.
    ASSERT_EQ(collect().size(), 1U);
    ASSERT_EQ(c->recovery_state(), RecoveryState::Loss);
    ASSERT_EQ(c->congestion_window(), 1560U);
    advance(5ms);
    // The original's ACK, with the probe's copy reported as a duplicate: one of two.
    receive(peer_.sack(at(100), Blocks{range(0, 100)}));
    EXPECT_EQ(counters().duplicate_sacks_received, 1U);
    EXPECT_EQ(counters().undos_duplicate_sack, 0U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Loss);
    // The head's copy, on an ACK of nothing new: every resend is reported, and the undo waits for an ACK of new data.
    receive(peer_.sack(at(100), Blocks{range(0, 100)}));
    EXPECT_EQ(counters().duplicate_sacks_received, 2U);
    EXPECT_EQ(counters().undos_duplicate_sack, 0U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Loss);
    EXPECT_EQ(c->congestion_window(), 1560U);
    advance(5ms);
    receive(ack_of(at(200)));
    EXPECT_EQ(counters().undos_duplicate_sack, 1U);
    EXPECT_EQ(counters().undos_original_ack, 0U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open);
    EXPECT_EQ(c->congestion_window(), 14600U) << "NewReno's initial window at 1460 bytes a segment";
    EXPECT_EQ(c->congestion_state().ssthresh, unbounded);
    EXPECT_EQ(c->lost_bytes(), 0U);
}

// The busy rule can hold the episode's first resend back: an ACK that moves SND.UNA before any resend left answers an
// original whatever it echoes (Linux's tcp_packet_delayed with no retrans_stamp), and the resend owed is owed no more.
TEST_P(TcpUndoHeld, AnAckBeforeAnyResendLeftAnswersAnOriginal) {
    auto* c = open_and_send();
    ASSERT_NE(c, nullptr);
    advance(10ms);
    receive(peer_.sack(at(0), Blocks{range(100, 400)}));
    EXPECT_TRUE(collect().empty()) << "the head is lost, and its packet still lent: the resend waits";
    EXPECT_EQ(counters().retransmits_deferred, 1U);
    ASSERT_EQ(c->recovery_state(), RecoveryState::Recovery);
    receive(ack_of(at(400)));  // the original, below the reported data, short of `recover`
    EXPECT_EQ(counters().undos_original_ack, 1U);
    EXPECT_EQ(counters().reordering_seen, 1U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open);
    EXPECT_EQ(c->congestion_window(), 14480U);
    advance(1ms);
    EXPECT_TRUE(collect().empty()) << "no copy goes for a resend nothing owes";
    EXPECT_EQ(counters().retransmits_copied, 0U);
    EXPECT_EQ(counters().fast_retransmits, 1U);
    return_loans();
}
