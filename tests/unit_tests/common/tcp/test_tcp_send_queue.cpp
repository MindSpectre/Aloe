#include <algorithm>
#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>

namespace {

    using namespace std::chrono_literals;
    using TcpSendQueue         = aloe::testing::TcpFixture;
    using TcpSendQueueRefusing = aloe::testing::TcpRefusingFixture;

    /// A shard with three retransmission-queue nodes, one of them reserved for the connection's FIN at Established:
    /// two one-byte segments in flight empty the rest, which pins what each path that needs a node does without one.
    class TcpSendPoolExhausted : public aloe::testing::TcpFixture {
    protected:
        TcpSendPoolExhausted() {
            configure_tcp({.send_pool = 3});
        }
    };

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpSendQueue,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpSendQueueRefusing,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpSendPoolExhausted,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

    /// The refusing device over an ARP cache whose entries are gone after 100 ms: a FIN the device refused at close
    /// finds no next hop when the RTO sends it again, a first send from the RTO path, the ARP poll's case. Three
    /// polls bound the wait.
    class TcpRefusingNextHopGone : public aloe::testing::TcpRefusingFixture {
    protected:
        TcpRefusingNextHopGone()
            : TcpRefusingFixture{arp_expiring()} {
            configure_tcp({.unresolved_polls = 3});
        }

    private:
        [[nodiscard]] static aloe::net::Ipv4Config arp_expiring() {
            aloe::net::Ipv4Config config = aloe::testing::stack_config();
            config.arp_reachable         = 50ms;
            config.arp_expire            = 100ms;
            return config;
        }
    };

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpRefusingNextHopGone,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

}  // namespace

TEST_P(TcpSendQueue, EachSentSegmentIsAnEntryUntilAcknowledged) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->queued_segments(), 0U) << "the SYN-ACK was acknowledged by the handshake";
    EXPECT_EQ(c->send(aloe::frames::pattern(3000)), 3000U);
    EXPECT_EQ(c->queued_segments(), 3U);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 3U);
    EXPECT_EQ(tcp_->sent_segment(*c, 0).length, 1460U);
    EXPECT_EQ(tcp_->sent_segment(*c, 2).length, 80U);
    EXPECT_EQ(tcp_->sent_segment(*c, 0).transmits, 1U);
    EXPECT_EQ(tcp_->sent_segment(*c, 0).sent, now_);
    EXPECT_EQ(tcp_->sent_segment(*c, 0).first_sent, now_);
    EXPECT_EQ(tcp_->sent_segment(*c, 0).delivered_at_send, 0U);
    receive(peer_.ack(frames[0]));
    EXPECT_EQ(c->queued_segments(), 2U);
    EXPECT_EQ(c->delivered(), 1460U);
    EXPECT_TRUE(poll()->events.acked());
    receive(peer_.ack(frames[2]));
    EXPECT_EQ(c->queued_segments(), 0U);
    EXPECT_EQ(c->delivered(), 3000U);
    EXPECT_EQ(tcp_->send_nodes_available(), tcp_->config().send_pool - 1) << "less the FIN's reserved node";
}

TEST_P(TcpSendQueue, APartialAckKeepsTheEntryWhole) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(1000)), 1000U);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    const aloe::wire::TcpSequence first = frames[0].tcp->sequence;
    receive(peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), first + 500U));
    EXPECT_EQ(c->acknowledged(), first + 500U);
    EXPECT_EQ(c->queued_segments(), 1U);
    EXPECT_EQ(tcp_->sent_segment(*c, 0).first, first) << "untrimmed until a resend";
    EXPECT_EQ(tcp_->sent_segment(*c, 0).length, 1000U);
}

TEST_P(TcpSendQueue, TheSendCapRefusesPrepareAndTheFreeingAckRaisesWritable) {
    configure_tcp({.send_segments = 2});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(1)), 1U);
    ASSERT_EQ(c->send(aloe::frames::pattern(1)), 1U) << "one byte per segment: two entries";
    EXPECT_EQ(c->queued_segments(), 2U);
    EXPECT_EQ(c->writable(), 0U);
    EXPECT_FALSE(c->prepare(1).has_value());
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 2U);
    receive(peer_.ack(frames[0]));
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.writable());
    EXPECT_GT(c->writable(), 0U);
}

TEST_P(TcpSendQueue, PureAcksTakeNoEntryAndAreForgotten) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(10)));
    EXPECT_EQ(collect().size(), 1U) << "the ACK";
    EXPECT_EQ(c->queued_segments(), 0U);
    advance(1ms);
    EXPECT_EQ(port.loans_outstanding(0), 0U) << "the ACK's loan came back and nobody holds it";
}

TEST_P(TcpSendQueue, ReleaseWithEntriesLentLeaksNothing) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(2000)), 2000U);
    EXPECT_EQ(collect().size(), 2U);
    EXPECT_TRUE(tcp_->sent_segment(*c, 0).packet.lent());
    c->release();
    EXPECT_EQ(tcp_->send_nodes_available(), tcp_->config().send_pool);
    advance(1ms);
    process_pending();
    EXPECT_EQ(port.loans_outstanding(0), 0U);
    EXPECT_EQ(tcp_->table_size(), 0U);
}

TEST_P(TcpSendQueue, AHandshakeSegmentIsAnEntryResentFromItsPacket) {
    const auto connected =
        tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    auto& c          = **connected;
    const auto first = collect();
    ASSERT_EQ(first.size(), 1U);
    EXPECT_EQ(c.queued_segments(), 1U);
    advance(1s);
    const auto again = collect();
    ASSERT_EQ(again.size(), 1U);
    EXPECT_EQ(again[0].tcp->sequence, first[0].tcp->sequence);
    // The same segment, from the same packet: every byte as it was but the checksum and the timestamp value, which
    // the resend rewrites with the clock a second later.
    constexpr std::size_t checksum_at = 16;              // in the fixed header
    constexpr std::size_t ts_value_at = 20 + 4 + 2 + 2;  // past the header, the MSS, two nops, kind and length
    const auto value_of               = [](const aloe::frames::ParsedFrame& frame) {
        return aloe::wire::TcpOptions::parse(aloe::frames::tcp_options_of(frame))->timestamp_value.value_or(0);
    };
    EXPECT_EQ(value_of(again[0]) - value_of(first[0]), 1000U);
    auto expected = first[0].l4;
    std::ranges::copy(std::span{again[0].l4}.subspan(checksum_at, 2), expected.begin() + checksum_at);
    std::ranges::copy(std::span{again[0].l4}.subspan(ts_value_at, 4), expected.begin() + ts_value_at);
    EXPECT_EQ(again[0].l4, expected) << "the same segment, from the same packet";
    EXPECT_NE(again[0].ipv4->identification, first[0].ipv4->identification);
    EXPECT_EQ(tcp_->sent_segment(c, 0).transmits, 2U);
    EXPECT_EQ(tcp_->counters().retransmits, 1U);
    receive(peer_.syn_ack(again[0]));
    EXPECT_EQ(c.queued_segments(), 0U) << "the SYN-ACK acknowledged the SYN";
}

TEST_P(TcpSendQueue, TheFinIsTheLastEntryAndTheTimerResendsTheHead) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(100)), 100U);
    c->close();
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 2U);
    EXPECT_EQ(c->queued_segments(), 2U);
    EXPECT_TRUE(tcp_->sent_segment(*c, 1).flags.has(aloe::wire::TcpFlag::Fin));
    advance(1s);
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(aloe::frames::tcp_payload(resent[0]).size(), 100U) << "the head, the data, not the FIN";
    EXPECT_EQ(tcp_->counters().retransmits, 1U);
}

TEST_P(TcpSendQueue, AckAcrossTheSequenceWrapFreesTheRightEntries) {
    force_next_isn(aloe::wire::TcpSequence{0xfffffc00U});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(2000)), 2000U) << "the second segment wraps past 2^32";
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 2U);
    EXPECT_EQ(frames[1].tcp->sequence, aloe::wire::TcpSequence{0xfffffc01U + 1460U});
    receive(peer_.ack(frames[0]));
    EXPECT_EQ(c->queued_segments(), 1U);
    receive(peer_.ack(frames[1]));
    EXPECT_EQ(c->queued_segments(), 0U);
    EXPECT_EQ(c->acknowledged(), c->committed());
}

TEST_P(TcpSendQueueRefusing, ARefusedCommitTakesNoEntry) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    refuse();
    const auto room = c->prepare(10);
    ASSERT_TRUE(room.has_value());
    EXPECT_FALSE(c->commit(10));
    EXPECT_EQ(c->queued_segments(), 0U);
    EXPECT_EQ(tcp_->send_nodes_available(), tcp_->config().send_pool - 1) << "less the FIN's reserved node";
    EXPECT_EQ(tcp_->counters().send_refused, 1U);
}

TEST_P(TcpSendPoolExhausted, CommitWithNoNodeIsRefusedWithAHint) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(1)), 1U);
    ASSERT_EQ(c->send(aloe::frames::pattern(1)), 1U);
    ASSERT_EQ(collect().size(), 2U);
    ASSERT_EQ(tcp_->send_nodes_available(), 0U) << "two unacknowledged segments hold both nodes";
    while (poll()) {}
    const aloe::wire::TcpSequence committed = c->committed();
    const auto room                         = c->prepare(1);
    ASSERT_TRUE(room.has_value()) << "the windows allow more: prepare does not ask the queue";
    EXPECT_FALSE(c->commit(1));
    EXPECT_EQ(tcp_->counters().allocation_failures, 1U) << "no queue node for the segment";
    EXPECT_EQ(tcp_->counters().commits_refused, 0U);
    EXPECT_EQ(c->committed(), committed) << "the byte is not accepted";
    EXPECT_EQ(c->queued_segments(), 2U);
    EXPECT_EQ(tcp_->counters().data_segments_sent, 2U);
    EXPECT_TRUE(collect().empty());
    process_pending();
    const auto hint = poll();
    ASSERT_TRUE(hint.has_value());
    EXPECT_TRUE(hint->events.writable()) << "the retry hint, at the next process";
}

TEST_P(TcpSendPoolExhausted, ConnectWithNoNodeFails) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(1)), 1U);
    ASSERT_EQ(c->send(aloe::frames::pattern(1)), 1U);
    ASSERT_EQ(collect().size(), 2U);
    ASSERT_EQ(tcp_->send_nodes_available(), 0U);
    const auto connected =
        tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port + 1}, now_);
    ASSERT_FALSE(connected.has_value());
    EXPECT_EQ(connected.error(), aloe::tcp::ConnectError::TableFull) << "no node for the SYN";
    EXPECT_EQ(tcp_->table_size(), 1U) << "no slot taken";
    EXPECT_TRUE(collect().empty()) << "no SYN";
}

TEST_P(TcpSendPoolExhausted, PassiveOpenWithNoNodeCountsDroppedNoNode) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(1)), 1U);
    ASSERT_EQ(c->send(aloe::frames::pattern(1)), 1U);
    const auto sent = collect();
    ASSERT_EQ(sent.size(), 2U);
    ASSERT_EQ(tcp_->send_nodes_available(), 0U);
    while (poll()) {}
    aloe::frames::TcpPeer second{aloe::testing::peer_spec(aloe::testing::tcp_peer_port + 1),
                                 aloe::wire::TcpSequence{5000}};
    receive(second.syn());
    EXPECT_EQ(tcp_->counters().dropped_no_node, 1U) << "no node for the SYN-ACK";
    EXPECT_EQ(tcp_->counters().resets_sent, 0U);
    EXPECT_EQ(tcp_->table_size(), 1U) << "no connection";
    EXPECT_TRUE(collect().empty()) << "no SYN-ACK and no RST: the peer's SYN timer tries again";
    EXPECT_FALSE(poll().has_value());
    receive(peer_.ack(sent[1]));  // frees both nodes
    receive(second.segment({aloe::wire::TcpFlag::Syn}, {}, second.iss(), {}, true));
    const auto answer = collect();
    ASSERT_EQ(answer.size(), 1U);
    EXPECT_TRUE(aloe::testing::is_flags(answer[0], {aloe::wire::TcpFlag::Syn, aloe::wire::TcpFlag::Ack}))
        << "the SYN again, once a node is free";
    EXPECT_EQ(tcp_->table_size(), 2U);
}

TEST_P(TcpSendPoolExhausted, CloseWithThePoolEmptySendsTheFinFromItsReservation) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    std::size_t segments = 0;
    while (c->send(aloe::frames::pattern(1)) == 1U) {
        ++segments;
    }
    EXPECT_EQ(segments, 2U) << "two nodes for data: the third was set aside for the FIN at Established";
    ASSERT_EQ(tcp_->send_nodes_available(), 0U);
    ASSERT_EQ(collect().size(), segments);
    const aloe::wire::TcpSequence fin_sequence = c->committed();
    c->close();
    EXPECT_EQ(c->state(), aloe::tcp::State::FinWait1);
    const auto fin = collect();
    ASSERT_EQ(fin.size(), 1U) << "the FIN leaves at once, on the node reserved for it";
    EXPECT_TRUE(aloe::testing::is_flags(fin[0], {aloe::wire::TcpFlag::Fin, aloe::wire::TcpFlag::Ack}));
    EXPECT_EQ(fin[0].tcp->sequence, fin_sequence);
    EXPECT_EQ(c->queued_segments(), segments + 1);
    EXPECT_EQ(tcp_->counters().send_refused, 0U);
    EXPECT_EQ(tcp_->counters().retransmits, 0U) << "a first send, not an expiry's";
    receive(peer_.ack(fin[0]));  // everything acknowledged
    EXPECT_EQ(c->state(), aloe::tcp::State::FinWait2);
    EXPECT_EQ(c->queued_segments(), 0U);
    EXPECT_EQ(tcp_->send_nodes_available(), 3U) << "the reservation was spent on the FIN: every node is free";
}

TEST_P(TcpRefusingNextHopGone, AFinTheDeviceRefusedFindsNoNextHopAtTheRtoAndPollsToUnresolved) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    refuse();
    c->close();  // the FIN's first send, refused by the device: the RTO retries it
    EXPECT_EQ(c->queued_segments(), 1U);
    allow();
    std::ignore         = collect();
    const auto requests = ip_.counters().arp_requests_sent;
    advance(200ms);  // the entry expired at 100 ms: the retry, a first send, finds no next hop
    EXPECT_EQ(ip_.counters().arp_requests_sent, requests + 1) << "the rebuild asked";
    const auto at_rto = collect();
    ASSERT_EQ(at_rto.size(), 1U) << "the request alone: no FIN without a next hop";
    EXPECT_FALSE(at_rto[0].tcp.has_value());
    EXPECT_EQ(c->state(), aloe::tcp::State::FinWait1);
    advance(10ms);
    advance(10ms);
    EXPECT_EQ(c->state(), aloe::tcp::State::FinWait1) << "two polls, below the bound";
    advance(10ms);
    EXPECT_EQ(c->state(), aloe::tcp::State::Closed) << "the third poll is the bound: Unresolved, not the data retries";
    EXPECT_EQ(tcp_->counters().connections_unresolved, 1U);
    EXPECT_EQ(tcp_->counters().connections_timed_out, 0U);
}

TEST_P(TcpRefusingNextHopGone, AFinTheDeviceRefusedLeavesAtTheNextPollOnceTheNextHopAnswers) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    refuse();
    c->close();
    allow();
    std::ignore = collect();
    advance(200ms);  // the expiry: the retry finds no next hop and asks
    ASSERT_EQ(collect().size(), 1U) << "the request";
    ip_.learn(aloe::testing::harness_ip, aloe::testing::harness_mac, now_);  // the reply
    advance(10ms);
    const auto fin = collect();
    ASSERT_EQ(fin.size(), 1U) << "the poll sent the FIN, not the next RTO";
    EXPECT_TRUE(aloe::testing::is_flags(fin[0], {aloe::wire::TcpFlag::Fin, aloe::wire::TcpFlag::Ack}));
    EXPECT_EQ(c->rto(), 200ms) << "a send that never left lost nothing: no backoff";
    receive(peer_.ack(fin[0]));
    EXPECT_EQ(c->state(), aloe::tcp::State::FinWait2);
}

TEST_P(TcpSendQueue, EstablishedSetsANodeAsideForTheFin) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const std::size_t pool = tcp_->config().send_pool;
    EXPECT_EQ(tcp_->send_nodes_available(), pool - 1) << "one node reserved at Established: a commit cannot take it";
    c->close();
    const auto fin = collect();
    ASSERT_EQ(fin.size(), 1U);
    EXPECT_EQ(tcp_->send_nodes_available(), pool - 1) << "the FIN holds the node it was given";
    receive(peer_.ack(fin[0]));
    EXPECT_EQ(c->state(), aloe::tcp::State::FinWait2);
    EXPECT_EQ(tcp_->send_nodes_available(), pool) << "acknowledged: the node is back and the reservation spent";
}

TEST_P(TcpSendQueue, AnAbortReturnsTheNodeSetAsideForTheFin) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const std::size_t pool = tcp_->config().send_pool;
    ASSERT_EQ(tcp_->send_nodes_available(), pool - 1);
    c->abort();
    EXPECT_EQ(tcp_->send_nodes_available(), pool) << "no FIN will come: the reservation is returned";
}
