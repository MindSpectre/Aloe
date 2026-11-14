#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>

namespace {

    using namespace std::chrono_literals;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;
    using aloe::wire::TcpSequence;
    using TcpAcks         = aloe::testing::TcpFixture;
    using TcpAcksRefusing = aloe::testing::TcpRefusingFixture;

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpAcks,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpAcksRefusing,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

    constexpr std::uint16_t mss = aloe::testing::tcp_fabric_mss;

    [[nodiscard]] std::size_t pure_acks(const std::vector<aloe::frames::ParsedFrame>& frames) {
        std::size_t count = 0;
        for (const auto& frame : frames) {
            if (aloe::testing::is_flags(frame, {TcpFlag::Ack}) && aloe::frames::tcp_payload(frame).empty()) {
                ++count;
            }
        }
        return count;
    }

}  // namespace

TEST_P(TcpAcks, ShortInOrderSegmentsCoalesceUntilFlush) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    for (int i = 0; i < 5; ++i) {
        receive(peer_.data(aloe::frames::pattern(10)));
    }
    EXPECT_TRUE(collect_queued().empty());
    const auto frames = collect();
    EXPECT_EQ(pure_acks(frames), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 51});
    EXPECT_EQ(tcp_->counters().pure_acks_sent, 1U);
}

TEST_P(TcpAcks, SameTickReplyCarriesTheAck) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(10)));
    EXPECT_EQ(c->send(aloe::frames::pattern(4)), 4U);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "the data segment, and no pure ACK after it";
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 11});
    EXPECT_EQ(aloe::frames::tcp_payload(frames[0]).size(), 4U);
    EXPECT_EQ(tcp_->counters().pure_acks_sent, 0U);
}

TEST_P(TcpAcks, EverySecondFullSizedSegmentIsAckedAtOnce) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(mss)));
    EXPECT_TRUE(collect_queued().empty());
    receive(peer_.data(aloe::frames::pattern(mss)));
    auto frames = collect_queued();
    EXPECT_EQ(pure_acks(frames), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 1 + 2 * mss});
    receive(peer_.data(aloe::frames::pattern(mss)));
    EXPECT_TRUE(collect_queued().empty());
    receive(peer_.data(aloe::frames::pattern(mss)));
    EXPECT_EQ(pure_acks(collect_queued()), 1U);
    receive(peer_.data(aloe::frames::pattern(10)));
    EXPECT_TRUE(collect_queued().empty()) << "a short segment is ordinary";
    EXPECT_EQ(pure_acks(collect()), 1U);
}

TEST_P(TcpAcks, ThreeGapSegmentsInOneBurstAttemptThreeDuplicateAcks) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(3)));
    std::ignore                = collect();
    const TcpSequence expected = peer_.snd_nxt();  // 1004
    // Three segments above a gap, injected together and processed in one pass: 1014..1043, held.
    for (const std::uint32_t at : {10U, 20U, 30U}) {
        auto packet = harness_->allocate(0);
        ASSERT_TRUE(packet.has_value());
        ASSERT_TRUE(aloe::frames::fill(
            *packet,
            peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(10), expected + at, peer_.rcv_nxt())));
        std::array<aloe::fabric::Packet, 1> out{std::move(*packet)};
        ASSERT_EQ(harness_->transmit(0, out), 1U);
    }
    process_pending();
    EXPECT_EQ(tcp_->counters().out_of_order_held, 3U);
    EXPECT_EQ(tcp_->counters().dropped_out_of_order, 0U);
    EXPECT_EQ(c->out_of_order_segments(), 3U);
    auto frames = collect_queued();
    EXPECT_EQ(pure_acks(frames), 3U) << "one per segment, not one coalesced at the end";
    for (const auto& frame : frames) {
        EXPECT_EQ(frame.tcp->acknowledgement, expected);
        EXPECT_EQ(frame.tcp->window, aloe::testing::tcp_fabric_budget - 3U) << "the edge restated";
    }
    // The gap filled, 1004..1013: the run joins and one ACK goes at once, at the end of the run.
    receive(peer_.data(aloe::frames::pattern(10)));
    frames = collect_queued();
    ASSERT_EQ(pure_acks(frames), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, expected + 40U);
    EXPECT_TRUE(collect().empty()) << "and nothing more at the flush";
    EXPECT_EQ(c->unread().size(), 43U);
    EXPECT_EQ(c->out_of_order_segments(), 0U);
    // Nothing held above: ordinary again.
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(5), expected + 40U, peer_.rcv_nxt()));
    EXPECT_TRUE(collect_queued().empty());
    EXPECT_EQ(pure_acks(collect()), 1U);
}

TEST_P(TcpAcks, InOrderDataBelowHeldDataIsAcknowledgedAtOnce) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence base = peer_.snd_nxt();
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(10), base + 2000U, peer_.rcv_nxt()));
    std::ignore = collect_queued();
    for (const std::uint32_t at : {10U, 20U}) {
        receive(peer_.data(aloe::frames::pattern(10)));
        const auto frames = collect_queued();
        ASSERT_EQ(pure_acks(frames), 1U) << "answered before the flush";
        EXPECT_EQ(frames[0].tcp->acknowledgement, base + at);
    }
    EXPECT_EQ(c->out_of_order_segments(), 1U);
}

TEST_P(TcpAcks, AnAckForASegmentAboveAGapRestatesTheEdge) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    for (int index = 0; index < 3; ++index) {
        receive(peer_.data(aloe::frames::pattern(1000)));
    }
    std::ignore = collect();
    c->consume(3000);
    EXPECT_TRUE(collect().empty()) << "65535 on offer is not twice 62535: no update";
    receive(peer_.segment(
        {TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(10), peer_.snd_nxt() + 1000U, peer_.rcv_nxt()));
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, peer_.snd_nxt());
    EXPECT_EQ(frames[0].tcp->window, 62535U) << "the edge offered, 65536, less RCV.NXT, 3001";
}

TEST_P(TcpAcks, DuplicateAndOutOfWindowDataAckedAtOnceRejectedRstIsSilent) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(3)));
    std::ignore = collect();
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(3), peer_.iss() + 1U, peer_.rcv_nxt()));
    EXPECT_EQ(tcp_->counters().dropped_duplicate, 1U);
    EXPECT_EQ(pure_acks(collect_queued()), 1U);
    const TcpSequence beyond = peer_.snd_nxt() + aloe::testing::tcp_fabric_budget + 10U;
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(3), beyond, peer_.rcv_nxt()));
    EXPECT_EQ(tcp_->counters().dropped_out_of_window, 1U) << "above a gap and past the edge";
    EXPECT_EQ(tcp_->counters().dropped_out_of_order, 0U);
    EXPECT_EQ(pure_acks(collect_queued()), 1U);
    receive(peer_.segment({TcpFlag::Rst, TcpFlag::Ack}, {}, beyond, peer_.rcv_nxt()));
    EXPECT_TRUE(collect_queued().empty()) << "a rejected RST is not answered";
    EXPECT_EQ(c->state(), aloe::tcp::State::Established);
    EXPECT_EQ(tcp_->counters().connections_reset, 0U);
    EXPECT_EQ(tcp_->counters().dropped_out_of_window, 2U);
}

TEST_P(TcpAcksRefusing, RefusalLeavesOneAckPendingWithoutReplayingDuplicates) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(3)));
    std::ignore = collect();
    refuse();
    for (const std::uint32_t at : {10U, 20U, 30U}) {
        receive(peer_.segment(
            {TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(10), peer_.snd_nxt() + at, peer_.rcv_nxt()));
    }
    EXPECT_EQ(tcp_->counters().send_refused, 3U) << "three attempts";
    EXPECT_TRUE(collect().empty());
    allow();
    const auto frames = collect();
    EXPECT_EQ(pure_acks(frames), 1U) << "the pending ACK, once: no manufactured duplicates";
}

TEST_P(TcpAcksRefusing, FullSegmentCountSaturatesAtTwoUntilAnAckLeaves) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    refuse();
    receive(peer_.data(aloe::frames::pattern(mss)));
    receive(peer_.data(aloe::frames::pattern(mss)));
    EXPECT_EQ(tcp_->counters().send_refused, 1U) << "the second full segment attempted an ACK";
    receive(peer_.data(aloe::frames::pattern(mss)));
    EXPECT_EQ(tcp_->counters().send_refused, 2U) << "saturated at two: every further full segment attempts again";
    allow();
    EXPECT_EQ(pure_acks(collect()), 1U);
    receive(peer_.data(aloe::frames::pattern(mss)));
    EXPECT_TRUE(collect_queued().empty()) << "the count was reset by the ACK that left";
    receive(peer_.data(aloe::frames::pattern(mss)));
    EXPECT_EQ(pure_acks(collect_queued()), 1U);
}

namespace {

    /// The ACK delay every test below configures; each steps the clock one millisecond per `advance`, since a wheel
    /// advanced once past several deadlines fires a re-arming timer once.
    constexpr auto delay = 40ms;

    class TcpDelayedAcks : public aloe::testing::TcpFixture {
    protected:
        TcpDelayedAcks() {
            configure_tcp({.delayed_ack = delay});
        }

        /// `count` steps of one millisecond, each flushed: every frame the stack sent on the way.
        [[nodiscard]] std::vector<aloe::frames::ParsedFrame> step(const int count) {
            std::vector<aloe::frames::ParsedFrame> frames;
            for (int index = 0; index < count; ++index) {
                advance(1ms);
                for (auto& frame : collect()) {
                    frames.push_back(std::move(frame));
                }
            }
            return frames;
        }
    };

    class TcpDelayedAcksRefusing : public aloe::testing::TcpRefusingFixture {
    protected:
        TcpDelayedAcksRefusing() {
            configure_tcp({.delayed_ack = delay});
        }
    };

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpDelayedAcks,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpDelayedAcksRefusing,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

}  // namespace

TEST_P(TcpDelayedAcks, OneSegmentIsAcknowledgedAtTheDeadline) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(100)));
    EXPECT_TRUE(collect().empty()) << "not at the flush of the same tick";
    EXPECT_TRUE(step(39).empty()) << "nor before the delay ran out";
    advance(1ms);
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(pure_acks(frames), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 101});
    EXPECT_EQ(tcp_->counters().delayed_acks_sent, 1U);
    EXPECT_EQ(tcp_->counters().pure_acks_sent, 1U);
    EXPECT_TRUE(step(100).empty()) << "one ACK, once";
}

TEST_P(TcpDelayedAcks, TheDelayCountsFromTheFirstSegment) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(100)));
    EXPECT_TRUE(step(30).empty());
    receive(peer_.data(aloe::frames::pattern(100)));
    EXPECT_TRUE(step(9).empty());
    const auto frames = step(1);
    ASSERT_EQ(pure_acks(frames), 1U) << "at 40 ms, not extended by the second segment";
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 201}) << "covering both";
    EXPECT_TRUE(step(30).empty()) << "none at 70 ms";
    EXPECT_EQ(tcp_->counters().delayed_acks_sent, 1U);
}

TEST_P(TcpDelayedAcks, TwoFullSegmentsWorthIsAcknowledgedAtOnce) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(mss)));
    EXPECT_TRUE(collect().empty());
    receive(peer_.data(aloe::frames::pattern(mss)));
    auto frames = collect_queued();
    ASSERT_EQ(pure_acks(frames), 1U) << "two full segments: at once";
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 1 + 2 * mss});
    // Four half-sized segments are two full segments' worth: counted in bytes, the ACK that left started it again.
    for (int index = 0; index < 3; ++index) {
        receive(peer_.data(aloe::frames::pattern(mss / 2)));
        EXPECT_TRUE(collect().empty()) << index;
    }
    receive(peer_.data(aloe::frames::pattern(mss / 2)));
    frames = collect_queued();
    ASSERT_EQ(pure_acks(frames), 1U) << "the fourth half-sized segment: at once";
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 1 + 4 * mss});
    EXPECT_TRUE(step(2 * 40).empty()) << "the ACKs that left cancelled the timer";
    EXPECT_EQ(tcp_->counters().delayed_acks_sent, 0U);
}

TEST_P(TcpDelayedAcks, AWriteCarriesTheAckAndEndsTheDelay) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(100)));
    const auto span = c->prepare(4);
    ASSERT_TRUE(span.has_value());
    ASSERT_TRUE(c->commit(4));
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(aloe::frames::tcp_payload(frames[0]).size(), 4U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 101});
    EXPECT_TRUE(step(40).empty()) << "the data segment ended the delay";
    EXPECT_EQ(tcp_->counters().delayed_acks_sent, 0U);
    EXPECT_EQ(tcp_->counters().pure_acks_sent, 0U);
}

TEST_P(TcpDelayedAcks, ASegmentAboveAGapIsAcknowledgedAtOnceAllTheSame) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(100)));
    const TcpSequence next = peer_.snd_nxt();
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(10), next + 100U, peer_.rcv_nxt()));
    auto frames = collect_queued();
    ASSERT_EQ(pure_acks(frames), 1U) << "above a gap: at once";
    EXPECT_EQ(frames[0].tcp->acknowledgement, next);
    // In-order data with something held above it: at once too.
    receive(peer_.data(aloe::frames::pattern(10)));
    frames = collect_queued();
    ASSERT_EQ(pure_acks(frames), 1U) << "below held data: at once";
    EXPECT_EQ(frames[0].tcp->acknowledgement, next + 10U);
    EXPECT_TRUE(step(40).empty()) << "the ACKs that left ended the delay";
    EXPECT_EQ(tcp_->counters().delayed_acks_sent, 0U);
    EXPECT_EQ(c->out_of_order_segments(), 1U);
}

TEST_P(TcpDelayedAcks, TheHandshakesAckAndAFinsAckStayAtTheFlush) {
    auto* c = open_active(peer_);  // expects the handshake's ACK at the first collect, not 40 ms later
    ASSERT_NE(c, nullptr);
    receive(peer_.fin());
    EXPECT_EQ(c->state(), aloe::tcp::State::CloseWait);
    EXPECT_TRUE(collect_queued().empty()) << "the FIN's ACK waits for the flush";
    const auto frames = collect();
    ASSERT_EQ(pure_acks(frames), 1U) << "and goes at it";
    EXPECT_EQ(frames[0].tcp->acknowledgement, peer_.snd_nxt());
    EXPECT_TRUE(step(40).empty());
    EXPECT_EQ(tcp_->counters().delayed_acks_sent, 0U);
}

TEST_P(TcpDelayedAcks, AWindowUpdateFromConsumeGoesAtTheFlushAndCarriesTheAck) {
    // Four slots and no timestamps: a budget of 4 * 1460 / 2 = 2920 and an edge at 2921.
    configure_tcp({.receive_segments = 4, .delayed_ack = delay});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(1000)));
    receive(peer_.data(aloe::frames::pattern(1000)));
    EXPECT_TRUE(collect().empty()) << "2000 bytes is less than two full segments: delayed";
    // The window on offer is 920; the candidate after the read is 2920, more than twice it: an update is owed.
    c->consume(2000);
    const auto frames = collect();
    ASSERT_EQ(pure_acks(frames), 1U) << "the update, at the flush";
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 2001});
    EXPECT_EQ(frames[0].tcp->window, 2920U);
    EXPECT_TRUE(step(40).empty()) << "it carried the delayed ACK: none follows at the deadline";
    EXPECT_EQ(tcp_->counters().delayed_acks_sent, 0U);
}

TEST_P(TcpDelayedAcksRefusing, ARefusedDelayedAckWaitsForTheFlush) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(100)));
    refuse();
    for (int index = 0; index < 39; ++index) {
        advance(1ms);
    }
    EXPECT_EQ(tcp_->counters().send_refused, 0U) << "nothing attempted before the deadline";
    advance(1ms);
    EXPECT_EQ(tcp_->counters().send_refused, 1U) << "the delayed ACK, attempted at the deadline";
    EXPECT_EQ(tcp_->counters().delayed_acks_sent, 0U) << "counted only when it left";
    allow();
    const auto frames = collect();
    ASSERT_EQ(pure_acks(frames), 1U) << "the flush sends it";
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 101});
    EXPECT_EQ(tcp_->counters().delayed_acks_sent, 0U);
}

TEST_P(TcpDelayedAcks, ReleaseWithADelayedAckPendingCancelsTheTimer) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(100)));
    EXPECT_EQ(wheel_.pending(), 1U) << "the ACK timer";
    c->release();
    EXPECT_EQ(wheel_.pending(), 0U);
    std::ignore = collect();  // the release's RST
    EXPECT_TRUE(step(40).empty());
    EXPECT_EQ(tcp_->counters().delayed_acks_sent, 0U);

    // The same with a RST from the peer in place of the release.
    aloe::frames::TcpPeer other{aloe::testing::peer_spec(aloe::testing::tcp_peer_port + 1), TcpSequence{5000U}};
    auto* d = open_passive(other);
    ASSERT_NE(d, nullptr);
    receive(other.data(aloe::frames::pattern(100)));
    EXPECT_EQ(wheel_.pending(), 1U);
    receive(other.rst());
    EXPECT_EQ(d->state(), aloe::tcp::State::Closed);
    EXPECT_EQ(wheel_.pending(), 0U);
    EXPECT_TRUE(step(40).empty());
    EXPECT_EQ(tcp_->counters().delayed_acks_sent, 0U);
    d->release();
}

TEST_P(TcpDelayedAcks, ADataSegmentLeavesTheDuplicateItCannotCarryToTheFlush) {
    peer_.set_sack_permitted(true);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->sack());
    const TcpSequence base = peer_.snd_nxt();
    receive(peer_.data(aloe::frames::pattern(100)));
    // Its first 50 bytes are old: a duplicate to report, with the ACK at its ordinary time, here the delay's.
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(100), base + 50U, peer_.rcv_nxt()));
    EXPECT_TRUE(collect().empty());
    const auto span = c->prepare(4);
    ASSERT_TRUE(span.has_value());
    ASSERT_TRUE(c->commit(4));
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 2U) << "the data segment, then the pure ACK that carries what it could not";
    EXPECT_EQ(aloe::frames::tcp_payload(frames[0]).size(), 4U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, base + 150U);
    EXPECT_EQ(pure_acks({frames[1]}), 1U);
    EXPECT_EQ(frames[1].tcp->acknowledgement, base + 150U);
    EXPECT_EQ(aloe::frames::sack_blocks_of(frames[1]).size(), 1U);
    EXPECT_EQ(tcp_->counters().duplicate_sacks_sent, 1U);
    EXPECT_TRUE(step(40).empty());
    EXPECT_EQ(tcp_->counters().delayed_acks_sent, 0U);
}

TEST_P(TcpDelayedAcks, DestroyingTheStackWithADelayedAckPendingCancelsTheTimer) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(100)));
    EXPECT_EQ(wheel_.pending(), 1U) << "the ACK timer";
    configure_tcp({.delayed_ack = delay});  // the old stack goes with its connection, the timer still armed
    EXPECT_EQ(wheel_.pending(), 0U);
    EXPECT_TRUE(step(40).empty());
}
