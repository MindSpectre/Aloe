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
#include <tcp_sack_brick.hpp>

namespace {

    using namespace std::chrono_literals;
    using aloe::frames::ParsedFrame;
    using aloe::frames::TcpPeer;
    using aloe::tcp::State;
    using aloe::tcp::TcpCounters;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;
    using aloe::wire::TcpSequence;
    using TcpWindowRefusing = aloe::testing::TcpRefusingFixture;

    constexpr std::uint16_t full_segment = aloe::testing::tcp_fabric_mss;

    /// The peer's `payload` bytes at `sequence` acknowledging `acknowledgement`, with the peer's state unchanged.
    [[nodiscard]] std::vector<std::byte> data_at(const TcpPeer& peer,
                                                 const std::size_t payload,
                                                 const TcpSequence sequence,
                                                 const TcpSequence acknowledgement) {
        return peer.segment(
            TcpFlags{TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(payload), sequence, acknowledgement);
    }

    /**
     * The brick over a fabric port, with the cases of the sequence check that read the offered edge (RFC 9293
     * 3.10.7.4): a segment at RCV.NXT whose payload the window does not cover has its ACK read and its payload refused,
     * as the RFC asks and Linux's `tcp_sequence` does not; a bare FIN at RCV.NXT is taken at a closed window, an empty
     * segment exactly at the edge is acceptable and a FIN's own number is not held against the edge, as in Linux's
     * `tcp_sequence` and `tcp_data_queue`. The cases that close the window give the connection four slots,
     * `configure_tcp({.receive_segments = 4})`, so the budget is two full segments of a peer without timestamps.
     */
    class TcpWindow : public aloe::testing::TcpFixture {
    protected:
        /// Opens a connection to `peer` and fills its window with two full segments left unread: with four slots the
        /// window is then zero and RCV.NXT stands at the edge. The peer saw the ACK that closed it. Null on failure.
        [[nodiscard]] Connection* open_with_a_closed_window(TcpPeer& peer) {
            Connection* c = open_passive(peer);
            if (c == nullptr) {
                return nullptr;
            }
            receive(peer.data(aloe::frames::pattern(full_segment)));
            receive(peer.data(aloe::frames::pattern(full_segment)));
            for (const ParsedFrame& frame : collect()) {
                peer.see(frame);
            }
            return c;
        }

        /// `c` sends `count` bytes in one segment, which `peer` sees and leaves unacknowledged.
        void send_unacknowledged(Connection& c, TcpPeer& peer, const std::size_t count) {
            ASSERT_EQ(c.send(aloe::frames::pattern(count)), count);
            for (const ParsedFrame& frame : collect()) {
                peer.see(frame);
            }
            ASSERT_EQ(c.unacknowledged(), count);
        }

        /// The stack has 1000 bytes unacknowledged and its window is zero; the peer's 100 bytes at RCV.NXT acknowledge
        /// them: the segment has no room, so its ACK is read and its payload refused, answered at once with the edge
        /// restated.
        void expect_the_ack_of_a_segment_refused_at_a_zero_window_read(TcpPeer& peer) {
            Connection* c = open_with_a_closed_window(peer);
            ASSERT_NE(c, nullptr);
            ASSERT_EQ(c->receive_window(), 0U);
            ASSERT_NO_FATAL_FAILURE(send_unacknowledged(*c, peer, 1000));
            ASSERT_FALSE(c->events().acked());
            const std::size_t unread = c->unread().size();
            const TcpCounters before = tcp_->counters();
            receive(data_at(peer, 100, peer.snd_nxt(), peer.rcv_nxt()));
            EXPECT_TRUE(c->events().acked());
            EXPECT_EQ(c->unacknowledged(), 0U) << "the ACK freed what it acknowledged";
            EXPECT_EQ(tcp_->counters().dropped_out_of_window, before.dropped_out_of_window + 1);
            EXPECT_EQ(c->unread().size(), unread) << "nothing of the 100 taken";
            const auto frames = collect_queued();
            ASSERT_EQ(frames.size(), 1U) << "one ACK at once";
            EXPECT_EQ(frames[0].tcp->acknowledgement, peer.snd_nxt());
            EXPECT_EQ(frames[0].tcp->window, 0U) << "the edge restated";
        }

        /// The stack's window is zero; the peer's bare FIN at RCV.NXT is taken, and its ACK states no window from the
        /// new RCV.NXT, one past the edge.
        void expect_a_bare_fin_at_a_zero_window_taken(TcpPeer& peer) {
            Connection* c = open_with_a_closed_window(peer);
            ASSERT_NE(c, nullptr);
            ASSERT_EQ(c->receive_window(), 0U);
            const TcpCounters before = tcp_->counters();
            receive(peer.fin());
            EXPECT_TRUE(c->events().peer_closed());
            EXPECT_TRUE(c->peer_closed());
            EXPECT_EQ(c->state(), State::CloseWait);
            EXPECT_EQ(c->receive_window(), 0U) << "RCV.NXT stands one past the edge: no window, not a wrapped one";
            EXPECT_EQ(aloe::testing::drops(tcp_->counters()), aloe::testing::drops(before));
            const auto frames = collect();
            ASSERT_EQ(frames.size(), 1U) << "the FIN acknowledged";
            EXPECT_EQ(frames[0].tcp->acknowledgement, peer.snd_nxt());
            EXPECT_EQ(frames[0].tcp->window, 0U);
        }

        /// With four slots and the window open: a hole of one full segment at RCV.NXT, and one full segment held above
        /// it fills the window to the edge, as a peer whose first segment was lost leaves it. Its ACK acknowledges
        /// nothing new; its answer is collected.
        void fill_above_a_hole(Connection& c, TcpPeer& peer) {
            ASSERT_EQ(c.receive_window(), 2U * full_segment);
            receive(data_at(peer, full_segment, peer.snd_nxt() + full_segment, c.acknowledged()));
            ASSERT_EQ(c.out_of_order_segments(), 1U) << "held above the hole, up to the edge";
            std::ignore = collect();
        }

        /// The stack has 1000 bytes unacknowledged and the window is filled above a hole (`fill_above_a_hole`). The
        /// peer's pure ACK at the edge, where such a peer sends its ACKs from, acknowledges the 1000: it is accepted
        /// and read, and draws no answer.
        void expect_an_empty_segment_at_the_edge_accepted(TcpPeer& peer) {
            Connection* c = open_passive(peer);
            ASSERT_NE(c, nullptr);
            ASSERT_NO_FATAL_FAILURE(send_unacknowledged(*c, peer, 1000));
            const TcpSequence edge = peer.snd_nxt() + c->receive_window();
            ASSERT_NO_FATAL_FAILURE(fill_above_a_hole(*c, peer));
            ASSERT_FALSE(c->events().acked());
            const TcpCounters before = tcp_->counters();
            receive(peer.segment(TcpFlags{TcpFlag::Ack}, {}, edge, peer.rcv_nxt()));
            EXPECT_TRUE(c->events().acked());
            EXPECT_EQ(c->unacknowledged(), 0U);
            EXPECT_EQ(aloe::testing::drops(tcp_->counters()), aloe::testing::drops(before)) << "no drop counter";
            EXPECT_TRUE(collect().empty()) << "no answer";
        }
    };

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

TEST_P(TcpWindow, TheAckOfASegmentRefusedAtAZeroWindowIsRead) {
    configure_tcp({.receive_segments = 4});
    expect_the_ack_of_a_segment_refused_at_a_zero_window_read(peer_);
}

TEST_P(TcpWindow, TheAckOfASegmentOneByteTooLongIsRead) {
    configure_tcp({.receive_segments = 4});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(full_segment)));
    receive(peer_.data(aloe::frames::pattern(full_segment - 500U)));
    for (const ParsedFrame& frame : collect()) {
        peer_.see(frame);
    }
    ASSERT_EQ(c->receive_window(), 500U) << "two full segments of budget, less 2420 bytes unread";
    ASSERT_NO_FATAL_FAILURE(send_unacknowledged(*c, peer_, 1000));
    ASSERT_FALSE(c->events().acked());
    const std::size_t unread = c->unread().size();
    receive(data_at(peer_, 501, peer_.snd_nxt(), peer_.rcv_nxt()));
    EXPECT_TRUE(c->events().acked());
    EXPECT_EQ(c->unacknowledged(), 0U) << "the ACK freed what it acknowledged";
    EXPECT_EQ(tcp_->counters().dropped_out_of_window, 1U);
    EXPECT_EQ(c->unread().size(), unread) << "nothing of the 501 taken";
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U) << "one ACK at once";
    EXPECT_EQ(frames[0].tcp->acknowledgement, peer_.snd_nxt());
    EXPECT_EQ(frames[0].tcp->window, 500U) << "the edge restated";
    receive(data_at(peer_, 500, peer_.snd_nxt(), peer_.rcv_nxt()));
    EXPECT_EQ(c->unread().size(), unread + 500U) << "500 bytes end at the edge: taken";
    EXPECT_EQ(tcp_->counters().dropped_out_of_window, 1U);
}

TEST_P(TcpWindow, TheWindowFieldOfARefusedSegmentIsRead) {
    configure_tcp({.receive_segments = 4});
    peer_.set_window(1000);
    auto* c = open_with_a_closed_window(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_NO_FATAL_FAILURE(send_unacknowledged(*c, peer_, 1000));
    ASSERT_EQ(c->writable(), 0U) << "the peer's window of 1000 is full";
    while (poll().has_value()) {}
    // Refused data that acknowledges nothing new and states the window as it stands.
    receive(data_at(peer_, 100, peer_.snd_nxt(), c->acknowledged()));
    EXPECT_EQ(tcp_->counters().dropped_out_of_window, 1U);
    EXPECT_EQ(tcp_->counters().duplicate_acks, 0U) << "an ACK that carries data is no duplicate, refused or not";
    EXPECT_FALSE(c->events().writable());
    EXPECT_EQ(collect_queued().size(), 1U) << "answered at once";
    peer_.set_window(2000);
    receive(data_at(peer_, 100, peer_.snd_nxt(), c->acknowledged()));
    EXPECT_EQ(tcp_->counters().dropped_out_of_window, 2U);
    EXPECT_TRUE(c->events().writable()) << "the window twice the last opens room";
    EXPECT_EQ(c->writable(), 1000U);
    EXPECT_EQ(c->unacknowledged(), 1000U);
    EXPECT_EQ(collect_queued().size(), 1U) << "answered at once again in the same tick: data is never limited";
    EXPECT_EQ(tcp_->counters().acks_rate_limited, 0U);
}

TEST_P(TcpWindow, ASegmentAboveAGapPastTheEdgeIsStillRefusedWhole) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_NO_FATAL_FAILURE(send_unacknowledged(*c, peer_, 1000));
    const TcpSequence edge = peer_.snd_nxt() + c->receive_window();
    // From 100 below the edge to 900 past it, with a gap before it: refused by the sequence check, its ACK with it.
    receive(data_at(peer_, 1000, TcpSequence{edge.value - 100U}, peer_.rcv_nxt()));
    EXPECT_EQ(c->unacknowledged(), 1000U) << "the new ACK is not read";
    EXPECT_FALSE(c->events().acked());
    EXPECT_EQ(tcp_->counters().dropped_out_of_window, 1U);
    EXPECT_EQ(c->out_of_order_segments(), 0U);
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U) << "data is answered at once";
    EXPECT_EQ(frames[0].tcp->acknowledgement, peer_.snd_nxt());
}

TEST_P(TcpWindow, ABareFinAtAZeroWindowIsTaken) {
    configure_tcp({.receive_segments = 4});
    expect_a_bare_fin_at_a_zero_window_taken(peer_);
}

TEST_P(TcpWindow, DataWithAFinAtAZeroWindowTakesNeither) {
    configure_tcp({.receive_segments = 4});
    auto* c = open_with_a_closed_window(peer_);
    ASSERT_NE(c, nullptr);
    const std::size_t unread = c->unread().size();
    receive(peer_.segment(TcpFlags{TcpFlag::Fin, TcpFlag::Psh, TcpFlag::Ack},
                          aloe::frames::pattern(10),
                          peer_.snd_nxt(),
                          peer_.rcv_nxt()));
    EXPECT_FALSE(c->peer_closed()) << "the FIN behind refused data is not taken";
    EXPECT_FALSE(c->events().peer_closed());
    EXPECT_EQ(c->state(), State::Established);
    EXPECT_EQ(c->unread().size(), unread);
    EXPECT_EQ(tcp_->counters().dropped_out_of_window, 1U);
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U) << "answered at once";
    EXPECT_EQ(frames[0].tcp->acknowledgement, peer_.snd_nxt()) << "neither the data nor the FIN";
    EXPECT_EQ(frames[0].tcp->window, 0U);
}

TEST_P(TcpWindow, AFinOnePastTheEdgeIsTaken) {
    configure_tcp({.receive_segments = 4});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(full_segment)));
    std::ignore = collect();
    ASSERT_EQ(c->receive_window(), full_segment);
    // The payload ends at the edge and the FIN stands one past it: a FIN's number takes no slot.
    const TcpSequence at = peer_.snd_nxt();
    receive(peer_.segment(
        TcpFlags{TcpFlag::Fin, TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(full_segment), at, peer_.rcv_nxt()));
    EXPECT_EQ(c->unread().size(), 2U * full_segment) << "the payload held";
    EXPECT_TRUE(c->events().peer_closed());
    EXPECT_EQ(c->state(), State::CloseWait);
    EXPECT_EQ(tcp_->counters().dropped_out_of_window, 0U);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, at + full_segment + 1U) << "the payload and the FIN";
    EXPECT_EQ(frames[0].tcp->window, 0U);
}

TEST_P(TcpWindow, AFinOnePastTheEdgeAboveAGapIsRemembered) {
    configure_tcp({.receive_segments = 4});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence base = peer_.snd_nxt();
    // A hole of one full segment at RCV.NXT, then one that ends at the edge with its FIN one past it.
    receive(peer_.segment(TcpFlags{TcpFlag::Fin, TcpFlag::Psh, TcpFlag::Ack},
                          aloe::frames::pattern(full_segment),
                          base + full_segment,
                          peer_.rcv_nxt()));
    EXPECT_EQ(c->out_of_order_segments(), 1U) << "held";
    EXPECT_FALSE(c->events().peer_closed()) << "the FIN is remembered, not taken";
    EXPECT_EQ(tcp_->counters().dropped_out_of_window, 0U);
    std::ignore = collect();
    receive(peer_.data(aloe::frames::pattern(full_segment)));  // fills the hole
    EXPECT_TRUE(c->events().peer_closed());
    EXPECT_EQ(c->state(), State::CloseWait);
    EXPECT_EQ(c->unread().size(), 2U * full_segment);
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U) << "one ACK, at once, after the join";
    EXPECT_EQ(frames[0].tcp->acknowledgement, base + (2U * full_segment) + 1U);
}

TEST_P(TcpWindow, AnEmptySegmentAtTheEdgeIsAccepted) {
    configure_tcp({.receive_segments = 4});
    expect_an_empty_segment_at_the_edge_accepted(peer_);
}

TEST_P(TcpWindow, AnEmptySegmentOnePastTheEdgeIsRefused) {
    configure_tcp({.receive_segments = 4});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_NO_FATAL_FAILURE(send_unacknowledged(*c, peer_, 1000));
    const TcpSequence edge = peer_.snd_nxt() + c->receive_window();
    ASSERT_NO_FATAL_FAILURE(fill_above_a_hole(*c, peer_));
    const auto past = peer_.segment(TcpFlags{TcpFlag::Ack}, {}, edge + 1U, peer_.rcv_nxt());
    receive(past);
    EXPECT_EQ(tcp_->counters().dropped_out_of_window, 1U);
    EXPECT_EQ(c->unacknowledged(), 1000U) << "its ACK goes with it";
    EXPECT_FALSE(c->events().acked());
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "the first answer of the interval";
    EXPECT_EQ(frames[0].tcp->acknowledgement, peer_.snd_nxt());
    advance(100ms);
    receive(past);
    EXPECT_TRUE(collect().empty()) << "answered through the limiter: held back 100 ms after the first";
    EXPECT_EQ(tcp_->counters().acks_rate_limited, 1U);
    EXPECT_EQ(tcp_->counters().dropped_out_of_window, 2U);
}

TEST_P(TcpWindow, TheEdgeTestsHoldAcrossTheTopOfTheSequenceSpace) {
    configure_tcp({.receive_segments = 4});
    // Two full segments from 0xfffff458 close the window at 0xffffffc0: the refused 100 bytes run past zero.
    TcpPeer refused{aloe::testing::peer_spec(aloe::testing::tcp_peer_port + 1), TcpSequence{0xfffff457U}};
    ASSERT_NO_FATAL_FAILURE(expect_the_ack_of_a_segment_refused_at_a_zero_window_read(refused));
    EXPECT_EQ(refused.snd_nxt().value, 0xffffffc0U);
    while (poll().has_value()) {}  // the next open expects its own Accepted first
    // The window closes at 0xffffffff: the bare FIN takes RCV.NXT to zero, one past the edge.
    TcpPeer fin{aloe::testing::peer_spec(aloe::testing::tcp_peer_port + 2), TcpSequence{0xfffff496U}};
    ASSERT_NO_FATAL_FAILURE(expect_a_bare_fin_at_a_zero_window_taken(fin));
    EXPECT_EQ(fin.snd_nxt().value, 0U);
    while (poll().has_value()) {}
    // RCV.NXT at 0xfffffa00, the hole up to 0xffffffb4, the edge at 0x568.
    TcpPeer empty{aloe::testing::peer_spec(aloe::testing::tcp_peer_port + 3), TcpSequence{0xfffff9ffU}};
    ASSERT_NO_FATAL_FAILURE(expect_an_empty_segment_at_the_edge_accepted(empty));
    EXPECT_EQ(empty.snd_nxt().value, 0xfffffa00U);
}
