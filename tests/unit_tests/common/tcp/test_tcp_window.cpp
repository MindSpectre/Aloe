#include <algorithm>
#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>

namespace {

    using namespace std::chrono_literals;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;
    using aloe::wire::TcpSequence;
    using TcpWindow         = aloe::testing::TcpFixture;
    using TcpWindowRefusing = aloe::testing::TcpRefusingFixture;

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpWindow,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpWindowRefusing,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

    constexpr std::uint32_t budget = aloe::testing::tcp_fabric_budget;

    /// The right edge a pure ACK offers: its acknowledgement plus its window.
    [[nodiscard]] TcpSequence right_edge(const aloe::frames::ParsedFrame& frame) {
        return frame.tcp->acknowledgement + frame.tcp->window;
    }

}  // namespace

TEST_P(TcpWindow, OneByteCostsOneByteOfCreditAndKeepsTheEdge) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(tcp_->byte_budget(), 373760U) << "half of 512 packets at MSS 1460, within 65535 << 3";
    EXPECT_EQ(c->receive_budget(), budget) << "a peer without window scaling: capped at the unscaled field";
    const TcpSequence initial_edge = TcpSequence{aloe::testing::tcp_peer_isn + 1} + budget;
    receive(peer_.data(aloe::frames::pattern(1)));
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, budget - 1U);
    EXPECT_EQ(right_edge(frames[0]), initial_edge) << "a whole packet node, one byte of credit";
    receive(peer_.data(aloe::frames::pattern(100)));
    frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, budget - 101U);
    EXPECT_EQ(right_edge(frames[0]), initial_edge);
    EXPECT_EQ(c->receive_window(), budget - 101U);
    EXPECT_EQ(tcp_->counters().window_updates, 0U);
    c->consume(50);
    EXPECT_TRUE(collect().empty()) << "fifty bytes read is less than a full segment: no update of its own";
    receive(peer_.data(aloe::frames::pattern(1)));
    frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, budget - 102U);
    EXPECT_EQ(right_edge(frames[0]), initial_edge) << "the next ACK keeps the edge too";
    EXPECT_EQ(tcp_->counters().window_updates, 0U);
}

TEST_P(TcpWindow, PartialConsumeFreesNoNodeAndKeepsTheEdge) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const std::size_t nodes = tcp_->nodes_available();
    receive(peer_.data(aloe::frames::pattern(10)));
    std::ignore = collect();
    c->consume(4);
    EXPECT_EQ(tcp_->nodes_available(), nodes - 1) << "the packet still holds six bytes";
    EXPECT_TRUE(collect().empty()) << "four bytes read move no edge";
    receive(peer_.data(aloe::frames::pattern(1)));
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, budget - 11U) << "the edge stood: the four bytes read are not offered yet";
}

TEST_P(TcpWindow, WrapsNearTheTop) {
    aloe::frames::TcpPeer high{aloe::testing::peer_spec(), TcpSequence{0xfffffff0U}};
    auto* c = open_passive(high);
    ASSERT_NE(c, nullptr);
    receive(high.data(aloe::frames::pattern(32)));
    EXPECT_EQ(c->unread().size(), 32U);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{0x11U}) << "0xfffffff1 + 32, wrapped";
    EXPECT_EQ(frames[0].tcp->window, budget - 32U);
    EXPECT_EQ(right_edge(frames[0]), TcpSequence{0xfffffff1U} + budget);
    receive(
        high.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(4), TcpSequence{0xfffffffeU}, high.rcv_nxt()));
    EXPECT_EQ(tcp_->counters().dropped_duplicate, 1U) << "entirely before rcv_nxt across the wrap";
    EXPECT_EQ(tcp_->counters().dropped_out_of_order, 0U);
}

TEST_P(TcpWindow, ZeroWindowAcceptsPureAcksAndReopensOnConsume) {
    constexpr std::uint16_t mss = aloe::testing::tcp_fabric_mss;
    configure_tcp({.receive_segments = 4});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->receive_budget(), 2U * mss) << "half of four slots";
    receive(peer_.data(aloe::frames::pattern(mss)));
    receive(peer_.data(aloe::frames::pattern(mss)));
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, 0U);
    receive(peer_.ack());
    EXPECT_EQ(tcp_->counters().dropped_out_of_window, 0U)
        << "a zero-length segment at rcv_nxt is acceptable at a zero window";
    receive(peer_.data(aloe::frames::pattern(1)));
    EXPECT_EQ(tcp_->counters().dropped_out_of_window, 1U) << "a byte is not";
    frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, 0U);
    peer_.rewind(1);
    c->consume(1000);
    EXPECT_TRUE(collect().empty()) << "less than a full segment fits: the window stays closed";
    c->consume(mss - 1000U);
    frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, mss) << "reopened once a full segment fits";
    EXPECT_EQ(tcp_->counters().window_updates, 1U);
    receive(peer_.data(aloe::frames::pattern(1)));
    EXPECT_EQ(c->unread().size(), mss + 1U);
}

TEST_P(TcpWindow, TinySegmentsExhaustSlotsWithCreditLeft) {
    configure_tcp({.receive_segments = 4});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    for (int i = 0; i < 4; ++i) {
        receive(peer_.data(aloe::frames::pattern(1)));
    }
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    const std::uint32_t credit = 2U * aloe::testing::tcp_fabric_mss - 4U;  // half of four slots, less four bytes
    EXPECT_EQ(frames[0].tcp->window, credit);
    receive(peer_.data(aloe::frames::pattern(1)));
    EXPECT_EQ(tcp_->counters().dropped_no_slot, 1U);
    frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, credit) << "neither extended nor retracted while every slot is held";
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 5});
    c->consume(1);
    EXPECT_TRUE(collect().empty()) << "a slot came back, but a byte moves no edge";
    peer_.rewind(1);
    receive(peer_.data(aloe::frames::pattern(1)));
    EXPECT_EQ(tcp_->counters().dropped_no_slot, 1U) << "the freed slot takes the segment";
    frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, credit - 1U) << "the edge stood";
}

TEST_P(TcpWindow, BudgetIsCappedAtTheWireMaximum) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(tcp_->byte_budget(), 373760U) << "half of 512 slots: the stack's largest budget, under its shift";
    EXPECT_EQ(c->receive_budget(), 65535U) << "the peer offered no window scaling: the unscaled field caps it";
    EXPECT_EQ(peer_.stack_window(), 65535U);
}

TEST_P(TcpWindow, TheBudgetCountsTheTimestampedSegment) {
    configure_tcp({.receive_segments = 32});
    peer_.set_timestamps(100);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(peer_.stack_window(), 23168U) << "the SYN-ACK states 32 * 1448 / 2";
    EXPECT_EQ(c->receive_budget(), 23168U);
}

TEST_P(TcpWindow, AnActiveOpenTakesItsBudgetAfterTheHandshake) {
    configure_tcp({.receive_segments = 32});
    peer_.set_timestamps(100);
    const auto connected =
        tcp_->connect({.address = aloe::testing::harness_ip, .port = peer_.spec().source_port}, now_);
    ASSERT_TRUE(connected.has_value());
    const auto syn = collect();
    ASSERT_EQ(syn.size(), 1U);
    EXPECT_EQ(syn[0].tcp->window, 23360U) << "the SYN states the largest budget: 32 * 1460 / 2";
    receive(peer_.syn_ack(syn[0]));
    const auto acks = collect();
    ASSERT_EQ(acks.size(), 1U);
    EXPECT_EQ(acks[0].tcp->window, 23168U) << "32 * 1448 / 2: not 536-byte slots (8576), not 1460-byte ones (23360)";
    EXPECT_EQ((*connected)->receive_budget(), 23168U);
}

TEST_P(TcpWindow, ASmallConsumeSendsNothingAndLeavesTheEdge) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    for (int i = 0; i < 3; ++i) {
        receive(peer_.data(aloe::frames::pattern(1000)));
    }
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, budget - 3000U);
    const std::uint64_t updates = tcp_->counters().window_updates;
    c->consume(1000);
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(tcp_->counters().window_updates, updates);
    EXPECT_EQ(c->receive_window(), budget - 3000U);
}

TEST_P(TcpWindow, AConsumeThatDoublesTheWindowSendsOneUpdate) {
    constexpr std::uint16_t mss = aloe::testing::tcp_fabric_mss;
    auto* c                     = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    std::uint32_t received = 0;
    while (received < 40000U) {
        const std::uint32_t length = std::min<std::uint32_t>(mss, 40000U - received);
        receive(peer_.data(aloe::frames::pattern(length)));
        received += length;
    }
    auto frames = collect();
    ASSERT_FALSE(frames.empty());
    EXPECT_EQ(frames.back().tcp->window, budget - 40000U) << "25535 on offer";
    EXPECT_EQ(c->unread().size(), 40000U);
    c->consume(30000);
    frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_TRUE(aloe::testing::is_flags(frames[0], {TcpFlag::Ack}));
    EXPECT_TRUE(aloe::frames::tcp_payload(frames[0]).empty());
    EXPECT_EQ(frames[0].tcp->window, 55535U) << "55535 is more than twice 25535";
    EXPECT_TRUE(collect().empty()) << "once";
}

TEST_P(TcpWindow, ADuplicateAckRestatesTheEdgeAfterAConsume) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    for (int i = 0; i < 3; ++i) {
        receive(peer_.data(aloe::frames::pattern(1000)));
    }
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, budget - 3000U);
    c->consume(3000);
    EXPECT_TRUE(collect().empty()) << "65535 is not twice 62535";
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack},
                          aloe::frames::pattern(1000),
                          TcpSequence{aloe::testing::tcp_peer_isn + 1},
                          peer_.rcv_nxt()));
    EXPECT_EQ(tcp_->counters().dropped_duplicate, 1U);
    frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, budget - 3000U) << "a duplicate ACK stays a duplicate: the edge is restated";
    receive(peer_.data(aloe::frames::pattern(1000)));
    EXPECT_TRUE(collect_queued().empty());
    frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, budget - 1000U) << "the ACK at the flush is ordinary: the edge moves";
}

TEST_P(TcpWindow, APeerWithASmallMssIsAcknowledgedEverySecondSegment) {
    aloe::frames::TcpSpec spec = aloe::testing::peer_spec();
    spec.mss                   = 536;
    aloe::frames::TcpPeer small{spec, TcpSequence{aloe::testing::tcp_peer_isn}};
    auto* c = open_passive(small);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->mss(), 536U);
    receive(small.data(aloe::frames::pattern(536)));
    EXPECT_TRUE(collect_queued().empty());
    receive(small.data(aloe::frames::pattern(536)));
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U) << "the second segment of the peer's full size is acknowledged at once";
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 1 + (2 * 536)});
}

TEST_P(TcpWindowRefusing, RefusedUpdateLeavesThePreviousOffer) {
    constexpr std::uint16_t mss = aloe::testing::tcp_fabric_mss;
    configure_tcp({.receive_segments = 4});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(mss)));
    receive(peer_.data(aloe::frames::pattern(mss)));
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    peer_.see(frames[0]);
    EXPECT_EQ(peer_.stack_window(), 0U) << "two full segments fill half of four slots";
    refuse();
    c->consume(2U * mss);
    EXPECT_TRUE(collect().empty()) << "the update could not leave";
    EXPECT_EQ(tcp_->counters().window_updates, 0U) << "an offer that was not queued is not recorded";
    EXPECT_EQ(tcp_->counters().send_refused, 1U);
    allow();
    frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "still pending: flush retries it";
    EXPECT_EQ(frames[0].tcp->window, 2U * mss);
    EXPECT_EQ(tcp_->counters().window_updates, 1U);
}

TEST_P(TcpWindowRefusing, AProbeAnsweredAtOnceKeepsTheReopeningTheFlushOwes) {
    constexpr std::uint16_t mss = aloe::testing::tcp_fabric_mss;
    configure_tcp({.receive_segments = 4});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(mss)));
    receive(peer_.data(aloe::frames::pattern(mss)));
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, 0U);
    refuse();
    c->consume(2U * mss);
    EXPECT_TRUE(collect().empty()) << "the reopening could not leave";
    allow();
    // The peer's zero-window probe: below RCV.NXT, out of the closed window, answered at once.
    receive(peer_.segment({TcpFlag::Ack}, {}, peer_.snd_nxt() + 0xffffffffU, peer_.rcv_nxt()));
    EXPECT_EQ(tcp_->counters().dropped_out_of_window, 1U);
    frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, 0U) << "the answer restates the edge";
    frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "the restating ACK did not cancel the update the flush owes";
    EXPECT_EQ(frames[0].tcp->window, 2U * mss);
    EXPECT_TRUE(collect().empty()) << "once";
}

TEST_P(TcpWindow, ADuplicateBeforeTheFlushKeepsTheUpdateAConsumeOwes) {
    constexpr std::uint16_t mss = aloe::testing::tcp_fabric_mss;
    configure_tcp({.receive_segments = 4});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(mss)));
    receive(peer_.data(aloe::frames::pattern(mss)));
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, 0U);
    c->consume(2U * mss);  // owes an update at the next flush
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack},
                          aloe::frames::pattern(10),
                          TcpSequence{aloe::testing::tcp_peer_isn + 1},
                          peer_.rcv_nxt()));
    EXPECT_EQ(tcp_->counters().dropped_duplicate, 1U);
    frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, 0U) << "a duplicate ACK stays a duplicate";
    frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "the flush still sends the update";
    EXPECT_EQ(frames[0].tcp->window, 2U * mss);
}

TEST_P(TcpWindow, ARestatingAckSatisfiesTheAckOfDataItCovers) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(100)));  // its ACK waits for the flush
    EXPECT_TRUE(collect_queued().empty());
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack},
                          aloe::frames::pattern(10),
                          TcpSequence{aloe::testing::tcp_peer_isn + 1},
                          peer_.rcv_nxt()));
    EXPECT_EQ(tcp_->counters().dropped_duplicate, 1U);
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 101});
    EXPECT_EQ(frames[0].tcp->window, budget - 100U);
    EXPECT_TRUE(collect().empty()) << "the ACK at once covered the data: nothing identical follows at the flush";
    EXPECT_EQ(c->unread().size(), 100U);
}
