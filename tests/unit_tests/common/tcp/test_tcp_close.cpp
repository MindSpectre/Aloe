#include <algorithm>
#include <aloe/frames>
#include <aloe/stream>
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
    using aloe::stream::Events;
    using aloe::tcp::State;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;
    using aloe::wire::TcpSequence;
    using TcpClose         = aloe::testing::TcpFixture;
    using TcpCloseRefusing = aloe::testing::TcpRefusingFixture;

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpClose,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpCloseRefusing,
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

}  // namespace

TEST_P(TcpClose, LocalFirst) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence committed = c->committed();
    c->close();
    EXPECT_EQ(c->state(), State::FinWait1);
    EXPECT_EQ(c->writable(), 0U) << "no more sends";
    EXPECT_EQ(wheel_.pending(), 1U);
    auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->flags, (TcpFlags{TcpFlag::Fin, TcpFlag::Ack}));
    EXPECT_EQ(frames[0].tcp->sequence, committed);
    EXPECT_EQ(c->committed(), committed + 1U) << "the FIN takes a sequence number";
    receive(peer_.ack(frames[0]));
    EXPECT_EQ(c->state(), State::FinWait2);
    EXPECT_EQ(wheel_.pending(), 0U);
    auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->events, Events{aloe::stream::Event::Acked});
    receive(peer_.fin());
    EXPECT_EQ(c->state(), State::Closed);
    EXPECT_TRUE(c->peer_closed());
    frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U) << "the terminal ACK was queued before Closed was published";
    EXPECT_EQ(frames[0].tcp->acknowledgement, peer_.snd_nxt());
    event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->events, (Events{aloe::stream::Event::PeerClosed, aloe::stream::Event::Closed}));
    EXPECT_EQ(tcp_->counters().connections_closed, 1U);
    EXPECT_EQ(tcp_->counters().control_segments_sent, 2U) << "the SYN-ACK and the FIN";
    c->release();
    EXPECT_EQ(tcp_->table_size(), 0U);
    EXPECT_TRUE(collect().empty()) << "a normal close sends no RST on release";
    EXPECT_EQ(tcp_->counters().resets_sent, 0U);
}

TEST_P(TcpClose, KeepsAnOpenPreparationForTheApplication) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    aloe::frames::TcpPeer other{aloe::testing::peer_spec(aloe::testing::tcp_peer_port + 1), TcpSequence{5000U}};
    auto* d = open_passive(other);
    ASSERT_NE(d, nullptr);
    const auto span    = c->prepare(10);
    const auto discard = d->prepare(10);
    ASSERT_TRUE(span.has_value());
    ASSERT_TRUE(discard.has_value());
    c->close();
    d->close();
    EXPECT_EQ(c->state(), State::FinWait1);
    EXPECT_EQ(d->state(), State::FinWait1);
    EXPECT_EQ(collect_queued().size(), 2U) << "both FINs left with the preparations still open";
    std::ranges::fill(*span, std::byte{0x5a});  // the span stays the application's until commit or release
    EXPECT_FALSE(c->commit(span->size())) << "nothing is sent after the FIN";
    EXPECT_EQ(tcp_->counters().commits_refused, 1U);
    EXPECT_TRUE(d->commit(0)) << "a discard is never refused";
    EXPECT_EQ(tcp_->counters().commits_refused, 1U);
    EXPECT_EQ(tcp_->counters().data_segments_sent, 0U);
    EXPECT_TRUE(collect().empty());
}

TEST_P(TcpClose, PeerFirstWithUnreadData) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(text("bye")));
    receive(peer_.fin());
    EXPECT_EQ(c->state(), State::CloseWait);
    EXPECT_TRUE(c->peer_closed());
    EXPECT_EQ(c->unread().size(), 3U) << "what came before the FIN is still readable";
    EXPECT_TRUE(collect_queued().empty()) << "the application's close may carry the ACK";
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->events, (Events{aloe::stream::Event::Readable, aloe::stream::Event::PeerClosed}));
    c->consume(3);
    c->close();
    EXPECT_EQ(c->state(), State::LastAck);
    auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->flags, (TcpFlags{TcpFlag::Fin, TcpFlag::Ack}));
    EXPECT_EQ(frames[0].tcp->acknowledgement, peer_.snd_nxt()) << "the FIN carries the peer's FIN ACK";
    EXPECT_TRUE(collect().empty()) << "and no pure ACK follows it";
    receive(peer_.ack(frames[0]));
    EXPECT_EQ(c->state(), State::Closed);
    EXPECT_EQ(poll()->events, (Events{aloe::stream::Event::Acked, aloe::stream::Event::Closed}));
    EXPECT_EQ(tcp_->counters().connections_closed, 1U);
    EXPECT_EQ(wheel_.pending(), 0U);
}

TEST_P(TcpClose, Simultaneous) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    c->close();
    const auto fin = collect_queued();
    ASSERT_EQ(fin.size(), 1U);
    receive(peer_.fin());  // the peer's FIN crossed ours: it acknowledges nothing new
    EXPECT_EQ(c->state(), State::Closing);
    auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U) << "a FIN in FinWait1 is acknowledged at once";
    EXPECT_EQ(frames[0].tcp->acknowledgement, peer_.snd_nxt());
    receive(peer_.ack(fin[0]));
    EXPECT_EQ(c->state(), State::Closed);
    EXPECT_TRUE(poll()->events.closed());
    EXPECT_EQ(tcp_->counters().connections_closed, 1U);
}

TEST_P(TcpClose, DataAndFinTogether) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack, TcpFlag::Fin}, text("end"), peer_.snd_nxt(), peer_.rcv_nxt()));
    EXPECT_EQ(c->unread().size(), 3U);
    EXPECT_TRUE(c->peer_closed());
    EXPECT_EQ(c->state(), State::CloseWait);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, peer_.snd_nxt() + 4U) << "three bytes and the FIN";
}

TEST_P(TcpClose, HandshakeAckWithFin) {
    ASSERT_TRUE(tcp_->listen(aloe::testing::tcp_listen_port).has_value());
    receive(peer_.syn());
    const auto syn_ack = collect();
    ASSERT_EQ(syn_ack.size(), 1U);
    peer_.see(syn_ack[0]);
    receive(peer_.fin());  // completes the handshake and closes the peer's half in one segment
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->events, (Events{aloe::stream::Event::Accepted, aloe::stream::Event::PeerClosed}));
    EXPECT_EQ(event->connection->state(), State::CloseWait);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 2});
}

TEST_P(TcpClose, DuplicateFin) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.fin());
    const auto first = collect();
    ASSERT_EQ(first.size(), 1U);
    peer_.rewind(1);
    receive(peer_.fin());  // retransmitted
    EXPECT_EQ(tcp_->counters().dropped_duplicate, 1U);
    const auto again = collect_queued();
    ASSERT_EQ(again.size(), 1U) << "acknowledged at once";
    EXPECT_EQ(again[0].tcp->acknowledgement, first[0].tcp->acknowledgement) << "rcv_nxt advanced once";
    EXPECT_EQ(c->state(), State::CloseWait);
    EXPECT_EQ(tcp_->pending_events(), 1U);
}

TEST_P(TcpClose, SecondFinAfterTheFirstIsDropped) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.fin());
    const auto first = collect();
    ASSERT_EQ(first.size(), 1U);
    ASSERT_EQ(c->state(), State::CloseWait);
    const auto unexpected = tcp_->counters().dropped_unexpected;
    receive(peer_.fin());  // a new FIN at rcv_nxt: sequence space the peer never offered
    EXPECT_EQ(tcp_->counters().dropped_unexpected, unexpected + 1U);
    const auto again = collect_queued();
    ASSERT_EQ(again.size(), 1U) << "acknowledged at once";
    EXPECT_EQ(again[0].tcp->flags, TcpFlags{TcpFlag::Ack}) << "a pure ACK";
    EXPECT_EQ(again[0].tcp->acknowledgement, first[0].tcp->acknowledgement) << "rcv_nxt did not move";
    EXPECT_EQ(c->state(), State::CloseWait);
}

TEST_P(TcpClose, AFinAboveAGapTakesEffectWhenTheGapFills) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence base = peer_.snd_nxt();
    receive(peer_.segment(
        {TcpFlag::Psh, TcpFlag::Ack, TcpFlag::Fin}, aloe::frames::pattern(100), base + 1000U, peer_.rcv_nxt()));
    EXPECT_EQ(c->out_of_order_segments(), 1U);
    EXPECT_FALSE(c->events().peer_closed()) << "the FIN is remembered, not taken";
    EXPECT_EQ(c->state(), State::Established);
    auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, base);
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(1000), base, peer_.rcv_nxt()));
    EXPECT_TRUE(c->events().peer_closed());
    EXPECT_TRUE(c->peer_closed());
    EXPECT_EQ(c->state(), State::CloseWait);
    EXPECT_EQ(c->unread().size(), 1100U);
    frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U) << "one ACK, at once, after the join";
    EXPECT_EQ(frames[0].tcp->acknowledgement, base + 1101U) << "1100 bytes and the FIN";
    EXPECT_TRUE(collect().empty());
}

TEST_P(TcpClose, ABareFinAboveAGapIsRemembered) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence base = peer_.snd_nxt();
    receive(peer_.segment({TcpFlag::Fin, TcpFlag::Ack}, {}, base + 1000U, peer_.rcv_nxt()));
    EXPECT_EQ(c->out_of_order_segments(), 0U) << "no packet is held for a bare FIN";
    EXPECT_FALSE(c->events().peer_closed());
    EXPECT_EQ(collect_queued().size(), 1U) << "answered at once";
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(1000), base, peer_.rcv_nxt()));
    EXPECT_TRUE(c->events().peer_closed());
    EXPECT_EQ(c->state(), State::CloseWait);
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, base + 1001U);
}

TEST_P(TcpClose, DataPastARememberedFinIsRefused) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence base = peer_.snd_nxt();
    receive(peer_.segment({TcpFlag::Fin, TcpFlag::Ack}, {}, base + 1000U, peer_.rcv_nxt()));
    std::ignore = collect_queued();
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(100), base + 1000U, peer_.rcv_nxt()));
    EXPECT_EQ(tcp_->counters().dropped_unexpected, 1U) << "data at the FIN";
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(200), base + 900U, peer_.rcv_nxt()));
    EXPECT_EQ(tcp_->counters().dropped_unexpected, 2U) << "data that runs past the FIN";
    EXPECT_EQ(collect_queued().size(), 2U) << "each answered at once";
    EXPECT_EQ(c->out_of_order_segments(), 0U);
    EXPECT_EQ(tcp_->counters().out_of_order_held, 0U);
}

TEST_P(TcpClose, AnInOrderFinReleasesWhatIsHeldAbove) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const std::size_t nodes = tcp_->nodes_available();
    receive(peer_.data(aloe::frames::pattern(10)));
    receive(peer_.segment(
        {TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(100), peer_.snd_nxt() + 2000U, peer_.rcv_nxt()));
    EXPECT_EQ(c->out_of_order_segments(), 1U);
    EXPECT_EQ(tcp_->nodes_available(), nodes - 2);
    receive(peer_.fin());
    EXPECT_EQ(c->state(), State::CloseWait);
    EXPECT_EQ(c->out_of_order_segments(), 0U);
    EXPECT_EQ(tcp_->nodes_available(), nodes - 1) << "only the in-order packet is still held";
    EXPECT_EQ(c->unread().size(), 10U);
}

TEST_P(TcpClose, AnInOrderFinWithDataDoesNotJoinWhatIsHeldAbove) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence base  = peer_.snd_nxt();
    const std::size_t nodes = tcp_->nodes_available();
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(100), base + 10U, peer_.rcv_nxt()));
    std::ignore = collect_queued();
    // The peer's stream ends at 10: what it sent above that is not data.
    receive(
        peer_.segment({TcpFlag::Psh, TcpFlag::Ack, TcpFlag::Fin}, aloe::frames::pattern(10), base, peer_.rcv_nxt()));
    EXPECT_EQ(c->state(), State::CloseWait);
    EXPECT_EQ(c->unread().size(), 10U);
    EXPECT_EQ(c->out_of_order_segments(), 0U);
    EXPECT_EQ(tcp_->nodes_available(), nodes - 1);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, base + 11U) << "ten bytes and the FIN";
}

TEST_P(TcpClose, AJoinThatReachesTheFinInFinWait2SendsOneAckAndCloses) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    c->close();
    const auto fin = collect_queued();
    ASSERT_EQ(fin.size(), 1U);
    receive(peer_.ack(fin[0]));
    ASSERT_EQ(c->state(), State::FinWait2);
    std::ignore            = poll();
    const TcpSequence base = peer_.snd_nxt();
    receive(peer_.segment({TcpFlag::Fin, TcpFlag::Ack}, {}, base + 100U, peer_.rcv_nxt()));
    std::ignore = collect_queued();
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(100), base, peer_.rcv_nxt()));
    EXPECT_EQ(c->state(), State::Closed) << "the terminal ACK left";
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "one ACK, none on the closed connection";
    EXPECT_EQ(frames[0].tcp->acknowledgement, base + 101U);
    EXPECT_EQ(tcp_->counters().connections_closed, 1U);
}

TEST_P(TcpClose, ABareFinBelowHeldDataEndsTheRunWhenTheGapFills) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence base = peer_.snd_nxt();
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(100), base + 1000U, peer_.rcv_nxt()));
    receive(peer_.segment({TcpFlag::Fin, TcpFlag::Ack}, {}, base + 1000U, peer_.rcv_nxt()));
    std::ignore = collect_queued();
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(1000), base, peer_.rcv_nxt()));
    EXPECT_TRUE(c->events().peer_closed());
    EXPECT_EQ(c->state(), State::CloseWait);
    EXPECT_EQ(c->unread().size(), 1000U) << "nothing past the peer's FIN is read";
    EXPECT_EQ(c->out_of_order_segments(), 0U);
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, base + 1001U);
}

TEST_P(TcpClose, AFinInsideHeldDataCutsTheRunWhenTheGapFills) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence base = peer_.snd_nxt();
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(100), base + 1100U, peer_.rcv_nxt()));
    receive(peer_.segment(
        {TcpFlag::Psh, TcpFlag::Ack, TcpFlag::Fin}, aloe::frames::pattern(150), base + 1000U, peer_.rcv_nxt()));
    std::ignore = collect_queued();
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(1000), base, peer_.rcv_nxt()));
    EXPECT_TRUE(c->events().peer_closed());
    EXPECT_EQ(c->state(), State::CloseWait);
    EXPECT_EQ(c->unread().size(), 1150U) << "the run ends at the FIN, inside the held node";
    EXPECT_EQ(c->out_of_order_segments(), 0U);
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, base + 1151U);
}

TEST_P(TcpClose, InOrderDataPastARememberedFinIsRefused) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence base = peer_.snd_nxt();
    receive(peer_.segment({TcpFlag::Fin, TcpFlag::Ack}, {}, base + 1000U, peer_.rcv_nxt()));
    std::ignore = collect_queued();
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(1100), base, peer_.rcv_nxt()));
    EXPECT_EQ(tcp_->counters().dropped_unexpected, 1U);
    EXPECT_TRUE(c->unread().empty());
    auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U) << "answered at once";
    EXPECT_EQ(frames[0].tcp->acknowledgement, base);
    EXPECT_EQ(frames[0].tcp->window, aloe::testing::tcp_fabric_budget) << "the edge restated";
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(1000), base, peer_.rcv_nxt()));
    EXPECT_EQ(c->unread().size(), 1000U);
    EXPECT_EQ(c->state(), State::CloseWait) << "the data ending at the FIN takes it";
    frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, base + 1001U);
}

TEST_P(TcpClose, ABareFinBelowARememberedOneReplacesIt) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence base = peer_.snd_nxt();
    receive(peer_.segment({TcpFlag::Fin, TcpFlag::Ack}, {}, base + 1000U, peer_.rcv_nxt()));
    receive(peer_.segment({TcpFlag::Fin, TcpFlag::Ack}, {}, base + 500U, peer_.rcv_nxt()));
    EXPECT_EQ(tcp_->counters().dropped_unexpected, 0U);
    receive(peer_.segment({TcpFlag::Fin, TcpFlag::Ack}, {}, base + 800U, peer_.rcv_nxt()));
    EXPECT_EQ(tcp_->counters().dropped_unexpected, 1U) << "a FIN above the remembered one is refused";
    std::ignore = collect_queued();
    receive(peer_.segment({TcpFlag::Psh, TcpFlag::Ack}, aloe::frames::pattern(500), base, peer_.rcv_nxt()));
    EXPECT_EQ(c->state(), State::CloseWait);
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, base + 501U);
}

TEST_P(TcpClose, FinRetransmitKeepsItsSequenceThenTimesOut) {
    configure_tcp({.data_retries = 2});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    c->close();
    const auto first = collect_queued();
    ASSERT_EQ(first.size(), 1U);
    // A handshake with a zero round trip leaves the RTO at rto_min, 200 ms, doubling from there: the FIN again at 200
    // and 600 ms, the time-out at 1.4 s. The wheel fires a timer once per advance, so time moves in steps of 100 ms.
    std::uint64_t retransmits = 0;
    for (std::int64_t step = 1; step < 14; ++step) {
        advance(100ms);
        const auto frames = collect();
        if (step == 2 || step == 6) {
            ASSERT_EQ(frames.size(), 1U) << "a retransmission at " << step * 100 << " ms";
            EXPECT_EQ(frames[0].tcp->flags, (TcpFlags{TcpFlag::Fin, TcpFlag::Ack}));
            EXPECT_EQ(frames[0].tcp->sequence, first[0].tcp->sequence) << "the FIN keeps its number";
            ++retransmits;
        } else {
            EXPECT_TRUE(frames.empty()) << "nothing at " << step * 100 << " ms";
        }
        EXPECT_EQ(tcp_->counters().retransmits, retransmits);
    }
    EXPECT_EQ(c->state(), State::FinWait1) << "still waiting for the FIN's ACK before 1.4 s";
    advance(100ms);
    EXPECT_EQ(c->state(), State::Closed);
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.timed_out());
    EXPECT_EQ(tcp_->counters().connections_timed_out, 1U);
    EXPECT_EQ(wheel_.pending(), 0U);
}

TEST_P(TcpClose, CloseInOtherStatesDoesNothing) {
    const auto connected =
        tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    std::ignore = collect();
    (*connected)->close();
    EXPECT_EQ((*connected)->state(), State::SynSent);
    EXPECT_TRUE(collect().empty());
    (*connected)->release();
    std::ignore = collect();
}

TEST_P(TcpClose, AbortSendsRstAndKeepsUnreadUntilRelease) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(text("kept")));
    const std::size_t nodes = tcp_->nodes_available();
    c->abort();
    EXPECT_EQ(c->state(), State::Closed);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "the RST, and no pending ACK after it";
    EXPECT_EQ(frames[0].tcp->flags, (TcpFlags{TcpFlag::Rst, TcpFlag::Ack}));
    EXPECT_EQ(frames[0].tcp->sequence, c->committed());
    EXPECT_EQ(poll()->events, (Events{aloe::stream::Event::Readable, aloe::stream::Event::Closed}));
    EXPECT_EQ(c->unread().size(), 4U);
    EXPECT_EQ(tcp_->nodes_available(), nodes);
    EXPECT_EQ(wheel_.pending(), 0U);
    EXPECT_EQ(tcp_->counters().resets_sent, 1U);
    c->release();
    EXPECT_EQ(tcp_->nodes_available(), nodes + 1);
    EXPECT_EQ(tcp_->counters().resets_sent, 1U) << "already closed: release sends nothing";
}

TEST_P(TcpClose, ReceivedResetThenLateSegmentsGetRstAndRstGetsSilence) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(text("kept")));
    receive(peer_.rst());
    EXPECT_EQ(c->state(), State::Closed);
    EXPECT_EQ(poll()->events, (Events{aloe::stream::Event::Readable, aloe::stream::Event::Reset}));
    EXPECT_EQ(c->unread().size(), 4U) << "held segments stay until release";
    EXPECT_EQ(tcp_->counters().connections_reset, 1U);
    EXPECT_TRUE(collect().empty());
    receive(peer_.data(text("late")));
    EXPECT_EQ(tcp_->counters().dropped_closed, 1U);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_TRUE(frames[0].tcp->flags.has(TcpFlag::Rst));
    receive(peer_.rst());
    EXPECT_EQ(tcp_->counters().dropped_closed, 2U);
    EXPECT_TRUE(collect().empty()) << "a RST to a closed entry is discarded, never answered";
    EXPECT_EQ(tcp_->pending_events(), 0U);
}

TEST_P(TcpClose, ReleaseOfAnOpenConnectionResetsAndFreesWithNoEvent) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(text("x")));
    c->release();
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_TRUE(frames[0].tcp->flags.has(TcpFlag::Rst));
    EXPECT_EQ(tcp_->table_size(), 0U);
    EXPECT_EQ(tcp_->pending_events(), 0U) << "the Closed raised by the abort went with the slot";
    EXPECT_EQ(wheel_.pending(), 0U);
    advance(70000ms);
    EXPECT_EQ(tcp_->pending_events(), 0U) << "a released connection raises nothing, ever";
    EXPECT_TRUE(collect().empty());
}

TEST_P(TcpClose, ReleaseTwiceIsRejected) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    c->release();
    ASSERT_EQ(tcp_->table_size(), 0U);
    std::ignore = collect();
    // A debug build asserts in a forked child; a release build ignores the second call.
    EXPECT_DEBUG_DEATH(c->release(), "release of a free slot");
    EXPECT_EQ(tcp_->table_size(), 0U);
    aloe::frames::TcpPeer first{aloe::testing::peer_spec(aloe::testing::tcp_peer_port + 1), TcpSequence{5000U}};
    aloe::frames::TcpPeer second{aloe::testing::peer_spec(aloe::testing::tcp_peer_port + 2), TcpSequence{6000U}};
    auto* a = open_passive(first);
    ASSERT_NE(a, nullptr);
    auto* b = open_passive(second);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a->index(), b->index()) << "the freed slot was pushed once, so two opens get two slots";
    EXPECT_EQ(tcp_->table_size(), 2U);
}

TEST_P(TcpCloseRefusing, FinalAckSurvivesRefusalAndClosedWaitsForIt) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    c->close();
    const auto fin = collect_queued();
    ASSERT_EQ(fin.size(), 1U);
    receive(peer_.ack(fin[0]));
    EXPECT_EQ(c->state(), State::FinWait2);
    std::ignore = poll();
    refuse();
    receive(peer_.fin());
    EXPECT_EQ(c->state(), State::FinWait2) << "the final transition waits until its ACK can be queued";
    EXPECT_EQ(poll()->events, Events{aloe::stream::Event::PeerClosed}) << "PeerClosed now; Closed only with the ACK";
    EXPECT_EQ(tcp_->counters().send_refused, 1U);
    allow();
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "flush retried the terminal ACK";
    EXPECT_EQ(frames[0].tcp->acknowledgement, peer_.snd_nxt());
    EXPECT_EQ(c->state(), State::Closed);
    EXPECT_EQ(poll()->events, Events{aloe::stream::Event::Closed});
    EXPECT_EQ(tcp_->counters().connections_closed, 1U);
}

TEST_P(TcpCloseRefusing, DuplicateFinAfterRefusedAckFinishesTheClose) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    c->close();
    const auto fin = collect_queued();
    ASSERT_EQ(fin.size(), 1U);
    receive(peer_.ack(fin[0]));
    EXPECT_EQ(c->state(), State::FinWait2);
    std::ignore = poll();
    refuse();
    receive(peer_.fin());
    EXPECT_EQ(c->state(), State::FinWait2) << "the terminal ACK was refused: Closed waits for it";
    EXPECT_EQ(poll()->events, Events{aloe::stream::Event::PeerClosed});
    allow();
    peer_.rewind(1);
    receive(peer_.fin());  // the peer retransmits its FIN before any flush
    EXPECT_EQ(tcp_->counters().dropped_duplicate, 1U);
    const auto frames = collect_queued();
    ASSERT_EQ(frames.size(), 1U) << "the duplicate's immediate ACK left without a flush";
    EXPECT_EQ(frames[0].tcp->acknowledgement, peer_.snd_nxt());
    EXPECT_EQ(c->state(), State::Closed) << "that ACK was the terminal one";
    const auto closed = poll();
    ASSERT_TRUE(closed.has_value()) << "Closed is raised by whichever ACK finally leaves";
    EXPECT_EQ(closed->events, Events{aloe::stream::Event::Closed});
    EXPECT_EQ(tcp_->counters().connections_closed, 1U);
}

TEST_P(TcpCloseRefusing, DataAfterThePeersFinIsDroppedWhileTheTerminalAckWaits) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    c->close();
    const auto fin = collect_queued();
    ASSERT_EQ(fin.size(), 1U);
    receive(peer_.ack(fin[0]));
    EXPECT_EQ(c->state(), State::FinWait2);
    std::ignore = poll();
    refuse();
    receive(peer_.fin());
    EXPECT_EQ(poll()->events, Events{aloe::stream::Event::PeerClosed});
    receive(peer_.data(text("late")));  // the peer's sequence space ended at its FIN
    EXPECT_EQ(c->unread().size(), 0U) << "nothing is readable after the peer's FIN";
    EXPECT_EQ(tcp_->counters().dropped_unexpected, 1U);
    EXPECT_FALSE(poll().has_value()) << "no Readable";
    EXPECT_EQ(c->state(), State::FinWait2) << "still waiting for the terminal ACK to leave";
}
