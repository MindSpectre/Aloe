#include <algorithm>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>

namespace {

    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;
    using aloe::wire::TcpSequence;
    using Block  = aloe::wire::TcpSackBlock;
    using Blocks = std::vector<Block>;

    constexpr TcpFlags data_flags{TcpFlag::Psh, TcpFlag::Ack};

    /// The SACK blocks a frame carries, in the peer's sequence numbers.
    [[nodiscard]] Blocks sacks(const aloe::frames::ParsedFrame& frame) {
        return aloe::frames::sack_blocks_of(frame);
    }

    [[nodiscard]] bool is_pure_ack(const aloe::frames::ParsedFrame& frame) {
        return aloe::testing::is_flags(frame, {TcpFlag::Ack}) && aloe::frames::tcp_payload(frame).empty();
    }

    [[nodiscard]] std::size_t pure_acks(const std::vector<aloe::frames::ParsedFrame>& frames) {
        return static_cast<std::size_t>(std::ranges::count_if(frames, is_pure_ack));
    }

    /// The peer of every test here offers timestamps, SACK-permitted and window scaling, unless the test says not.
    template <typename Fixture>
    class SackPeer : public Fixture {
    protected:
        SackPeer() {
            this->peer_.set_timestamps(100);
            this->peer_.set_sack_permitted(true);
            this->peer_.set_window_scale(7);
        }

        /// Sequence `offset` past the peer's first byte of data after the handshake.
        [[nodiscard]] TcpSequence at(const std::uint32_t offset) const {
            return base_ + offset;
        }

        [[nodiscard]] Block range(const std::uint32_t first, const std::uint32_t end) const {
            return Block{.first = at(first), .end = at(end)};
        }

        /// A passive open, remembering where the peer's data starts.
        [[nodiscard]] auto* open() {
            auto* c = this->open_passive(this->peer_);
            base_   = this->peer_.snd_nxt();
            return c;
        }

        /// `length` bytes at `at(offset)` with `flags`.
        [[nodiscard]] std::vector<std::byte>
        segment_at(const std::uint32_t offset, const std::uint16_t length, const TcpFlags flags = data_flags) const {
            return this->peer_.segment(flags, aloe::frames::pattern(length), at(offset), this->peer_.rcv_nxt());
        }

        /// The frames sent from the harness together, then one receive pass over them all: one `process` call.
        void burst(const std::vector<std::vector<std::byte>>& frames) {
            for (const std::vector<std::byte>& frame : frames) {
                auto packet = this->harness_->allocate(0);
                ASSERT_TRUE(packet.has_value());
                ASSERT_TRUE(aloe::frames::fill(*packet, frame));
                std::array<aloe::fabric::Packet, 1> out{std::move(*packet)};
                ASSERT_EQ(this->harness_->transmit(0, out), 1U);
            }
            this->process_pending();
        }

        /// `count` adjacent segments of `length` bytes from `at(first)` on, in one burst.
        void burst_above(const std::uint32_t first, const std::size_t count, const std::uint16_t length) {
            std::vector<std::vector<std::byte>> frames;
            frames.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                frames.push_back(segment_at(first + static_cast<std::uint32_t>(index * length), length));
            }
            burst(frames);
        }

        /// No drop and no refused send: a test whose pool runs dry fails here rather than by a count.
        void expect_nothing_lost() {
            const auto& counters = this->tcp_->counters();
            EXPECT_EQ(counters.send_refused, 0U);
            EXPECT_EQ(counters.dropped_no_node, 0U);
            EXPECT_EQ(counters.dropped_no_slot, 0U);
            EXPECT_EQ(counters.dropped_out_of_order, 0U);
            EXPECT_EQ(counters.dropped_out_of_window, 0U);
        }

        TcpSequence base_{};
    };

    using TcpSack         = SackPeer<aloe::testing::TcpFixture>;
    using TcpSackRefusing = SackPeer<aloe::testing::TcpRefusingFixture>;

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpSack,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpSackRefusing,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

}  // namespace

TEST_P(TcpSack, EveryPureAckCarriesTheBlocksWhileDataIsHeld) {
    // A budget of 4000 bytes, so a consume can double the window: the shift is 0 and the fields count bytes.
    configure_tcp({.receive_window = 4000});
    auto* c = open();
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->sack());
    for (int index = 0; index < 3; ++index) {
        receive(peer_.data(aloe::frames::pattern(1000)));
    }
    std::ignore = collect();
    receive(segment_at(3500, 100));
    auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, at(3000));
    EXPECT_EQ(sacks(frames[0]), (Blocks{range(3500, 3600)}));
    const auto options = aloe::frames::tcp_options_of(frames[0]);
    ASSERT_EQ(options.size(), 12U + 12U) << "nop, nop, TS, then nop, nop, sack and one block";
    EXPECT_EQ(std::to_integer<int>(options[2]), 8) << "the timestamps first";
    EXPECT_EQ(std::to_integer<int>(options[14]), 5) << "the SACK option after them";
    EXPECT_EQ(frames[0].tcp->window, 1000U) << "the edge restated";

    c->consume(3000);  // 4000 on offer against 1000: an update is owed to the flush
    EXPECT_TRUE(collect_queued().empty());
    frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, 4000U);
    EXPECT_EQ(sacks(frames[0]), (Blocks{range(3500, 3600)})) << "the update carries the blocks too";

    receive(segment_at(3000, 500));  // the gap fills
    frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, at(3600));
    EXPECT_TRUE(sacks(frames[0]).empty()) << "nothing held: no blocks";
    EXPECT_EQ(aloe::frames::tcp_options_of(frames[0]).size(), 12U);
    EXPECT_EQ(tcp_->counters().sack_acks_sent, 2U);
    EXPECT_EQ(tcp_->counters().duplicate_sacks_sent, 0U);
}

TEST_P(TcpSack, ADataSegmentCarriesNoBlocksAndTheOwedAckStillGoes) {
    auto* c = open();
    ASSERT_NE(c, nullptr);
    burst_above(1000, 5, 100);
    EXPECT_EQ(pure_acks(collect_queued()), 3U) << "the fourth and fifth are owed to the flush";
    ASSERT_EQ(c->send(aloe::frames::pattern(10)), 10U);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 2U);
    EXPECT_EQ(aloe::frames::tcp_payload(frames[0]).size(), 10U);
    EXPECT_TRUE(sacks(frames[0]).empty()) << "a data segment never carries blocks";
    EXPECT_EQ(aloe::frames::tcp_options_of(frames[0]).size(), 12U);
    ASSERT_TRUE(is_pure_ack(frames[1])) << "the data segment did not cancel the ACK that owes blocks";
    EXPECT_EQ(sacks(frames[1]), (Blocks{range(1000, 1500)}));
    expect_nothing_lost();
}

TEST_P(TcpSack, ABurstAboveAGapIsAnsweredByThreeAcksAndOneAtTheFlush) {
    auto* c = open();
    ASSERT_NE(c, nullptr);
    burst_above(100, 30, 10);
    EXPECT_EQ(c->out_of_order_segments(), 30U);
    const auto now = collect_queued();
    ASSERT_EQ(now.size(), 3U);
    for (const auto& frame : now) {
        EXPECT_TRUE(is_pure_ack(frame));
        EXPECT_EQ(frame.tcp->acknowledgement, at(0));
        EXPECT_EQ(frame.tcp->window, now[0].tcp->window) << "each restates the edge";
    }
    EXPECT_EQ(sacks(now[2]), (Blocks{range(100, 130)})) << "the third saw three segments";
    const auto flushed = collect();
    ASSERT_EQ(flushed.size(), 1U);
    ASSERT_TRUE(is_pure_ack(flushed[0]));
    const Blocks blocks = sacks(flushed[0]);
    ASSERT_FALSE(blocks.empty());
    EXPECT_EQ(blocks[0], range(100, 400)) << "all thirty";
    EXPECT_EQ(tcp_->counters().sack_acks_sent, 4U);
    EXPECT_EQ(tcp_->counters().out_of_order_held, 30U);
    expect_nothing_lost();
}

TEST_P(TcpSack, WithoutSackEverySegmentAboveAGapIsAnsweredAtOnce) {
    peer_.set_sack_permitted(false);
    auto* c = open();
    ASSERT_NE(c, nullptr);
    ASSERT_FALSE(c->sack());
    burst_above(100, 30, 10);
    const auto now = collect_queued();
    EXPECT_EQ(pure_acks(now), 30U);
    for (const auto& frame : now) {
        EXPECT_TRUE(sacks(frame).empty());
        EXPECT_EQ(aloe::frames::tcp_options_of(frame).size(), 12U);
    }
    EXPECT_TRUE(collect().empty()) << "nothing owed to the flush";
    EXPECT_EQ(tcp_->counters().sack_acks_sent, 0U);
    expect_nothing_lost();
}

TEST_P(TcpSack, TheCountOfThreeStartsAgainWhenRcvNxtMoves) {
    auto* c = open();
    ASSERT_NE(c, nullptr);
    burst_above(100, 2, 10);
    EXPECT_EQ(pure_acks(collect_queued()), 2U);
    receive(peer_.segment({TcpFlag::Ack}, {}, at(1000), peer_.rcv_nxt()));  // the peer's pure ACK during the hole
    EXPECT_TRUE(collect_queued().empty()) << "not answered";
    burst_above(120, 3, 10);
    EXPECT_EQ(pure_acks(collect_queued()), 1U) << "the pure ACK did not count: the third segment is answered at once";
    EXPECT_EQ(pure_acks(collect()), 1U);
    receive(segment_at(0, 100));  // fills the gap: the run joins, RCV.NXT moves to 150
    auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, at(150));
    burst_above(300, 5, 10);
    frames = collect_queued();
    EXPECT_EQ(pure_acks(frames), 3U) << "three at once again";
    EXPECT_EQ(pure_acks(collect()), 1U);
    expect_nothing_lost();
}

TEST_P(TcpSack, ADuplicateBelowRcvNxtIsReportedFirst) {
    auto* c = open();
    ASSERT_NE(c, nullptr);
    receive(segment_at(0, 100));
    std::ignore = collect();
    receive(segment_at(0, 100));
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, at(100));
    EXPECT_EQ(sacks(frames[0]), (Blocks{range(0, 100)}));
    EXPECT_EQ(tcp_->counters().duplicate_sacks_sent, 1U);
    EXPECT_EQ(tcp_->counters().sack_acks_sent, 1U);
    EXPECT_TRUE(collect().empty()) << "reported once";
}

TEST_P(TcpSack, ADuplicatePrefixIsReportedAtTheOrdinaryAck) {
    auto* c = open();
    ASSERT_NE(c, nullptr);
    receive(segment_at(0, 200));
    std::ignore = collect();
    receive(segment_at(100, 200));  // 100 old bytes, then 100 new ones
    EXPECT_TRUE(collect_queued().empty()) << "in-order data: its ACK waits for the flush";
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, at(300));
    EXPECT_EQ(sacks(frames[0]), (Blocks{range(100, 200)}));
    EXPECT_EQ(c->unread().size(), 300U);
    EXPECT_EQ(tcp_->counters().duplicate_sacks_sent, 1U);
}

TEST_P(TcpSack, ADuplicateOfHeldDataIsFollowedByItsBlock) {
    auto* c = open();
    ASSERT_NE(c, nullptr);
    receive(segment_at(500, 200));
    std::ignore = collect();
    receive(segment_at(550, 100));
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(sacks(frames[0]), (Blocks{range(550, 650), range(500, 700)}));
    EXPECT_EQ(tcp_->counters().dropped_duplicate, 1U);
    EXPECT_EQ(tcp_->counters().duplicate_sacks_sent, 1U);
}

TEST_P(TcpSack, HeldDataAnInOrderSegmentOverlappedIsReportedAtTheJoin) {
    auto* c = open();
    ASSERT_NE(c, nullptr);
    receive(segment_at(500, 500));
    std::ignore = collect();
    receive(segment_at(0, 750));
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, at(1000));
    EXPECT_EQ(sacks(frames[0]), (Blocks{range(500, 750)})) << "the held part RCV.NXT had passed";
    EXPECT_EQ(c->unread().size(), 1000U);
}

TEST_P(TcpSack, AReleaseForRoomCutsTheBlockInTheSameAck) {
    configure_tcp({.receive_segments = 4});
    auto* c = open();
    ASSERT_NE(c, nullptr);
    for (const std::uint32_t offset : {500U, 1000U, 1500U, 2000U}) {
        receive(segment_at(offset, 100));
    }
    auto frames = collect();
    ASSERT_FALSE(frames.empty());
    EXPECT_EQ(sacks(frames.back()), (Blocks{range(2000, 2100), range(1500, 1600), range(1000, 1100)}));
    receive(segment_at(200, 100));  // below them all: takes the node of the highest
    EXPECT_EQ(tcp_->counters().out_of_order_released, 1U);
    frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    const Blocks blocks = sacks(frames[0]);
    EXPECT_EQ(blocks, (Blocks{range(200, 300), range(1500, 1600), range(1000, 1100)}));
    for (const Block& block : blocks) {
        EXPECT_FALSE(block.first.before(at(2100)) && at(2000).before(block.end)) << "the released range is gone";
    }
}

TEST_P(TcpSack, ABareFinAboveAGapIsABlockOfItsOwn) {
    auto* c = open();
    ASSERT_NE(c, nullptr);
    receive(segment_at(500, 100));
    std::ignore = collect();
    receive(peer_.segment({TcpFlag::Fin, TcpFlag::Ack}, {}, at(1000), peer_.rcv_nxt()));
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(sacks(frames[0]), (Blocks{range(1000, 1001), range(500, 600)}));
}

TEST_P(TcpSack, AFinOnAHeldSegmentEndsItsBlock) {
    auto* c = open();
    ASSERT_NE(c, nullptr);
    receive(segment_at(500, 100, {TcpFlag::Fin, TcpFlag::Psh, TcpFlag::Ack}));
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(sacks(frames[0]), (Blocks{range(500, 601)}));
}

TEST_P(TcpSack, AFinBelowTheRememberedOneReplacesItInTheBlocks) {
    auto* c = open();
    ASSERT_NE(c, nullptr);
    receive(segment_at(1000, 1000, {TcpFlag::Fin, TcpFlag::Psh, TcpFlag::Ack}));
    auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(sacks(frames[0]), (Blocks{range(1000, 2001)}));
    receive(peer_.segment({TcpFlag::Fin, TcpFlag::Ack}, {}, at(500), peer_.rcv_nxt()));
    frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(sacks(frames[0]), (Blocks{range(500, 501), range(1000, 2000)}))
        << "the forgotten FIN's number is no longer reported";
}

TEST_P(TcpSack, ThreeBlocksBesideTimestampsAndFourWithout) {
    auto* c = open();
    ASSERT_NE(c, nullptr);
    for (const std::uint32_t offset : {500U, 1000U, 1500U, 2000U}) {
        receive(segment_at(offset, 100));
    }
    auto frames = collect();
    ASSERT_FALSE(frames.empty());
    EXPECT_EQ(sacks(frames.back()), (Blocks{range(2000, 2100), range(1500, 1600), range(1000, 1100)}));
    EXPECT_EQ(aloe::frames::tcp_options_of(frames.back()).size(), 12U + 4U + (3U * 8U));

    // A second connection, whose peer offers no timestamps: four blocks fit.
    aloe::frames::TcpPeer other{aloe::testing::peer_spec(40001), TcpSequence{5000U}};
    other.set_sack_permitted(true);
    other.set_window_scale(7);
    auto* d = open_passive(other);
    ASSERT_NE(d, nullptr);
    ASSERT_TRUE(d->sack());
    const TcpSequence first = other.snd_nxt();
    for (const std::uint32_t offset : {500U, 1000U, 1500U, 2000U}) {
        receive(other.segment(data_flags, aloe::frames::pattern(100), first + offset, other.rcv_nxt()));
    }
    frames = collect();
    ASSERT_FALSE(frames.empty());
    const auto block = [first](const std::uint32_t from, const std::uint32_t to) {
        return Block{.first = first + from, .end = first + to};
    };
    EXPECT_EQ(sacks(frames.back()), (Blocks{block(2000, 2100), block(1500, 1600), block(1000, 1100), block(500, 600)}));
    EXPECT_EQ(aloe::frames::tcp_options_of(frames.back()).size(), 4U + (4U * 8U));
}

TEST_P(TcpSackRefusing, ARefusedAckKeepsItsReportForTheFlush) {
    auto* c = open();
    ASSERT_NE(c, nullptr);
    receive(segment_at(0, 100));
    std::ignore = collect();
    refuse();
    receive(segment_at(500, 100));  // above the gap first: a later payload segment would forget the duplicate
    receive(segment_at(0, 100));    // then a duplicate below RCV.NXT
    EXPECT_TRUE(collect().empty()) << "every send refused";
    EXPECT_EQ(tcp_->counters().sack_acks_sent, 0U);
    EXPECT_EQ(tcp_->counters().duplicate_sacks_sent, 0U);
    allow();
    const auto frames = collect();
    ASSERT_EQ(pure_acks(frames), 1U) << "the ACK owed to the flush, an ordinary one";
    const auto ack = std::ranges::find_if(frames, is_pure_ack);
    EXPECT_EQ(sacks(*ack), (Blocks{range(0, 100), range(500, 600)})) << "the duplicate first, then the block";
    EXPECT_EQ(tcp_->counters().sack_acks_sent, 1U);
    EXPECT_EQ(tcp_->counters().duplicate_sacks_sent, 1U);
    EXPECT_TRUE(collect().empty());
}

TEST_P(TcpSack, AProbeAndAResetCarryNoBlocksTheyDoNotOwe) {
    auto* c = open();
    ASSERT_NE(c, nullptr);
    receive(segment_at(500, 100));
    std::ignore = collect();
    c->abort();
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_TRUE(aloe::testing::is_flags(frames[0], {TcpFlag::Rst, TcpFlag::Ack}));
    EXPECT_TRUE(sacks(frames[0]).empty());
    EXPECT_EQ(aloe::frames::tcp_options_of(frames[0]).size(), 12U);
}
