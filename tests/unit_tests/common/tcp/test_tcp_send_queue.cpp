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
    EXPECT_EQ(tcp_->send_nodes_available(), tcp_->config().send_pool);
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
    EXPECT_EQ(tcp_->send_nodes_available(), tcp_->config().send_pool);
    EXPECT_EQ(tcp_->counters().send_refused, 1U);
}
