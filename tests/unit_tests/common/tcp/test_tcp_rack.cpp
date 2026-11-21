#include <algorithm>
#include <aloe/core>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>
#include <tcp_sack_brick.hpp>

// RACK, RFC 8985 section 6: the most recently sent segment acknowledged either way and its round trip, the minimum
// round trip, the reordering window and its adaptation, and detection over the retransmission queue; then the brick:
// the reordering timer, the timeout's marks by RACK's round trip, reneging and its short timer, and the duplicate count
// left to a peer without SACK. The brick's peer offers timestamps and SACK-permitted, so a segment carries 1448 bytes,
// and its handshake takes 10 ms: srtt 10 ms, rttvar 5 ms, an RTO of 210 ms, a minimum round trip of 10 ms, and a
// reordering window of a quarter of it, 2.5 ms, which the 1 ms wheel rounds up to 3.
namespace {

    using namespace std::chrono_literals;
    using aloe::tcp::RecoveryState;
    using aloe::wire::TcpSequence;
    using Block     = aloe::wire::TcpSackBlock;
    using Blocks    = std::vector<Block>;
    using Pool      = aloe::tcp::detail::SentSegmentPool<aloe::fabric::Packet>;
    using Queue     = aloe::tcp::detail::RetransmissionQueue<aloe::fabric::Packet>;
    using Entry     = Queue::Entry;
    using Rack      = aloe::tcp::detail::Rack;
    using TimePoint = aloe::core::TimePoint;

    /// The stamp `count` milliseconds after the epoch.
    [[nodiscard]] TimePoint ms(const std::int64_t count) {
        return TimePoint{} + std::chrono::milliseconds{count};
    }

    [[nodiscard]] Block B(const std::uint32_t first, const std::uint32_t end) {
        return Block{.first = TcpSequence{first}, .end = TcpSequence{end}};
    }

    /// RACK over a queue on a pool of 40 nodes; entries are pushed with the stamp of their transmission.
    class TcpRack : public ::testing::Test {
    protected:
        /// An entry of `length` payload bytes at `first`, linked at the tail and stamped at `sent`.
        std::uint32_t push(const std::uint32_t first, const std::uint32_t length, const TimePoint sent) {
            const std::uint32_t index = queue
                                            .push(pool,
                                                  Entry{.first   = TcpSequence{first},
                                                        .length  = length,
                                                        .payload = static_cast<std::uint16_t>(length)})
                                            .value();
            queue.sent(pool.node(index), sent);
            return index;
        }

        /// The entries `block` covers marked received, as a valid block of an ACK marks them.
        void report(const Block& block) {
            std::ignore = queue.report(pool, block, [](const Entry&) {});
        }

        [[nodiscard]] const Entry& entry(const std::uint32_t index) const {
            return pool.node(index);
        }

        [[nodiscard]] Rack::Detected detect(const aloe::core::Duration window, const TimePoint now) {
            return rack.detect(queue, pool, window, now);
        }

        Pool pool{40};
        Queue queue{};
        Rack rack{};
    };

    using aloe::testing::drops;

    /// The brick tests' shared helpers (`aloe::testing::SackBrick`), and the first test's loss through its resend.
    template <typename Fixture>
    class RackBrick : public aloe::testing::SackBrick<Fixture> {
    protected:
        using Connection = aloe::testing::SackBrick<Fixture>::Connection;

        /// The ACK of the first segment 10 ms after the sends, with a block that reports the third; the second waits a
        /// quarter of the round trip, 3 ms on the wheel, then the reordering timer marks it and resends it. Returns the
        /// resend; empty, with a failure added, when it did not leave.
        [[nodiscard]] std::vector<aloe::frames::ParsedFrame> lose_the_second(Connection& c) {
            this->advance(10ms);
            this->receive(this->peer_.sack(this->at(100), Blocks{this->range(200, 300)}));
            this->advance(3ms);
            auto resent = this->collect();
            if (resent.size() != 1 || resent[0].tcp->sequence != this->at(100)) {
                ADD_FAILURE() << "expected the second segment resent by the reordering timer, got " << resent.size()
                              << " frames";
                return {};
            }
            EXPECT_EQ(c.recovery_state(), RecoveryState::Recovery);
            return resent;
        }
    };

    using TcpRackBrick    = RackBrick<aloe::testing::TcpFixture>;
    using TcpRackHeld     = RackBrick<aloe::testing::TcpHeldFixture>;
    using TcpRackRecorded = RackBrick<aloe::testing::TcpRecordedFixture>;

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpRackBrick,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpRackHeld,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpRackRecorded,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

}  // namespace

TEST_F(TcpRack, TheLatestSentSegmentWinsAndEqualStampsGoBySequence) {
    rack.advance(ms(5), TcpSequence{1300}, false, false, ms(15));
    rack.advance(ms(5), TcpSequence{1200}, false, false, ms(15));  // the same stamp, a lower end: not later
    rack.advance(ms(4), TcpSequence{1500}, false, false, ms(15));  // a higher end, an earlier stamp: not later
    EXPECT_TRUE(rack.advanced());
    EXPECT_EQ(rack.round_trip(), 11ms) << "the round trip is the last entry's, whatever its stamp";
    const std::uint32_t before    = push(1150, 100, ms(5));  // ends at 1250, below RACK's 1300
    const std::uint32_t after     = push(1300, 100, ms(5));  // ends at 1400
    const Rack::Detected detected = detect(0ms, ms(100));
    EXPECT_EQ(detected.marked, 1U);
    EXPECT_TRUE(entry(before).lost) << "sent at RACK's stamp and ending below its end: sent before it";
    EXPECT_FALSE(entry(after).lost) << "sent at RACK's stamp and ending above its end: not sent before it";
}

TEST_F(TcpRack, AResendAcknowledgedTooSoonIsSkipped) {
    rack.sample(10ms, ms(0));
    rack.advance(ms(12), TcpSequence{1100}, true, false, ms(15));
    EXPECT_FALSE(rack.advanced()) << "3 ms after the resend, below the minimum round trip: the ACK is the original's";
    rack.advance(ms(12), TcpSequence{1100}, true, false, ms(23));
    EXPECT_TRUE(rack.advanced());
    EXPECT_EQ(rack.round_trip(), 11ms);
}

TEST_F(TcpRack, ACumulativeAckOfTheOriginalIsSkipped) {
    rack.sample(10ms, ms(0));
    rack.advance(ms(12), TcpSequence{1100}, true, true, ms(30));
    EXPECT_FALSE(rack.advanced()) << "the echo is older than the resend: the ACK answers the original";
    rack.advance(ms(12), TcpSequence{1100}, false, true, ms(30));
    EXPECT_TRUE(rack.advanced()) << "an entry sent once has nothing to tell apart";
    EXPECT_EQ(rack.round_trip(), 18ms);
}

TEST_F(TcpRack, TheWindowIsZeroWithNoReorderingSeenInRecoveryOrBehindThreeReports) {
    rack.sample(10ms, ms(0));
    EXPECT_EQ(rack.window(true, 0, 10ms), 0ms) << "in recovery or the loss state";
    EXPECT_EQ(rack.window(false, 3, 10ms), 0ms) << "three segments reported: as quick as three duplicates";
    EXPECT_EQ(rack.window(false, 2, 10ms), 2500us) << "a quarter of the minimum round trip";
    EXPECT_FALSE(rack.reordering_seen());
}

TEST_F(TcpRack, TheWindowIsAQuarterOfTheMinimumOnceReorderingWasSeen) {
    rack.sample(10ms, ms(0));
    rack.see_reordering();
    EXPECT_TRUE(rack.reordering_seen());
    EXPECT_EQ(rack.window(true, 5, 10ms), 2500us);
    EXPECT_EQ(Rack{}.window(false, 0, std::nullopt), 0ms) << "no round trip measured yet";
}

TEST_F(TcpRack, ADuplicateReportGrowsTheWindowOncePerRound) {
    rack.sample(10ms, ms(0));
    rack.see_reordering();
    rack.on_ack(TcpSequence{1000}, TcpSequence{2000}, true);
    rack.on_ack(TcpSequence{1000}, TcpSequence{2000}, true);  // the round that began at 2000 is still open
    EXPECT_EQ(rack.window(false, 0, 10ms), 5ms);
    rack.on_ack(TcpSequence{2000}, TcpSequence{3000}, true);  // SND.UNA reached 2000: a new round
    EXPECT_EQ(rack.window(false, 0, 10ms), 7500us);
    rack.on_ack(TcpSequence{3000}, TcpSequence{4000}, false);  // the round ends, with no report to grow it
    rack.on_ack(TcpSequence{3000}, TcpSequence{4000}, true);
    EXPECT_EQ(rack.window(false, 0, 10ms), 10ms);
}

TEST_F(TcpRack, TheWindowNeverPassesTheSmoothedRoundTrip) {
    rack.sample(10ms, ms(0));
    rack.see_reordering();
    for (std::uint32_t round = 1; round <= 5; ++round) {
        rack.on_ack(TcpSequence{round * 1000}, TcpSequence{(round + 1) * 1000}, true);
    }
    EXPECT_EQ(rack.window(false, 0, 10ms), 10ms) << "six quarters of 10 ms, capped at srtt";
    EXPECT_EQ(rack.window(false, 0, 12ms), 12ms);
    EXPECT_EQ(rack.window(false, 0, 20ms), 15ms);
}

TEST_F(TcpRack, ADuplicateReportBeforeReorderingWasSeenGrowsNothing) {
    rack.sample(10ms, ms(0));
    rack.on_ack(TcpSequence{1000}, TcpSequence{2000}, true);
    EXPECT_EQ(rack.window(false, 0, 10ms), 2500us);
    rack.see_reordering();
    rack.on_ack(TcpSequence{1000}, TcpSequence{2000}, true);
    EXPECT_EQ(rack.window(false, 0, 10ms), 5ms) << "the report before started no round";
}

TEST_F(TcpRack, TheMultiplierReturnsAfterSixteenRecoveries) {
    rack.sample(10ms, ms(0));
    rack.see_reordering();
    rack.on_ack(TcpSequence{1000}, TcpSequence{2000}, true);
    Rack spared = rack;
    for (int recovery = 0; recovery < 15; ++recovery) {
        rack.recovery_ended();
        spared.recovery_ended();
    }
    EXPECT_EQ(rack.window(false, 0, 10ms), 5ms) << "fifteen recoveries: one left";
    rack.recovery_ended();
    EXPECT_EQ(rack.window(false, 0, 10ms), 2500us) << "the sixteenth: the multiplier is one again";
    spared.undone_by_report();
    spared.recovery_ended();
    EXPECT_EQ(spared.window(false, 0, 10ms), 5ms) << "an undo by duplicate report gave back one recovery";
    spared.recovery_ended();
    EXPECT_EQ(spared.window(false, 0, 10ms), 2500us);
}

TEST_F(TcpRack, DetectionMarksWhatIsDueAndReportsTheLongestWait) {
    const std::uint32_t first  = push(1000, 100, ms(0));
    const std::uint32_t second = push(1100, 100, ms(0));
    push(1200, 100, ms(2));
    report(B(1200, 1300));
    rack.advance(ms(2), TcpSequence{1300}, false, false, ms(12));
    ASSERT_EQ(rack.round_trip(), 10ms);
    // Due at 0 + 10 + 2.5 = 12.5 ms.
    Rack::Detected detected = detect(2500us, ms(11));
    EXPECT_EQ(detected.marked, 0U);
    EXPECT_EQ(detected.wait, 1500us);
    EXPECT_FALSE(rack.advanced()) << "a detection clears the mark of RACK's move";
    detected = detect(2500us, ms(13));
    EXPECT_EQ(detected.marked, 2U);
    EXPECT_EQ(detected.resends_lost, 0U);
    EXPECT_EQ(detected.wait, 0ms);
    EXPECT_TRUE(entry(first).lost);
    EXPECT_TRUE(entry(second).lost);
    EXPECT_EQ(queue.lost_bytes(), 200U);
}

TEST_F(TcpRack, AResendIsJudgedByItsResendAndMarkedAgain) {
    const std::uint32_t head = push(1000, 100, ms(0));
    queue.mark_lost(pool, head);
    queue.sent(pool.node(head), ms(13));  // resent at 13 ms
    queue.clear_lost(pool, head);
    push(1100, 100, ms(14));
    report(B(1100, 1200));
    rack.advance(ms(14), TcpSequence{1200}, false, false, ms(30));
    const Rack::Detected detected = detect(0ms, ms(30));
    EXPECT_EQ(detected.marked, 1U) << "13 + 16 <= 30";
    EXPECT_EQ(detected.resends_lost, 1U);
    EXPECT_TRUE(entry(head).lost);
}

TEST_F(TcpRack, AnEntryWaitingForItsResendIsSkipped) {
    const std::uint32_t head = push(1000, 100, ms(0));
    queue.mark_lost(pool, head);
    push(1100, 100, ms(1));
    report(B(1100, 1200));
    rack.advance(ms(1), TcpSequence{1200}, false, false, ms(20));
    const Rack::Detected detected = detect(0ms, ms(20));
    EXPECT_EQ(detected.marked, 0U) << "marked and not resent: it is already lost";
    EXPECT_EQ(detected.wait, 0ms);
    EXPECT_TRUE(entry(head).lost);
    EXPECT_EQ(queue.lost_bytes(), 100U);
}

TEST_F(TcpRack, DetectionStopsAtTheFirstEntryNotSentBefore) {
    // RACK at the head's own stamp and end: the head was not sent before it, and was never resent.
    push(1000, 100, ms(5));
    rack.advance(ms(5), TcpSequence{1100}, false, false, ms(15));
    const Rack::Detected alone = detect(0ms, ms(100));
    // Behind the head, 32 entries stamped earlier than it, which no real queue holds: each would be marked if the walk
    // reached it. One comparison at the head decides.
    for (std::uint32_t position = 1; position < 33; ++position) {
        push(1000 + (position * 100), 100, ms(0));
    }
    ASSERT_EQ(queue.size(), 33U);
    const Rack::Detected many = detect(0ms, ms(100));
    EXPECT_EQ(alone.marked, 0U);
    EXPECT_EQ(many.marked, alone.marked);
    EXPECT_EQ(many.wait, alone.wait);
    EXPECT_EQ(queue.lost_bytes(), 0U);
}

TEST_F(TcpRack, TheMinimumIsForgottenAfterItsWindow) {
    EXPECT_FALSE(rack.min_round_trip().has_value());
    rack.sample(10ms, ms(0));
    rack.sample(30ms, ms(400'000));
    EXPECT_EQ(rack.min_round_trip(), 10ms) << "the window before is kept";
    rack.sample(30ms, ms(700'000));
    EXPECT_EQ(rack.min_round_trip(), 30ms) << "two windows on, the 10 ms is gone";
    rack.sample(20ms, ms(1'500'000));
    EXPECT_EQ(rack.min_round_trip(), 20ms) << "more than two windows on, nothing older is kept";
}

TEST_F(TcpRack, WrapsAcrossTheTopOfTheSequenceSpace) {
    const std::uint32_t below = push(0xffffff80U, 0x40, ms(0));  // ends at 0xffffffc0
    const std::uint32_t above = push(0x40U, 0x40, ms(0));        // ends at 0x80
    rack.advance(ms(0), TcpSequence{0x40U}, false, false, ms(10));
    const Rack::Detected detected = detect(0ms, ms(10));
    EXPECT_EQ(detected.marked, 1U);
    EXPECT_TRUE(entry(below).lost) << "the same stamp, and 0xffffffc0 is before 0x40 across the wrap";
    EXPECT_FALSE(entry(above).lost);
}

TEST_P(TcpRackBrick, ALossInTheMiddleIsMarkedByTheReorderingTimer) {
    auto* c = open_and_send();
    ASSERT_NE(c, nullptr);
    advance(10ms);
    receive(peer_.sack(at(100), Blocks{range(200, 300)}));
    EXPECT_TRUE(collect().empty()) << "one segment reported: the hole waits";
    EXPECT_EQ(c->reordering_window(), 2500us);
    advance(2ms);
    EXPECT_TRUE(collect().empty()) << "due at 10 + 10 + 2.5 = 22.5 ms, 23 on the wheel";
    advance(1ms);
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, at(100));
    EXPECT_EQ(counters().reorder_timeouts, 1U);
    EXPECT_EQ(counters().rack_marked, 1U);
    EXPECT_EQ(counters().recoveries, 1U);
    EXPECT_EQ(counters().fast_retransmits, 1U);
    EXPECT_EQ(counters().retransmits, 0U) << "no expiry of the RTO";
    EXPECT_EQ(counters().duplicate_acks, 0U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Recovery);
}

TEST_P(TcpRackBrick, ThreeReportedSegmentsMarkAtOnce) {
    auto* c = open_and_send();
    ASSERT_NE(c, nullptr);
    advance(10ms);
    receive(peer_.sack(at(0), Blocks{range(100, 400)}));
    // Three reported and no reordering seen: the window is zero, and the head, due at 10 + 10 + 0 = 20 ms, is lost.
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, at(0));
    EXPECT_EQ(counters().rack_marked, 1U);
    EXPECT_EQ(counters().recoveries, 1U);
    EXPECT_EQ(c->reordering_window(), 0ms);
    advance(5ms);
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(counters().reorder_timeouts, 0U) << "nothing was left waiting: no reordering timer";
}

TEST_P(TcpRackBrick, ALateSegmentCancelsTheReorderingTimer) {
    auto* c = open_and_send();
    ASSERT_NE(c, nullptr);
    advance(10ms);
    receive(peer_.sack(at(100), Blocks{range(200, 300)}));
    advance(1ms);
    receive(ack_of(at(300)));  // the second segment arrived late, below the third the block reported
    EXPECT_EQ(counters().reordering_seen, 1U);
    advance(5ms);
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(counters().fast_retransmits, 0U);
    EXPECT_EQ(counters().reorder_timeouts, 0U);
    EXPECT_EQ(counters().rack_marked, 0U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open);
}

TEST_P(TcpRackBrick, ALostResendIsFoundWithoutTheTimer) {
    auto* c = open_and_send();
    ASSERT_NE(c, nullptr);
    ASSERT_FALSE(lose_the_second(*c).empty());
    std::vector<aloe::frames::ParsedFrame> sixth;
    ASSERT_TRUE(send(*c, 1, sixth));  // at 23 ms, within the window recovery allows
    advance(7ms);
    receive(peer_.sack(at(100), Blocks{range(200, 600)}));
    // RACK takes the sixth segment, sent at 23 ms with the resend, and its round trip of 7 ms; the resend, at the same
    // stamp and lower, is due at 23 + 7 + 0 = 30 ms, the window zero in recovery.
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, at(100));
    EXPECT_EQ(counters().lost_retransmits, 1U);
    EXPECT_EQ(counters().rack_marked, 2U);
    EXPECT_EQ(counters().reorder_timeouts, 1U) << "the first loss's only";
    EXPECT_EQ(counters().recoveries, 1U) << "within recovery a lost resend reduces nothing further";
    EXPECT_EQ(counters().retransmits, 0U);
}

TEST_P(TcpRackBrick, TheTimeoutMarksWhatRackSaysIsOld) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    std::vector<aloe::frames::ParsedFrame> early;
    ASSERT_TRUE(send(*c, 3, early));  // at 10 ms: the tail probe for 10 + 2 * 10 = 30 ms
    advance(20ms);
    const auto probe = collect();
    ASSERT_EQ(probe.size(), 1U);
    EXPECT_EQ(probe[0].tcp->sequence, early[2].tcp->sequence) << "the third, probed: the RTO is armed for 240 ms";
    advance(205ms);
    std::vector<aloe::frames::ParsedFrame> late;
    // At 235 ms, 5 ms before the RTO: a probe is scheduled again, for min(235 + 20, 240), the RTO's moment, and none
    // goes while the first is in flight, so the expiry at 240 ms is the RTO's.
    ASSERT_TRUE(send(*c, 2, late));
    advance(5ms);
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U) << "the head, one segment at the expiry";
    EXPECT_EQ(resent[0].tcp->sequence, early[0].tcp->sequence);
    EXPECT_EQ(counters().retransmits, 1U);
    EXPECT_TRUE(tcp_->sent_segment(*c, 1).lost) << "sent 230 ms ago, RACK's round trip being 10 ms";
    EXPECT_TRUE(tcp_->sent_segment(*c, 2).lost) << "probed 210 ms ago";
    EXPECT_FALSE(tcp_->sent_segment(*c, 3).lost) << "sent 5 ms ago: RACK's to judge";
    EXPECT_FALSE(tcp_->sent_segment(*c, 4).lost);
    EXPECT_EQ(c->lost_bytes(), 200U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Loss);
    EXPECT_EQ(counters().sack_reneging, 0U);
}

TEST_P(TcpRackBrick, ADuplicateReportOfAResentReportedEntryIsReordering) {
    auto* c = open_and_send();
    ASSERT_NE(c, nullptr);
    advance(10ms);
    receive(peer_.sack(at(0), Blocks{range(200, 300)}));  // the first two wait
    advance(3ms);
    ASSERT_EQ(collect().size(), 2U) << "the reordering timer marked and resent both holes";
    receive(peer_.sack(at(0), Blocks{range(100, 300)}));  // the second hole's resend reported
    ASSERT_TRUE(tcp_->sent_segment(*c, 1).reported);
    EXPECT_EQ(counters().reordering_seen, 0U) << "a resend reported is no reordering";
    // The original arrived too: a duplicate report of it, inside the second block.
    receive(peer_.sack(at(0), Blocks{range(100, 200), range(100, 300)}));
    EXPECT_EQ(counters().duplicate_sacks_received, 1U);
    EXPECT_EQ(counters().reordering_seen, 1U);
    EXPECT_EQ(c->reordering_window(), 2500us)
        << "not zero in recovery any more, and not grown: the report that showed the reordering came before it";
}

TEST_P(TcpRackBrick, AReorderingExpiryTakesNoRetry) {
    configure_tcp({.data_retries = 1});
    auto* c = open_and_send();
    ASSERT_NE(c, nullptr);
    ASSERT_FALSE(lose_the_second(*c).empty());
    EXPECT_EQ(c->state(), aloe::tcp::State::Established);
    EXPECT_EQ(c->retransmit_count(), 0U) << "a reordering expiry is no retry";
    // The resend of the head at 23 ms armed the RTO from then: 23 + 210 = 233 ms.
    advance(209ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U) << "the one retry data_retries allows";
    EXPECT_EQ(resent[0].tcp->sequence, at(100));
    EXPECT_EQ(counters().retransmits, 1U);
    EXPECT_EQ(c->state(), aloe::tcp::State::Established);
    std::ignore = poll();  // Acked and anything else so far
    advance(419ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);  // the backed-off RTO, 420 ms on: the cap
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.timed_out());
    EXPECT_EQ(counters().connections_timed_out, 1U);
}

TEST_P(TcpRackBrick, AReportedEntryThePeerDiscardedIsResentAfterTheShortTimer) {
    auto* c = open_and_send();
    ASSERT_NE(c, nullptr);
    advance(10ms);
    receive(peer_.sack(at(0), Blocks{range(100, 200)}));
    receive(ack_of(at(100)));  // the head left is the entry the peer reported: it discarded it
    ASSERT_TRUE(tcp_->sent_segment(*c, 0).reported);
    // Reneging arms the RTO at max(srtt / 2, 10 ms) = 10 ms from now, in place of the reordering timer that would have
    // fired at 23 ms for the first segment.
    advance(9ms);
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(counters().reorder_timeouts, 0U);
    advance(1ms);
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, at(100));
    EXPECT_EQ(counters().sack_reneging, 1U);
    EXPECT_EQ(counters().retransmits, 1U);
    for (std::size_t position = 0; position < 4; ++position) {
        EXPECT_FALSE(tcp_->sent_segment(*c, position).reported) << "entry " << position;
    }
    EXPECT_EQ(c->reported_bytes(), 0U);
    EXPECT_EQ(c->lost_bytes(), 300U) << "every entry lost; the resend cleared the head's mark";
    EXPECT_EQ(c->recovery_state(), RecoveryState::Loss);
}

TEST_P(TcpRackBrick, ABelievedDuplicateReportIsCounted) {
    auto* c = open_and_send();
    ASSERT_NE(c, nullptr);
    ASSERT_FALSE(lose_the_second(*c).empty());
    receive(peer_.sack(at(200), Blocks{range(100, 200)}));  // the resend and the original both arrived
    EXPECT_EQ(counters().duplicate_sacks_received, 1U);
    EXPECT_EQ(c->acknowledged(), at(200));
}

TEST_P(TcpRackBrick, ANewHigherBlockListedBeforeANewLowerOneIsNoReordering) {
    auto* c = open_and_send();
    ASSERT_NE(c, nullptr);
    advance(10ms);
    // One option, the higher block first: the blocks are applied lowest first, as Linux sorts them, so the lower one
    // is not taken for data that arrived after the higher.
    receive(peer_.sack(at(0), Blocks{range(300, 400), range(100, 200)}));
    EXPECT_TRUE(tcp_->sent_segment(*c, 1).reported);
    EXPECT_TRUE(tcp_->sent_segment(*c, 3).reported);
    EXPECT_EQ(counters().reordering_seen, 0U);
    // A block of a later ACK that reports an original below the highest reported: that one did arrive late.
    receive(peer_.sack(at(0), Blocks{range(200, 300)}));
    EXPECT_EQ(counters().reordering_seen, 1U);
}

TEST_P(TcpRackBrick, AnAckBelowSndUnaWithBlocksRunsDetection) {
    auto* c = open_and_send();
    ASSERT_NE(c, nullptr);
    advance(10ms);
    receive(ack_of(at(100)));
    ASSERT_EQ(c->acknowledged(), at(100));
    // Late, below SND.UNA, with a block of three: RACK's state moves, the window is zero behind three reports, and the
    // second segment is due at 10 + 10 + 0 = 20 ms, now.
    receive(peer_.sack(at(0), Blocks{range(200, 500)}));
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, at(100));
    EXPECT_EQ(c->acknowledged(), at(100)) << "nothing else of the late ACK moves";
    EXPECT_EQ(c->recovery_state(), RecoveryState::Recovery);
}

TEST_P(TcpRackBrick, ATerminalEventWithTheReorderingTimerArmedLeavesNothing) {
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
    auto* c                   = open_and_send();
    ASSERT_NE(c, nullptr);
    const std::uint32_t slot = c->index();
    advance(10ms);
    receive(peer_.sack(at(100), Blocks{range(200, 300)}));  // the reordering timer is armed for 23 ms
    receive(peer_.rst());
    ASSERT_EQ(c->state(), aloe::tcp::State::Closed);
    advance(10ms);
    EXPECT_TRUE(collect().empty()) << "nothing fires";
    EXPECT_EQ(counters().reorder_timeouts, 0U);
    c->release();
    EXPECT_EQ(tcp_->send_nodes_available(), nodes);
    EXPECT_EQ(free_packets(), packets);
    // A fresh connection on the same slot measures its own round trip, 20 ms: a quarter of it. RACK's state of the
    // first, a minimum of 10 ms, would give 2.5 ms.
    aloe::frames::TcpPeer next{aloe::testing::peer_spec(aloe::testing::tcp_peer_port + 1), TcpSequence{5000}};
    next.set_timestamps(100);
    next.set_sack_permitted(true);
    auto* fresh = open_passive(next, 20ms);
    ASSERT_NE(fresh, nullptr);
    EXPECT_EQ(fresh->index(), slot);
    EXPECT_EQ(fresh->reordering_window(), 5ms);
}

TEST_P(TcpRackBrick, WithoutSackThreeDuplicatesStillStartRecovery) {
    peer_.set_sack_permitted(false);
    auto* c = open_and_send();
    ASSERT_NE(c, nullptr);
    ASSERT_FALSE(c->sack());
    advance(10ms);
    receive(ack_of(at(0)));
    receive(ack_of(at(0)));
    EXPECT_TRUE(collect().empty());
    receive(ack_of(at(0)));
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U) << "the third duplicate";
    EXPECT_EQ(resent[0].tcp->sequence, at(0));
    EXPECT_EQ(c->recovery_state(), RecoveryState::Recovery);
    EXPECT_EQ(counters().recoveries, 1U);
    EXPECT_EQ(counters().rack_marked, 0U);
    advance(5ms);
    EXPECT_EQ(counters().reorder_timeouts, 0U);
}

TEST_P(TcpRackBrick, AnInOrderAckDoesConstantWork) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    std::vector<aloe::frames::ParsedFrame> one;
    ASSERT_TRUE(send(*c, 1, one));
    advance(10ms);
    receive(peer_.ack(one[0]));
    EXPECT_EQ(c->queued_segments(), 0U);
    EXPECT_EQ(counters().rack_marked, 0U);
    EXPECT_EQ(counters().reordering_seen, 0U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open);
    EXPECT_EQ(drops(counters()), 0U);
    advance(300ms);
    EXPECT_TRUE(collect().empty()) << "nothing armed with nothing in flight";
}

// The ACK that ends an episode settles the end before RACK looks for losses, as Linux's `tcp_fastretrans_alert` runs
// its step D before step E: detection on it reads the reordering window of the open state the ACK leaves.
TEST_P(TcpRackBrick, AReportAboveRecoverOnTheAckThatEndsRecoveryWaitsAQuarterRoundTrip) {
    auto* c = open_and_send();
    ASSERT_NE(c, nullptr);
    advance(10ms);
    // Four reported, at 20 ms: the window is zero, the head is lost and recovery begins with `recover` at the fifth
    // segment's last byte. Nothing is in flight; PRR allows the 400 delivered, so the head and two new segments go.
    receive(peer_.sack(at(0), Blocks{range(100, 500)}));
    const auto head = collect();
    ASSERT_EQ(head.size(), 1U);
    std::vector<aloe::frames::ParsedFrame> fresh;
    ASSERT_TRUE(send(*c, 2, fresh));
    advance(10ms);
    // At 30 ms the ACK covers `recover` and reports the second new segment, sent at 20 ms. The head's resend filled the
    // hole, so the ACK echoes it and answers no original: recovery ends, undone by nothing; then, open with one segment
    // reported, the window is 10 / 4 = 2.5 ms, and the first new segment waits until 20 + 10 + 2.5 = 32.5 ms. In
    // recovery's window, zero, it would have been lost at once.
    peer_.see(head[0]);
    receive(peer_.sack(at(500), Blocks{range(600, 700)}));
    EXPECT_TRUE(collect().empty()) << "nothing resent on the ACK that ended recovery";
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open);
    EXPECT_EQ(counters().recoveries, 1U) << "no second episode on that ACK";
    EXPECT_EQ(counters().rack_marked, 1U);
    EXPECT_EQ(c->reordering_window(), 2500us);
    advance(2ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);  // 33 ms on the wheel: the reordering timer marks it, and the second episode begins there
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, at(500));
    EXPECT_EQ(counters().reorder_timeouts, 1U);
    EXPECT_EQ(counters().recoveries, 2U);
}

TEST_P(TcpRackBrick, AHoleAboveRecoverIsMarkedByTheReorderingTimerNotByTheRto) {
    auto* c = open_and_send();
    ASSERT_NE(c, nullptr);
    advance(10ms);
    // Three reported, at 20 ms: the head is lost; with the fifth unreported, 100 stay in flight, and PRR allows
    // 300 above them, so the head and two new segments go.
    receive(peer_.sack(at(0), Blocks{range(100, 400)}));
    const auto head = collect();
    ASSERT_EQ(head.size(), 1U);
    std::vector<aloe::frames::ParsedFrame> fresh;
    ASSERT_TRUE(send(*c, 2, fresh));
    advance(10ms);
    // At 30 ms the ACK covers `recover`, freeing the fifth original, sent at 10 ms, last: RACK's round trip is 20 ms.
    // It echoes the head's resend, which filled the hole, so recovery ends, undone by nothing, and the first new
    // segment waits 20 + 20 + 2.5 - 30 = 12.5 ms, until 42.5 ms, 43 on the wheel. A wait reckoned with recovery's
    // window of zero, 10 ms, would expire at 40 ms into the open window and mark nothing.
    peer_.see(head[0]);
    receive(peer_.sack(at(500), Blocks{range(600, 700)}));
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open);
    EXPECT_EQ(counters().recoveries, 1U);
    advance(12ms);
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(counters().reorder_timeouts, 0U);
    advance(1ms);
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U) << "the reordering timer repairs it, 200 ms before the RTO would";
    EXPECT_EQ(resent[0].tcp->sequence, at(500));
    EXPECT_EQ(counters().reorder_timeouts, 1U);
    EXPECT_EQ(counters().rack_marked, 2U);
    EXPECT_EQ(counters().recoveries, 2U);
    EXPECT_EQ(counters().retransmits, 0U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Recovery);
}

TEST_P(TcpRackBrick, ALossFoundOnTheAckThatEndsRecoveryBeginsTheNext) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    const std::uint32_t mss = c->mss();
    ASSERT_EQ(mss, 1448U);
    ASSERT_EQ(c->send(aloe::frames::pattern(10U * mss)), 10U * mss);  // the initial window, 14480
    sent = collect();
    ASSERT_EQ(sent.size(), 10U);
    advance(10ms);
    // Nine reported at 20 ms: the head is lost. The target is max(14480 / 2, 2 * 1448) = 7240; nothing is in flight,
    // so PRR's window is the target: the head's resend and four new segments fill it.
    receive(peer_.sack(at(0), Blocks{range(mss, 10 * mss)}));
    ASSERT_EQ(c->congestion_state().ssthresh, 7240U);
    const auto head = collect();
    ASSERT_EQ(head.size(), 1U);
    ASSERT_EQ(c->send(aloe::frames::pattern(4U * mss)), 4U * mss);
    ASSERT_EQ(collect().size(), 4U);
    advance(10ms);
    // At 30 ms the ACK covers `recover` and reports the last three new segments; it echoes the head's resend, which
    // filled the hole, so it answers no original. Recovery ends, the window is its target, 7240; open, three reported
    // and no reordering seen make the reordering window zero, so the first new segment, sent at 20 ms against RACK's
    // round trip of 10 ms, is lost on this ACK, and the next episode begins on it. Its target is
    // max(min(5792, 7240) / 2, 2896) = 2896; RecoverFS 5792 - 4344 + 5792 = 7240 with the 14480 the ACK advanced less
    // the 8688 fewer reported; nothing is in flight, so PRR allows the target and the hole goes.
    peer_.see(head[0]);
    receive(peer_.sack(at(10 * mss), Blocks{range(11 * mss, 14 * mss)}));
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, at(10 * mss));
    EXPECT_EQ(c->recovery_state(), RecoveryState::Recovery);
    EXPECT_EQ(counters().recoveries, 2U);
    EXPECT_EQ(counters().rack_marked, 2U);
    EXPECT_EQ(c->congestion_state().ssthresh, 2896U) << "a new target, from the flight of the new episode";
    peer_.see(resent[0]);  // the hole's resend repaired it: the last ACK echoes it
    receive(ack_of(at(14 * mss)));
    EXPECT_EQ(c->recovery_state(), RecoveryState::Open);
    EXPECT_EQ(c->congestion_window(), 2896U) << "the second episode ends at its own target";
}

TEST_P(TcpRackRecorded, AResendFoundLostAgainInTheLossStateBeginsRecovery) {
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    std::vector<aloe::frames::ParsedFrame> early;
    ASSERT_TRUE(send(*c, 3, early));  // at 10 ms; the tail probe for 30 ms
    sent = early;
    advance(20ms);
    ASSERT_EQ(collect().size(), 1U) << "the third, probed: the RTO, 210 ms, for 240";
    advance(205ms);
    std::vector<aloe::frames::ParsedFrame> late;
    ASSERT_TRUE(send(*c, 2, late));  // at 235 ms: no probe goes while the first is in flight
    advance(5ms);
    // The RTO at 240 ms: the head resent; the second, 230 ms old, and the probed third, 210 ms old, lost; the fourth
    // and fifth, 5 ms old, not.
    ASSERT_EQ(collect().size(), 1U);
    ASSERT_EQ(c->recovery_state(), RecoveryState::Loss);
    const std::uint32_t calls = c->congestion_state().calls;
    ASSERT_EQ(c->congestion_state().last_hook, "on_timeout");
    advance(10ms);
    // At 250 ms the fifth is reported: RACK's round trip 15 ms, the window zero in the loss state, and the fourth,
    // sent at 235 ms below it, is due at 235 + 15 = 250 ms. A first loss: the loss state goes on, and the pass sends
    // the three entries that owe a resend under the unbounded window.
    receive(peer_.sack(at(0), Blocks{range(400, 500)}));
    EXPECT_EQ(counters().rack_marked, 1U);
    EXPECT_EQ(counters().lost_retransmits, 0U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Loss) << "an original RACK marks leaves the loss state as it is";
    EXPECT_EQ(counters().recoveries, 0U);
    EXPECT_EQ(collect().size(), 3U);
    advance(10ms);
    // At 260 ms the fourth's resend, sent at 250 ms, is reported: the head's resend of 240 ms and the second's and
    // third's of 250 ms below it are due at 250 + 10 = 260 ms at the latest. Resends found lost again: recovery
    // begins, and the policy is told of the congestion with the flight, 500.
    receive(peer_.sack(at(0), Blocks{range(300, 500)}));
    EXPECT_EQ(counters().lost_retransmits, 3U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Recovery);
    EXPECT_EQ(counters().recoveries, 1U);
    EXPECT_EQ(c->congestion_state().calls, calls + 1);
    EXPECT_EQ(c->congestion_state().last_hook, "on_congestion");
    EXPECT_EQ(c->congestion_state().last_flight, 500U);
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 3U);
    EXPECT_EQ(resent[0].tcp->sequence, at(0));
    EXPECT_EQ(resent[1].tcp->sequence, at(100));
    EXPECT_EQ(resent[2].tcp->sequence, at(200));
}

TEST_P(TcpRackRecorded, AReorderingExpiryThatMarksInTheLossStateBeginsRecovery) {
    auto* c = open_and_send();
    ASSERT_NE(c, nullptr);
    advance(10ms);
    // At 20 ms the second is reported, then the first arrives late: reordering is seen, so the reordering window is a
    // quarter of the minimum round trip, 2.5 ms, in the loss state too.
    receive(peer_.sack(at(0), Blocks{range(100, 200)}));
    receive(ack_of(at(200)));
    ASSERT_EQ(counters().reordering_seen, 1U);
    // That ACK moved SND.UNA with three left in flight and nothing reported: the tail probe for 20 + 2 * 10 = 40 ms.
    advance(20ms);
    ASSERT_EQ(collect().size(), 1U) << "the fifth, probed: the RTO, 210 ms, for 250";
    advance(210ms);
    // The RTO at 250 ms marks the three left and resends the head; the next ACK, a duplicate at 251 ms, lets the
    // other two go under the unbounded window, and two new segments follow.
    ASSERT_EQ(collect().size(), 1U);
    ASSERT_EQ(c->recovery_state(), RecoveryState::Loss);
    advance(1ms);
    receive(ack_of(at(200)));
    ASSERT_EQ(collect().size(), 2U);
    std::vector<aloe::frames::ParsedFrame> fresh;
    ASSERT_TRUE(send(*c, 2, fresh));
    advance(10ms);
    // At 261 ms the second new segment is reported: RACK's round trip 10 ms, and everything sent at 250 and 251 ms
    // below it waits, the longest 251 + 10 + 2.5 - 261 = 2.5 ms, until 263.5 ms, 264 on the wheel.
    receive(peer_.sack(at(200), Blocks{range(600, 700)}));
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(c->recovery_state(), RecoveryState::Loss);
    const std::uint32_t calls = c->congestion_state().calls;
    advance(2ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);
    // The expiry marks the three resends and the first new segment; in the loss state that begins recovery, and the
    // policy is told of the congestion with the flight, 500.
    EXPECT_EQ(counters().reorder_timeouts, 1U);
    EXPECT_EQ(counters().rack_marked, 4U);
    EXPECT_EQ(c->recovery_state(), RecoveryState::Recovery);
    EXPECT_EQ(counters().recoveries, 1U);
    EXPECT_EQ(c->congestion_state().calls, calls + 1);
    EXPECT_EQ(c->congestion_state().last_hook, "on_congestion");
    EXPECT_EQ(c->congestion_state().last_flight, 500U);
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 4U);
    EXPECT_EQ(resent[0].tcp->sequence, at(200));
    EXPECT_EQ(resent[3].tcp->sequence, at(500));
    EXPECT_EQ(counters().retransmits, 1U) << "the one expiry of the RTO";
}

TEST_P(TcpRackHeld, AMarkedEntryBehindTheHeadWaitsForItsPacketThenCopies) {
    auto* c = open_and_send();
    ASSERT_NE(c, nullptr);
    advance(10ms);
    receive(peer_.sack(at(0), Blocks{range(200, 300)}));  // the first two wait, the second behind the head
    advance(3ms);
    // The timer marks both. The head's packet is still lent: its resend waits, and the pass stops there.
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(counters().retransmits_deferred, 1U);
    EXPECT_TRUE(tcp_->sent_segment(*c, 1).lost);
    advance(1ms);
    // The flush: the head goes as a copy; the entry behind it is tried, and its packet is still lent too.
    auto copies = collect();
    ASSERT_EQ(copies.size(), 1U);
    EXPECT_EQ(copies[0].tcp->sequence, at(0));
    EXPECT_EQ(counters().retransmits_copied, 1U);
    EXPECT_EQ(counters().retransmits_deferred, 2U);
    advance(1ms);
    copies = collect();
    ASSERT_EQ(copies.size(), 1U);
    EXPECT_EQ(copies[0].tcp->sequence, at(100));
    EXPECT_TRUE(std::ranges::equal(aloe::frames::tcp_payload(copies[0]), aloe::frames::tcp_payload(sent[1])));
    EXPECT_EQ(counters().retransmits_copied, 2U);
    EXPECT_EQ(counters().fast_retransmits, 2U) << "one per marked entry, at its first try";
    receive(ack_of(at(100)));  // the head freed
    const auto& behind = tcp_->sent_segment(*c, 0);
    ASSERT_EQ(behind.first, at(100));
    EXPECT_FALSE(behind.copy.empty()) << "its copy is out";
    const auto original = aloe::frames::parse_frame(behind.packet.data());
    ASSERT_TRUE(original.has_value());
    EXPECT_EQ(original->l4, sent[1].l4) << "nothing was written into the packet the device held";
    return_loans();
}
