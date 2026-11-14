#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <tuple>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>

// RFC 7323 timestamps: negotiated in the handshake, the option on every segment of a connection that has it,
// TS.Recent by the rule with Last.ACK.sent, PAWS, and the echo as a round-trip sample where Karn takes none.
namespace {

    using namespace std::chrono_literals;
    using TcpTimestamps = aloe::testing::TcpFixture;

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpTimestamps,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

    [[nodiscard]] std::optional<aloe::wire::TcpOptions> options_of(const aloe::frames::ParsedFrame& frame) {
        return aloe::wire::TcpOptions::parse(aloe::frames::tcp_options_of(frame));
    }

}  // namespace

TEST_P(TcpTimestamps, NegotiatedWhenBothOfferAndEverySegmentCarriesTheOption) {
    peer_.set_timestamps(100);
    ASSERT_TRUE(tcp_->listen(aloe::testing::tcp_listen_port).has_value());
    receive(peer_.syn());
    const auto syn_ack = collect();
    ASSERT_EQ(syn_ack.size(), 1U);
    EXPECT_EQ(syn_ack[0].tcp->data_offset, 36U) << "mss, nop, nop, TS";
    const auto options = options_of(syn_ack[0]);
    ASSERT_TRUE(options.has_value());
    EXPECT_EQ(options->mss, 1460U);
    EXPECT_EQ(options->timestamp_echo, 100U);
    peer_.see(syn_ack[0]);
    receive(peer_.ack());
    const auto accepted = poll();
    ASSERT_TRUE(accepted.has_value());
    auto* c = accepted->connection;
    EXPECT_TRUE(c->timestamps());
    EXPECT_EQ(c->mss(), 1448U);
    ASSERT_EQ(c->send(aloe::frames::pattern(1500)), 1500U);
    const auto data = collect();
    ASSERT_EQ(data.size(), 2U) << "1448 and 52";
    EXPECT_EQ(data[0].tcp->data_offset, 32U);
    EXPECT_EQ(options_of(data[0])->timestamp_echo, 100U);
    EXPECT_EQ(options_of(data[0])->timestamp_value, *options->timestamp_value) << "same tick, same value";
    EXPECT_EQ(aloe::frames::tcp_payload(data[0]).size(), 1448U);
}

TEST_P(TcpTimestamps, OffWhenThePeerOmitsItAndOffByConfig) {
    auto* c = open_passive(peer_);  // the default peer offers no timestamps
    ASSERT_NE(c, nullptr);
    EXPECT_FALSE(c->timestamps());
    EXPECT_EQ(c->mss(), 1460U);
    configure_tcp({.timestamps = false});
    peer_.set_timestamps(100);
    ASSERT_TRUE(tcp_->listen(aloe::testing::tcp_listen_port).has_value());
    receive(peer_.syn());
    const auto syn_ack = collect();
    ASSERT_EQ(syn_ack.size(), 1U);
    EXPECT_EQ(syn_ack[0].tcp->data_offset, 24U);
}

TEST_P(TcpTimestamps, AnActiveOpenOffersAndTakesThePeersAnswer) {
    const auto connected =
        tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    auto& c        = **connected;
    const auto syn = collect();
    ASSERT_EQ(syn.size(), 1U);
    EXPECT_EQ(syn[0].tcp->data_offset, 40U) << "mss, sackOK, TS, nop, wscale";
    EXPECT_EQ(options_of(syn[0])->timestamp_echo, 0U) << "a SYN echoes nothing";
    peer_.set_timestamps(5);
    receive(peer_.syn_ack(syn[0]));
    EXPECT_TRUE(c.timestamps());
    EXPECT_EQ(c.mss(), 1448U);
    std::ignore    = poll();
    const auto ack = collect();
    ASSERT_EQ(ack.size(), 1U);
    EXPECT_EQ(options_of(ack[0])->timestamp_echo, 5U);
}

TEST_P(TcpTimestamps, AnActiveOpenAnsweredWithoutTheOptionTurnsItOff) {
    const auto connected =
        tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    const auto syn = collect();
    ASSERT_EQ(syn.size(), 1U);
    receive(peer_.syn_ack(syn[0]));
    EXPECT_FALSE((*connected)->timestamps());
    EXPECT_EQ((*connected)->mss(), 1460U);
    std::ignore    = poll();
    const auto ack = collect();
    ASSERT_EQ(ack.size(), 1U);
    EXPECT_EQ(ack[0].tcp->data_offset, 20U);
}

TEST_P(TcpTimestamps, TheClockIsMilliseconds) {
    peer_.set_timestamps(100);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(1)), 1U);
    const auto first = collect();
    ASSERT_EQ(first.size(), 1U);
    advance(5ms);
    ASSERT_EQ(c->send(aloe::frames::pattern(1)), 1U);
    const auto second = collect();
    ASSERT_EQ(second.size(), 1U);
    EXPECT_EQ(*options_of(second[0])->timestamp_value - *options_of(first[0])->timestamp_value, 5U);
}

TEST_P(TcpTimestamps, TheEchoIsTheLatestAcceptableValueAtOrBeforeTheLastAckSent) {
    peer_.set_timestamps(100);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    peer_.set_timestamps(200);
    receive(peer_.data(aloe::frames::pattern(10)));
    peer_.set_timestamps(300);
    receive(peer_.data(aloe::frames::pattern(10)));  // past the last ACK sent: not taken yet
    const auto ack = collect();
    ASSERT_EQ(ack.size(), 1U);
    EXPECT_EQ(options_of(ack[0])->timestamp_echo, 200U);
    peer_.set_timestamps(400);
    receive(peer_.data(aloe::frames::pattern(10)));
    const auto next = collect();
    ASSERT_EQ(next.size(), 1U);
    EXPECT_EQ(options_of(next[0])->timestamp_echo, 400U) << "the ACK moved Last.ACK.sent past the second segment";
    c->consume(c->unread().size());
}

TEST_P(TcpTimestamps, PawsDropsAnOlderTimestampAndAcknowledges) {
    peer_.set_timestamps(500);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(aloe::frames::pattern(10)));
    std::ignore = collect();
    peer_.set_timestamps(400);
    const aloe::wire::TcpSequence second = peer_.snd_nxt();
    const auto old                       = peer_.data(aloe::frames::pattern(10));
    receive(old);
    EXPECT_EQ(tcp_->counters().dropped_paws, 1U);
    EXPECT_EQ(c->unread().size(), 10U) << "the second segment's bytes never arrived";
    EXPECT_EQ(collect().size(), 1U) << "acknowledged at once";
    peer_.set_timestamps(600);
    receive(peer_.segment(
        {aloe::wire::TcpFlag::Psh, aloe::wire::TcpFlag::Ack}, aloe::frames::pattern(10), second, peer_.rcv_nxt()));
    EXPECT_EQ(c->unread().size(), 20U) << "the same bytes with a fresh timestamp are taken";
    EXPECT_EQ(tcp_->counters().dropped_paws, 1U);
}

TEST_P(TcpTimestamps, ASegmentWithoutTheOptionIsAccepted) {
    peer_.set_timestamps(100);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    peer_.set_timestamps(std::nullopt);
    receive(peer_.data(aloe::frames::pattern(10)));
    EXPECT_EQ(c->unread().size(), 10U);
    EXPECT_EQ(tcp_->counters().dropped_paws, 0U);
}

TEST_P(TcpTimestamps, TheEchoGivesASampleWhereKarnWouldNot) {
    peer_.set_timestamps(100);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(10)), 10U);
    std::ignore = collect();
    advance(200ms);
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(c->rto(), 400ms);
    peer_.see(resent[0]);
    advance(3ms);
    receive(peer_.ack());  // echoes the resend's value: a sample of 3 ms, and the backoff ends
    EXPECT_EQ(tcp_->counters().rtt_samples, 2U);
    EXPECT_EQ(c->rto(), 200375us) << "rttvar 0.75 ms, srtt 0.375 ms: 0.375 + max(3, 200)";
}

TEST_P(TcpTimestamps, AFullSizedSegmentIsTheMssLessTheOption) {
    peer_.set_timestamps(100);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    constexpr std::size_t full = aloe::testing::tcp_fabric_mss - aloe::wire::TcpOptions::timestamps_size;  // 1448
    receive(peer_.data(aloe::frames::pattern(full)));
    EXPECT_TRUE(collect_queued().empty());
    receive(peer_.data(aloe::frames::pattern(full)));
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U) << "the second full segment of a timestamped peer is acknowledged at once";
    EXPECT_EQ(frames[0].tcp->acknowledgement, aloe::wire::TcpSequence{aloe::testing::tcp_peer_isn + 1 + (2 * full)});
    c->consume(c->unread().size());
}
