#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <tuple>
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
    // Three segments above a gap, injected together and processed in one pass.
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
    EXPECT_EQ(tcp_->counters().dropped_out_of_order, 3U);
    auto frames = collect_queued();
    EXPECT_EQ(pure_acks(frames), 3U) << "one per segment, not one coalesced at the end";
    for (const auto& frame : frames) {
        EXPECT_EQ(frame.tcp->acknowledgement, expected);
    }
    // Recovery: every segment that advances rcv_nxt is acknowledged at once until the remembered end, 1044.
    receive(peer_.data(aloe::frames::pattern(10)));  // 1004..1013
    frames = collect_queued();
    EXPECT_EQ(pure_acks(frames), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, expected + 10U);
    receive(peer_.data(aloe::frames::pattern(10)));  // 1014..1023: still below the gap end
    EXPECT_EQ(pure_acks(collect_queued()), 1U);
    receive(peer_.data(aloe::frames::pattern(20)));  // 1024..1043
    EXPECT_EQ(pure_acks(collect_queued()), 1U) << "reaching the remembered end";
    receive(peer_.data(aloe::frames::pattern(5)));  // past it: ordinary again
    EXPECT_TRUE(collect_queued().empty());
    EXPECT_EQ(pure_acks(collect()), 1U);
    EXPECT_EQ(c->unread().size(), 48U);
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
    EXPECT_EQ(tcp_->counters().dropped_out_of_order, 1U) << "a gap, far beyond the window";
    EXPECT_EQ(pure_acks(collect_queued()), 1U);
    receive(peer_.segment({TcpFlag::Rst, TcpFlag::Ack}, {}, beyond, peer_.rcv_nxt()));
    EXPECT_TRUE(collect_queued().empty()) << "a rejected RST is not answered";
    EXPECT_EQ(c->state(), aloe::tcp::State::Established);
    EXPECT_EQ(tcp_->counters().connections_reset, 0U);
    EXPECT_EQ(tcp_->counters().dropped_out_of_window, 1U);
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
