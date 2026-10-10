#include <aloe/frames>
#include <aloe/stream>
#include <aloe/tcp>
#include <aloe/wire>
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
    using TcpReceive = aloe::testing::TcpFixture;

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpReceive,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

    [[nodiscard]] std::vector<std::byte> text(const char* s) {
        std::vector<std::byte> out;
        for (; *s != '\0'; ++s) {
            out.push_back(static_cast<std::byte>(*s));
        }
        return out;
    }

    /// The unread bytes copied out: for assertions only; the brick itself never copies.
    template <typename C>
    [[nodiscard]] std::vector<std::byte> unread_of(const C& c) {
        std::vector<std::byte> out;
        for (const std::span<const std::byte> chunk : c.unread()) {
            out.insert(out.end(), chunk.begin(), chunk.end());
        }
        return out;
    }

    template <typename C>
    [[nodiscard]] std::vector<std::size_t> chunk_sizes(const C& c) {
        std::vector<std::size_t> out;
        for (const std::span<const std::byte> chunk : c.unread()) {
            out.push_back(chunk.size());
        }
        return out;
    }

}  // namespace

TEST_P(TcpReceive, ThreeSegmentsGiveThreeChunksAndConsumeCrossesBoundaries) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const std::size_t nodes = tcp_->nodes_available();
    receive(peer_.data(text("abc")));
    auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.readable());
    receive(peer_.data(text("defgh")));
    receive(peer_.data(text("ij")));
    EXPECT_EQ(tcp_->pending_events(), 1U) << "three Readable raises, one entry";
    EXPECT_EQ(c->unread().size(), 10U);
    EXPECT_EQ(chunk_sizes(*c), (std::vector<std::size_t>{3, 5, 2}));
    EXPECT_EQ(unread_of(*c), text("abcdefghij"));
    EXPECT_EQ(tcp_->nodes_available(), nodes - 3);
    EXPECT_TRUE(burst_empty()) << "the held packets were moved out of the burst";
    EXPECT_TRUE(collect_queued().empty()) << "ordinary in-order data waits for flush";

    c->consume(4);
    EXPECT_EQ(c->unread().size(), 6U);
    EXPECT_EQ(chunk_sizes(*c), (std::vector<std::size_t>{4, 2}));
    EXPECT_EQ(unread_of(*c), text("efghij"));
    EXPECT_EQ(tcp_->nodes_available(), nodes - 2) << "the first packet went back";
    c->consume(6);
    EXPECT_TRUE(c->unread().empty());
    EXPECT_EQ(tcp_->nodes_available(), nodes);

    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "one coalesced ACK for the three segments";
    EXPECT_EQ(frames[0].tcp->flags, TcpFlags{TcpFlag::Ack});
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 11});
    EXPECT_EQ(frames[0].tcp->window, aloe::testing::tcp_fabric_budget) << "everything consumed: the full budget";
    EXPECT_EQ(tcp_->counters().pure_acks_sent, 1U);
}

TEST_P(TcpReceive, FinalHandshakeAckCarriesData) {
    ASSERT_TRUE(tcp_->listen(aloe::testing::tcp_listen_port).has_value());
    receive(peer_.syn());
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    peer_.see(frames[0]);
    receive(peer_.data(text("abc")));  // the peer's ACK completing the handshake, with its first bytes
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.accepted());
    EXPECT_TRUE(event->events.readable());
    auto& c = *event->connection;
    EXPECT_EQ(c.state(), aloe::tcp::State::Established);
    EXPECT_EQ(c.unread().size(), 3U);
    EXPECT_EQ(unread_of(c), text("abc"));
    EXPECT_EQ(tcp_->counters().connections_accepted, 1U);
    EXPECT_EQ(tcp_->counters().dropped_out_of_window, 0U) << "the window was offered with the SYN-ACK";
}

TEST_P(TcpReceive, OverlapIsTrimmedByOffsetAndDuplicatesAckedAtOnce) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(text("abcdef")));
    // A retransmission starting three bytes back, carrying three new bytes.
    receive(peer_.segment(
        {TcpFlag::Psh, TcpFlag::Ack}, text("defghi"), TcpSequence{aloe::testing::tcp_peer_isn + 4}, peer_.rcv_nxt()));
    EXPECT_EQ(c->unread().size(), 9U);
    EXPECT_EQ(unread_of(*c), text("abcdefghi"));
    EXPECT_EQ(chunk_sizes(*c), (std::vector<std::size_t>{6, 3})) << "the second chunk starts after the trimmed prefix";
    EXPECT_EQ(tcp_->counters().dropped_duplicate, 0U);
    EXPECT_TRUE(collect_queued().empty());
    // A fully duplicate segment: dropped, and acknowledged before the next segment is processed.
    receive(peer_.segment(
        {TcpFlag::Psh, TcpFlag::Ack}, text("abc"), TcpSequence{aloe::testing::tcp_peer_isn + 1}, peer_.rcv_nxt()));
    EXPECT_EQ(tcp_->counters().dropped_duplicate, 1U);
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 10});
    EXPECT_EQ(c->unread().size(), 9U);
}

TEST_P(TcpReceive, ChecksumVerdicts) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    peer_.spec().checksums = aloe::frames::Checksums::Wrong;
    receive(peer_.data(text("bad")));
    peer_.rewind(3);
    EXPECT_EQ(tcp_->counters().dropped_bad_checksum, 1U)
        << (offloads() ? "the device's Bad verdict" : "the software check");
    EXPECT_TRUE(c->unread().empty());
    EXPECT_TRUE(collect().empty()) << "nothing is acknowledged";
    peer_.spec().checksums = aloe::frames::Checksums::Correct;
    receive(peer_.data(text("good")));
    EXPECT_EQ(c->unread().size(), 4U);
    EXPECT_EQ(tcp_->counters().dropped_bad_checksum, 1U);
}

TEST_P(TcpReceive, Ipv4OptionsShiftThePayload) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(aloe::frames::with_ipv4_options(peer_.data(text("options")), 2));
    EXPECT_EQ(c->unread().size(), 7U);
    EXPECT_EQ(unread_of(*c), text("options"));
}

TEST_P(TcpReceive, BadHeadersAndMalformedOptionsAreDroppedSilently) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    auto frame          = peer_.data(text("x"));
    frame[14 + 20 + 12] = std::byte{0x40};  // data offset 16: under the fixed header
    receive(frame);
    EXPECT_EQ(tcp_->counters().dropped_bad_header, 1U);
    peer_.rewind(1);
    aloe::frames::TcpPeer other{aloe::testing::peer_spec(40001), TcpSequence{2000U}};
    auto syn          = other.syn();
    syn[14 + 20 + 21] = std::byte{0};  // the MSS option with length 0
    receive(syn);
    EXPECT_EQ(tcp_->counters().dropped_bad_header, 2U);
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(tcp_->table_size(), 1U);
    EXPECT_TRUE(c->unread().empty());
}

TEST_P(TcpReceive, SlotCapDropsWithoutAdvancingAndRecovers) {
    configure_tcp({.receive_segments = 2});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(text("a")));
    receive(peer_.data(text("b")));
    receive(peer_.data(text("c")));
    EXPECT_EQ(tcp_->counters().dropped_no_slot, 1U);
    EXPECT_EQ(c->unread().size(), 2U);
    auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U) << "a refused segment is acknowledged at once";
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 3}) << "the last accepted byte";
    EXPECT_EQ(frames[0].tcp->window, 2U * aloe::testing::tcp_fabric_mss - 2U) << "credit is not retracted to zero";
    receive(peer_.data(text("d")));  // a gap now: `c` was never accepted
    EXPECT_EQ(tcp_->counters().dropped_out_of_order, 1U);
    frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 3});
    c->consume(1);
    peer_.rewind(2);
    receive(peer_.data(text("c")));
    EXPECT_EQ(c->unread().size(), 2U);
    EXPECT_EQ(unread_of(*c), text("bc"));
    receive(peer_.data(text("d")));
    EXPECT_EQ(tcp_->counters().dropped_no_slot, 2U) << "two slots, both held";
}

TEST_P(TcpReceive, SharedPoolExhaustionNeverAdvancesRcvNxt) {
    configure_tcp({.receive_pool = 2});
    auto* first = open_passive(peer_);
    ASSERT_NE(first, nullptr);
    aloe::frames::TcpPeer other{aloe::testing::peer_spec(40001), TcpSequence{2000U}};
    auto* second = open_passive(other);
    ASSERT_NE(second, nullptr);
    receive(peer_.data(text("a")));
    receive(peer_.data(text("b")));
    EXPECT_EQ(tcp_->nodes_available(), 0U);
    receive(other.data(text("x")));
    EXPECT_EQ(tcp_->counters().dropped_no_node, 1U);
    EXPECT_TRUE(second->unread().empty());
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{2001U}) << "nothing of the second was accepted";
    EXPECT_EQ(frames[0].tcp->destination_port, 40001U);
    first->consume(2);
    EXPECT_EQ(tcp_->nodes_available(), 2U);
    other.rewind(1);
    receive(other.data(text("x")));
    EXPECT_EQ(second->unread().size(), 1U);
    EXPECT_EQ(tcp_->counters().dropped_no_node, 1U);
}
