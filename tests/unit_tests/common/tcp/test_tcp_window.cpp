#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
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
    EXPECT_EQ(tcp_->byte_budget(), 46720U) << "32 packets at MSS 1460";
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
    EXPECT_EQ(tcp_->counters().window_updates, 0U);
    c->consume(50);
    frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "consumption that frees credit is a window update";
    EXPECT_EQ(frames[0].tcp->window, budget - 51U);
    EXPECT_EQ(right_edge(frames[0]), initial_edge + 50U);
    EXPECT_EQ(tcp_->counters().window_updates, 1U);
    EXPECT_TRUE(collect().empty()) << "once";
}

TEST_P(TcpWindow, PartialConsumeFreesNoNodeButMovesTheEdge) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const std::size_t nodes = tcp_->nodes_available();
    receive(peer_.data(aloe::frames::pattern(10)));
    std::ignore = collect();
    c->consume(4);
    EXPECT_EQ(tcp_->nodes_available(), nodes - 1) << "the packet still holds six bytes";
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, budget - 6U);
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
    configure_tcp({.receive_segments = 1});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(tcp_->byte_budget(), 1460U);
    receive(peer_.data(aloe::frames::pattern(1460)));
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
    c->consume(1460);
    frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, 1460U) << "reopened";
    EXPECT_EQ(tcp_->counters().window_updates, 1U);
    receive(peer_.data(aloe::frames::pattern(1)));
    EXPECT_EQ(c->unread().size(), 1U);
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
    const std::uint32_t credit = 4U * aloe::testing::tcp_fabric_mss - 4U;
    EXPECT_EQ(frames[0].tcp->window, credit);
    receive(peer_.data(aloe::frames::pattern(1)));
    EXPECT_EQ(tcp_->counters().dropped_no_slot, 1U);
    frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, credit) << "neither extended nor retracted while every slot is held";
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 5});
    c->consume(1);
    frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, credit + 1U) << "a slot and a byte came back";
}

TEST_P(TcpWindow, BudgetIsCappedAtTheWireMaximum) {
    configure_tcp({.receive_segments = 64});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(tcp_->byte_budget(), 65535U);
    EXPECT_EQ(peer_.stack_window(), 65535U);
}

TEST_P(TcpWindowRefusing, RefusedUpdateLeavesThePreviousOffer) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(100)));
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    peer_.see(frames[0]);
    EXPECT_EQ(peer_.stack_window(), budget - 100U);
    refuse();
    c->consume(100);
    EXPECT_TRUE(collect().empty()) << "the update could not leave";
    EXPECT_EQ(tcp_->counters().window_updates, 0U) << "an offer that was not queued is not recorded";
    EXPECT_EQ(tcp_->counters().send_refused, 1U);
    allow();
    frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "still pending: flush retries it";
    EXPECT_EQ(frames[0].tcp->window, budget);
    EXPECT_EQ(tcp_->counters().window_updates, 1U);
}
