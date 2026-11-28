#include <algorithm>
#include <aloe/core>
#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>
#include <cstdint>
#include <tuple>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>

// The retransmission timer: RFC 6298's estimator with Linux's floor and ceiling, Karn's rule, the backoff, the retry
// caps and the user timeout; then its expiries into the peer's closed window, which count with the zero-window probes
// and not towards data_retries, so a connection whose peer answers every resend stays open (RFC 9293's MUST-37). A
// handshake with a zero round trip leaves the RTO at rto_min, 200 ms.
namespace {

    using namespace std::chrono_literals;
    using TcpRto     = aloe::testing::TcpFixture;
    using TcpRtoHeld = aloe::testing::TcpHeldFixture;

    /**
     * A connection whose peer closed its window on 100 bytes in flight: the ACK that closes it acknowledges nothing new
     * and states a zero window. The peer offers no SACK-permitted, so no tail probe comes before the RTO. Under a
     * ceiling of one second a run of many expiries stays short: they come 200 ms, 400 ms and 800 ms apart, then every
     * second.
     */
    class TcpRtoClosedWindow : public aloe::testing::TcpFixture {
    protected:
        static constexpr std::uint8_t data_retries    = aloe::tcp::TcpConfig{}.data_retries;
        static constexpr aloe::core::Duration ceiling = std::chrono::seconds{1};

        Connection* c_                 = nullptr;
        aloe::core::Duration interval_ = std::chrono::milliseconds{200};  ///< To the next expiry: the RTO now.

        /// The stack under `config`, the connection open, the 100 bytes sent and the peer's window closed on them.
        void open_closed(const aloe::tcp::TcpConfig& config) {
            configure_tcp(config);
            c_ = open_passive(peer_);
            ASSERT_NE(c_, nullptr);
            ASSERT_EQ(c_->send(aloe::frames::pattern(100)), 100U);
            ASSERT_EQ(collect().size(), 1U);
            peer_.set_window(0);
            receive(peer_.ack());
            ASSERT_EQ(c_->writable(), 0U);
            ASSERT_FALSE(c_->probing()) << "data in flight: the RTO's, not the probes'";
        }

        /// The next expiry, one interval on, at most one interval per step (the wheel fires a re-armed timer once per
        /// advance), and the resend it sends; the interval doubles up to the ceiling.
        void expire() {
            advance(interval_);
            interval_ = std::min<aloe::core::Duration>(interval_ * 2, ceiling);
            ASSERT_EQ(collect().size(), 1U) << "the head again";
        }

        /// The peer answers the last resend: an ACK at SND.UNA with its window still zero.
        void answer() {
            receive(peer_.ack());
        }
    };

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpRto,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpRtoHeld,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpRtoClosedWindow,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

}  // namespace

TEST_P(TcpRto, TheHandshakeGivesTheFirstSampleAndTheFloorHoldsAtZeroRtt) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->srtt(), 0ns) << "the ACK came in the same tick as the SYN-ACK";
    EXPECT_EQ(c->rto(), 200ms);
    EXPECT_EQ(c->retransmit_count(), 0U);
    EXPECT_EQ(tcp_->counters().rtt_samples, 1U);
}

TEST_P(TcpRto, TheEstimatorFollowsRfc6298WithLinuxsFloor) {
    const auto connected =
        tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    auto& c = **connected;
    EXPECT_EQ(c.rto(), 1s) << "rto_initial before any sample";
    const auto syn = collect();
    ASSERT_EQ(syn.size(), 1U);
    advance(10ms);
    receive(peer_.syn_ack(syn[0]));
    EXPECT_EQ(c.srtt(), 10ms);
    EXPECT_EQ(c.rto(), 210ms) << "10 + max(4 * 5, 200)";
    std::ignore = poll();
    std::ignore = collect();  // the handshake ACK
    ASSERT_EQ(c.send(aloe::frames::pattern(10)), 10U);
    const auto data = collect();
    ASSERT_EQ(data.size(), 1U);
    advance(30ms);
    receive(peer_.ack(data[0]));
    EXPECT_EQ(c.srtt(), 12500us) << "7/8 * 10 + 1/8 * 30";
    EXPECT_EQ(c.rto(), 212500us) << "12.5 + max(4 * 8.75, 200): rttvar updated first";
}

TEST_P(TcpRto, ALargeSampleGoesAboveTheFloorAndTheCeilingHolds) {
    configure_tcp({.rto_max = std::chrono::seconds{1}});
    const auto connected =
        tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    auto& c        = **connected;
    const auto syn = collect();
    ASSERT_EQ(syn.size(), 1U);
    advance(600ms);
    receive(peer_.syn_ack(syn[0]));
    EXPECT_EQ(c.rto(), 1s) << "600 + 1200 capped at rto_max";
}

TEST_P(TcpRto, ALostDataSegmentIsResentAtTheRtoThenDoubling) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(100)), 100U);
    const auto first = collect();
    ASSERT_EQ(first.size(), 1U);
    EXPECT_EQ(wheel_.pending(), 1U);
    advance(199ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);
    const auto second = collect();
    ASSERT_EQ(second.size(), 1U);
    EXPECT_EQ(second[0].tcp->sequence, first[0].tcp->sequence);
    EXPECT_EQ(c->rto(), 400ms);
    EXPECT_EQ(c->retransmit_count(), 1U);
    EXPECT_EQ(tcp_->counters().retransmits, 1U);
    advance(400ms);
    EXPECT_EQ(collect().size(), 1U);
    EXPECT_EQ(c->rto(), 800ms);
    EXPECT_EQ(c->retransmit_count(), 2U);
}

TEST_P(TcpRto, AnAckOfNewDataResetsTheRetryCountAndKarnKeepsTheBackoff) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(2000)), 2000U);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 2U);
    advance(200ms);
    EXPECT_EQ(collect().size(), 1U) << "the head resent";
    receive(peer_.ack(frames[0]));  // frees only the resent entry: no sample, and no echo to fall back on
    EXPECT_EQ(c->retransmit_count(), 0U);
    EXPECT_EQ(c->rto(), 400ms) << "backed off until a valid sample";
    EXPECT_EQ(tcp_->counters().rtt_samples, 1U) << "only the handshake's";
    advance(400ms);
    EXPECT_EQ(collect().size(), 1U) << "the second segment, at the backed-off RTO from the ACK";
    advance(50ms);
    receive(peer_.ack(frames[1]));  // frees only the second entry, resent too: still no sample
    EXPECT_EQ(tcp_->counters().rtt_samples, 1U);
    ASSERT_EQ(c->send(aloe::frames::pattern(10)), 10U);
    const auto third = collect();
    ASSERT_EQ(third.size(), 1U);
    advance(20ms);
    receive(peer_.ack(third[0]));
    EXPECT_EQ(tcp_->counters().rtt_samples, 2U) << "transmitted once: a sample";
    EXPECT_EQ(c->rto(), 202500us)
        << "srtt 7/8 * 0 + 1/8 * 20 = 2.5, rttvar 3/4 * 0 + 1/4 * 20 = 5: 2.5 + max(4 * 5, 200), backoff gone";
    EXPECT_EQ(tcp_->counters().undos_original_ack + tcp_->counters().undos_duplicate_sack, 0U)
        << "no echo, and the first resend left before either ACK: neither answers an original";
}

TEST_P(TcpRto, KarnDiscardsTheSampleWhenAnyFreedEntryWasRetransmitted) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(2000)), 2000U);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 2U);
    advance(200ms);
    EXPECT_EQ(collect().size(), 1U) << "the head resent";
    const auto samples = tcp_->counters().rtt_samples;
    peer_.see(frames[0]);
    receive(peer_.ack(frames[1]));  // one cumulative ACK of both: the second left once, the first twice
    EXPECT_EQ(c->queued_segments(), 0U);
    EXPECT_EQ(tcp_->counters().rtt_samples, samples) << "a resent entry among those freed: no sample, as Linux";
    EXPECT_EQ(c->rto(), 400ms) << "the backoff stays";
}

TEST_P(TcpRto, AnAckBetweenExpiriesReArmsToNowPlusRto) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(2000)), 2000U);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 2U);
    advance(100ms);
    receive(peer_.ack(frames[0]));  // the first, sent once: a sample of 100 ms
    EXPECT_EQ(c->rto(), 212500us) << "srtt 12.5, rttvar 25: 12.5 + max(4 * 25, 200)";
    advance(100ms);
    EXPECT_TRUE(collect().empty()) << "+200, the first arming's deadline: the ACK re-armed the timer";
    advance(112ms);
    EXPECT_TRUE(collect().empty()) << "+312";
    advance(1ms);
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U) << "+313, the first tick at or after 100 + 212.5";
    EXPECT_EQ(resent[0].tcp->sequence, frames[1].tcp->sequence);
    EXPECT_EQ(tcp_->counters().retransmits, 1U);
}

TEST_P(TcpRto, DataRetriesEndInTimedOut) {
    configure_tcp({.data_retries = 2});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(10)), 10U);
    std::ignore = collect();
    advance(200ms);
    advance(400ms);
    EXPECT_EQ(tcp_->counters().retransmits, 2U);
    advance(800ms);
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.timed_out());
    EXPECT_EQ(c->state(), aloe::tcp::State::Closed);
    EXPECT_EQ(c->queued_segments(), 0U);
    EXPECT_EQ(tcp_->counters().connections_timed_out, 1U);
    EXPECT_EQ(tcp_->counters().connections_user_timed_out, 0U);
    EXPECT_EQ(wheel_.pending(), 0U);
}

TEST_P(TcpRto, TheFinIsResentOnTheRtoAndItsAckCancelsTheTimer) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    c->close();
    const auto fin = collect();
    ASSERT_EQ(fin.size(), 1U);
    advance(200ms);
    EXPECT_EQ(collect().size(), 1U);
    advance(400ms);
    EXPECT_EQ(collect().size(), 1U);
    EXPECT_EQ(tcp_->counters().retransmits, 2U);
    receive(peer_.ack(fin[0]));
    EXPECT_EQ(c->state(), aloe::tcp::State::FinWait2);
    EXPECT_EQ(wheel_.pending(), 0U);
    advance(5s);
    EXPECT_TRUE(collect().empty());
}

TEST_P(TcpRto, TheSynKeepsItsScheduleAndItsOwnCap) {
    configure_tcp({.retries = 2});
    const auto connected =
        tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    auto& c     = **connected;
    std::ignore = collect();
    advance(1s);
    EXPECT_EQ(collect().size(), 1U);
    advance(2s);
    EXPECT_EQ(collect().size(), 1U);
    advance(4s);
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.timed_out());
    EXPECT_EQ(c.state(), aloe::tcp::State::Closed);
}

TEST_P(TcpRto, TheUserTimeoutEndsTheConnectionAtTheDeadlineWithTheLastRtoClamped) {
    configure_tcp({.user_timeout = std::chrono::milliseconds{500}});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(10)), 10U);
    std::ignore = collect();
    advance(200ms);
    EXPECT_EQ(collect().size(), 1U) << "one resend";
    advance(299ms);
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(c->state(), aloe::tcp::State::Established) << "the next RTO would be at +600: clamped to +500";
    advance(1ms);
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.timed_out());
    EXPECT_EQ(tcp_->counters().connections_user_timed_out, 1U);
    EXPECT_EQ(tcp_->counters().connections_timed_out, 1U);
}

TEST_P(TcpRto, AUserTimeoutShorterThanTheRtoEndsAtTheDeadline) {
    configure_tcp({.user_timeout = std::chrono::milliseconds{100}});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(10)), 10U);
    std::ignore = collect();
    advance(100ms);
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.timed_out());
    EXPECT_EQ(tcp_->counters().retransmits, 0U) << "no resend before the deadline";
    EXPECT_EQ(c->state(), aloe::tcp::State::Closed);
}

TEST_P(TcpRto, TheUserTimeoutDoesNotBoundTheHandshake) {
    configure_tcp({.user_timeout = std::chrono::milliseconds{100}});
    const auto connected =
        tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    std::ignore = collect();
    advance(1s);
    EXPECT_EQ(collect().size(), 1U) << "the SYN again, not a time-out";
    EXPECT_EQ((*connected)->state(), aloe::tcp::State::SynSent);
}

TEST_P(TcpRto, TheFirstExpiryTellsTheCongestionControl) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(100)), 100U);
    std::ignore = collect();
    EXPECT_GT(c->congestion_window(), c->mss()) << "the initial window";
    advance(200ms);
    EXPECT_EQ(c->recovery_state(), aloe::tcp::RecoveryState::Loss);
    EXPECT_EQ(c->congestion_window(), 1460U) << "NewReno's on_timeout: nothing in flight after the marking, plus one "
                                                "segment";
    EXPECT_EQ(c->congestion_state().ssthresh, 2920U) << "and on_congestion's target: two segments at the least";
}

TEST_P(TcpRtoHeld, AResendThatFindsNoNextHopWaitsForTheNextExpiryNotTheNextFlush) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(10)), 10U);
    ASSERT_EQ(collect().size(), 1U);
    advance(121s);  // the RTO finds the packet still lent and defers the resend; the ARP entry expires meanwhile
    EXPECT_EQ(tcp_->counters().retransmits, 1U);
    EXPECT_EQ(tcp_->counters().retransmits_deferred, 1U);
    return_loans();
    // The flush retries the deferred resend: the packet is back, but the rebuild finds no next hop.
    std::ignore           = collect();
    const auto unresolved = tcp_->counters().send_unresolved;
    EXPECT_GE(unresolved, 1U);
    std::ignore = collect();
    EXPECT_EQ(tcp_->counters().send_unresolved, unresolved) << "off the deferred list: the timer tries again";
    EXPECT_EQ(c->queued_segments(), 1U);
    EXPECT_EQ(wheel_.pending(), 1U);
}

TEST_P(TcpRtoClosedWindow, AnsweredResendsIntoAZeroWindowKeepTheConnection) {
    open_closed({.rto_max = ceiling});
    ASSERT_FALSE(HasFatalFailure());
    const int expiries = data_retries + 5;
    for (int expiry = 0; expiry < expiries; ++expiry) {
        expire();
        ASSERT_FALSE(HasFatalFailure()) << "expiry " << expiry + 1;
        answer();
    }
    EXPECT_EQ(c_->state(), aloe::tcp::State::Established) << "the peer answered every resend";
    EXPECT_EQ(c_->retransmit_count(), 0U) << "an expiry at a closed window is counted with the probes";
    EXPECT_EQ(tcp_->counters().retransmits, static_cast<std::uint64_t>(expiries));
    EXPECT_EQ(tcp_->counters().connections_timed_out, 0U);
}

TEST_P(TcpRtoClosedWindow, UnansweredResendsIntoAZeroWindowEndIt) {
    open_closed({.rto_max = ceiling});
    ASSERT_FALSE(HasFatalFailure());
    for (int expiry = 0; expiry < data_retries; ++expiry) {
        expire();
        ASSERT_FALSE(HasFatalFailure()) << "expiry " << expiry + 1;
    }
    EXPECT_EQ(c_->state(), aloe::tcp::State::Established);
    EXPECT_EQ(tcp_->counters().retransmits, data_retries);
    advance(interval_);
    EXPECT_TRUE(collect().empty()) << "the expiry after data_retries unanswered resends sends nothing";
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.timed_out());
    EXPECT_EQ(c_->state(), aloe::tcp::State::Closed);
    EXPECT_EQ(tcp_->counters().connections_timed_out, 1U);
    EXPECT_EQ(tcp_->counters().connections_user_timed_out, 0U);
}

TEST_P(TcpRtoClosedWindow, OneAnswerStartsTheCountAgain) {
    open_closed({.rto_max = ceiling});
    ASSERT_FALSE(HasFatalFailure());
    for (int expiry = 0; expiry < data_retries - 1; ++expiry) {
        expire();
        ASSERT_FALSE(HasFatalFailure()) << "expiry " << expiry + 1;
    }
    answer();
    for (int expiry = 0; expiry < data_retries - 1; ++expiry) {
        expire();
        ASSERT_FALSE(HasFatalFailure()) << "expiry " << expiry + 1 << " after the answer";
    }
    EXPECT_EQ(c_->state(), aloe::tcp::State::Established)
        << "2 * (data_retries - 1) unanswered resends, but no more than data_retries - 1 in a row";
    EXPECT_EQ(tcp_->counters().retransmits, 2U * (data_retries - 1U));
    EXPECT_EQ(tcp_->counters().connections_timed_out, 0U);
}

TEST_P(TcpRtoClosedWindow, TheUserTimeoutEndsItAllTheSame) {
    open_closed({.user_timeout = std::chrono::seconds{2}});
    ASSERT_FALSE(HasFatalFailure());
    for (int expiry = 0; expiry < 3; ++expiry) {  // at 200 ms, 600 ms and 1400 ms
        expire();
        ASSERT_FALSE(HasFatalFailure()) << "expiry " << expiry + 1;
        answer();
    }
    advance(599ms);
    EXPECT_EQ(c_->state(), aloe::tcp::State::Established) << "the next RTO, due at 3 s, is clamped to 2 s";
    advance(1ms);
    EXPECT_TRUE(collect().empty()) << "no resend at the deadline";
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.timed_out()) << "2 s after the head's first send, every resend answered";
    EXPECT_EQ(c_->state(), aloe::tcp::State::Closed);
    EXPECT_EQ(tcp_->counters().connections_user_timed_out, 1U);
    EXPECT_EQ(tcp_->counters().connections_timed_out, 1U);
}

TEST_P(TcpRtoClosedWindow, AfterTheWindowOpensTheCapStartsFromNothing) {
    open_closed({.rto_max = ceiling});
    ASSERT_FALSE(HasFatalFailure());
    for (int expiry = 0; expiry < 20; ++expiry) {
        expire();
        ASSERT_FALSE(HasFatalFailure()) << "expiry " << expiry + 1;
        answer();
    }
    peer_.set_window(65535);
    receive(peer_.ack());  // the window opens, nothing new acknowledged
    EXPECT_TRUE(collect().empty()) << "the head left at the last expiry";
    EXPECT_EQ(c_->retransmit_count(), 0U) << "the 20 expiries at the closed window left the cap's count alone";
    for (int expiry = 0; expiry < data_retries; ++expiry) {
        expire();
        ASSERT_FALSE(HasFatalFailure()) << "expiry " << expiry + 1 << " at the open window";
        EXPECT_EQ(c_->state(), aloe::tcp::State::Established);
    }
    EXPECT_EQ(c_->retransmit_count(), data_retries);
    advance(interval_);
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.timed_out()) << "data_retries unanswered expiries at the open window";
    EXPECT_EQ(c_->state(), aloe::tcp::State::Closed);
    EXPECT_EQ(tcp_->counters().retransmits, 20U + data_retries);
    EXPECT_EQ(tcp_->counters().connections_timed_out, 1U);
}

TEST_P(TcpRtoClosedWindow, TheBackoffMovesAtAClosedWindow) {
    open_closed({});
    ASSERT_FALSE(HasFatalFailure());
    for (const auto interval : {200ms, 400ms, 800ms}) {
        advance(interval - 1ms);
        EXPECT_TRUE(collect().empty()) << "before " << interval.count() << " ms";
        advance(1ms);
        ASSERT_EQ(collect().size(), 1U) << "at " << interval.count() << " ms";
        EXPECT_EQ(c_->rto(), interval * 2);
        answer();
    }
    EXPECT_EQ(tcp_->counters().retransmits, 3U);
    EXPECT_EQ(c_->retransmit_count(), 0U);
}
