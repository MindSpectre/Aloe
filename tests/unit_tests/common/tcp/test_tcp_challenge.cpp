#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>

// RFC 5961 sections 3 and 7, as Linux's `tcp_validate_incoming`, `tcp_reset_check`, `tcp_send_challenge_ack` and
// `tcp_oow_rate_limited` apply them: a RST resets only at RCV.NXT, at RCV.NXT - 1 once the peer's FIN was taken, or at
// the end of what is held above a gap on a connection that uses SACK; any other RST inside the window draws a challenge
// ACK, and one outside it is dropped in silence. Every challenge, and the ACK for a refused segment that occupies no
// sequence space, goes through one limiter per connection: one answer per `challenge_ack_interval`, 500 ms.
namespace {

    using namespace std::chrono_literals;
    using aloe::fabric::EmulatedOffloads;
    using aloe::frames::ParsedFrame;
    using aloe::frames::TcpPeer;
    using aloe::tcp::State;
    using aloe::tcp::TcpCounters;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;
    using aloe::wire::TcpSequence;
    using TcpChallenge         = aloe::testing::TcpFixture;
    using TcpChallengeRefusing = aloe::testing::TcpRefusingFixture;

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpChallenge,
                             ::testing::Values(EmulatedOffloads::None, EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpChallengeRefusing,
                             ::testing::Values(EmulatedOffloads::None, EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

    /// The peer's RST at `sequence`, with no ACK bit and the peer's state unchanged.
    [[nodiscard]] std::vector<std::byte> rst_at(const TcpPeer& peer, const TcpSequence sequence) {
        return peer.segment(TcpFlags{TcpFlag::Rst}, {}, sequence, peer.rcv_nxt());
    }

    /// `count` numbers before `sequence`, across the top of the sequence space too.
    [[nodiscard]] TcpSequence back(const TcpSequence sequence, const std::uint32_t count) {
        return TcpSequence{sequence.value - count};
    }

    [[nodiscard]] std::vector<std::byte> payload(const std::size_t count) {
        return std::vector<std::byte>(count, std::byte{0x61});
    }

    /// `frame` is a pure ACK at `sequence` acknowledging `acknowledgement`: what a challenge looks like, and the answer
    /// to a refused segment.
    void expect_pure_ack(const ParsedFrame& frame, const TcpSequence sequence, const TcpSequence acknowledgement) {
        ASSERT_TRUE(frame.tcp.has_value());
        EXPECT_EQ(frame.tcp->flags, TcpFlags{TcpFlag::Ack});
        EXPECT_EQ(frame.tcp->sequence, sequence);
        EXPECT_EQ(frame.tcp->acknowledgement, acknowledgement);
        EXPECT_EQ(frame.l4.size(), frame.tcp->data_offset) << "no payload";
    }

    using AfterFinCase = std::tuple<EmulatedOffloads, State>;

    /// The brick over a fabric port, parametrised over the three states that follow the peer's FIN as well as the
    /// offloads.
    class TcpChallengeAfterFin : public ::testing::TestWithParam<AfterFinCase>,
                                 protected aloe::testing::detail::FabricPair,
                                 protected aloe::testing::TcpHarness<aloe::fabric::Port> {
    protected:
        TcpChallengeAfterFin()
            : FabricPair{std::get<0>(GetParam())},
              TcpHarness{fabric, harness, port} {
        }

        /// Takes `c` to the parameter's state: CloseWait on the peer's FIN; LastAck on our close after it; Closing on
        /// the peer's FIN while ours is out and unacknowledged, since the peer never sees it.
        void reach(Connection& c) {
            const State state = std::get<1>(GetParam());
            if (state == State::Closing) {
                c.close();
                std::ignore = collect();
            }
            receive(peer_.fin());
            std::ignore = collect();
            if (state == State::LastAck) {
                c.close();
                std::ignore = collect();
            }
            ASSERT_EQ(c.state(), state);
        }
    };

    [[nodiscard]] std::string after_fin_name(const ::testing::TestParamInfo<AfterFinCase>& info) {
        const std::string offloads =
            std::get<0>(info.param) == EmulatedOffloads::Checksums ? "Checksums" : "NoOffloads";
        switch (std::get<1>(info.param)) {
            case State::CloseWait:
                return offloads + "_CloseWait";
            case State::Closing:
                return offloads + "_Closing";
            case State::LastAck:
                return offloads + "_LastAck";
            default:
                return offloads + "_Other";
        }
    }

    INSTANTIATE_TEST_SUITE_P(States,
                             TcpChallengeAfterFin,
                             ::testing::Combine(::testing::Values(EmulatedOffloads::None, EmulatedOffloads::Checksums),
                                                ::testing::Values(State::CloseWait, State::Closing, State::LastAck)),
                             after_fin_name);

}  // namespace

TEST_P(TcpChallenge, ARstAtRcvNxtResets) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(rst_at(peer_, peer_.snd_nxt()));
    EXPECT_EQ(c->state(), State::Closed);
    EXPECT_TRUE(c->events().reset());
    EXPECT_EQ(tcp_->counters().connections_reset, 1U);
    EXPECT_TRUE(collect().empty());
}

TEST_P(TcpChallenge, ARstInsideTheWindowIsChallenged) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence next        = peer_.snd_nxt();
    const std::uint32_t window    = c->receive_window();
    // One past RCV.NXT, then the window's last number, past the interval so the limiter lets the second go.
    const TcpSequence positions[] = {next + 1U, next + (window - 1U)};
    for (std::size_t round = 0; round < 2; ++round) {
        SCOPED_TRACE(::testing::Message() << "round " << round);
        if (round > 0) {
            advance(500ms);
        }
        const TcpCounters before = tcp_->counters();
        receive(rst_at(peer_, positions[round]));
        EXPECT_EQ(c->state(), State::Established);
        EXPECT_FALSE(c->events().reset());
        const auto frames = collect();
        ASSERT_EQ(frames.size(), 1U);
        expect_pure_ack(frames[0], c->committed(), next);
        EXPECT_EQ(frames[0].tcp->window, window) << "the edge restated";
        EXPECT_EQ(tcp_->counters().dropped_reset_challenged, before.dropped_reset_challenged + 1);
        EXPECT_EQ(tcp_->counters().challenge_acks_sent, before.challenge_acks_sent + 1);
    }
    EXPECT_EQ(tcp_->counters().connections_reset, 0U);
}

TEST_P(TcpChallenge, ARstOutsideTheWindowIsIgnored) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence next = peer_.snd_nxt();
    const TcpSequence edge = next + c->receive_window();
    TcpCounters expected   = tcp_->counters();
    receive(rst_at(peer_, back(next, 2)));
    receive(rst_at(peer_, edge + 1U));
    expected.segments_received     += 2;
    expected.dropped_out_of_window += 2;  // an empty segment outside the window, before RCV.NXT or past the edge
    EXPECT_EQ(tcp_->counters(), expected) << "no counter moves but the drop's";
    EXPECT_EQ(c->state(), State::Established);
    EXPECT_TRUE(collect().empty());
}

TEST_P(TcpChallengeAfterFin, ARstOneBackAfterThePeersFinResets) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_NO_FATAL_FAILURE(reach(*c));
    receive(rst_at(peer_, back(peer_.snd_nxt(), 1)));  // the FIN's own number
    EXPECT_EQ(c->state(), State::Closed);
    EXPECT_TRUE(c->events().reset());
    EXPECT_EQ(tcp_->counters().connections_reset, 1U);
    EXPECT_TRUE(collect().empty());
}

TEST_P(TcpChallenge, ARstOneBackBeforeThePeersFinIsIgnored) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    TcpCounters expected = tcp_->counters();
    receive(rst_at(peer_, back(peer_.snd_nxt(), 1)));
    expected.segments_received     += 1;
    expected.dropped_out_of_window += 1;
    EXPECT_EQ(tcp_->counters(), expected) << "no counter moves but the drop's";
    EXPECT_EQ(c->state(), State::Established);
    EXPECT_TRUE(collect().empty());
}

TEST_P(TcpChallenge, ARstAtTheEndOfWhatIsHeldResets) {
    peer_.set_sack_permitted(true);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->sack());
    const TcpSequence next = peer_.snd_nxt();
    receive(peer_.segment(TcpFlags{TcpFlag::Psh, TcpFlag::Ack}, payload(100), next + 1000U, peer_.rcv_nxt()));
    ASSERT_EQ(c->out_of_order_segments(), 1U);
    std::ignore = collect();
    receive(rst_at(peer_, next + 1100U));  // where a peer whose segments below were lost stands
    EXPECT_EQ(c->state(), State::Closed);
    EXPECT_TRUE(c->events().reset());
    EXPECT_EQ(tcp_->counters().connections_reset, 1U);
    EXPECT_TRUE(collect().empty());
}

TEST_P(TcpChallenge, ARstAtTheEndOfAHeldFinResets) {
    peer_.set_sack_permitted(true);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->sack());
    const TcpSequence next = peer_.snd_nxt();
    receive(
        peer_.segment(TcpFlags{TcpFlag::Fin, TcpFlag::Psh, TcpFlag::Ack}, payload(100), next + 1000U, peer_.rcv_nxt()));
    ASSERT_EQ(c->out_of_order_segments(), 1U);
    std::ignore = collect();
    receive(rst_at(peer_, next + 1100U));
    EXPECT_EQ(c->state(), State::Established) << "the FIN's number belongs to the held range";
    EXPECT_EQ(tcp_->counters().dropped_reset_challenged, 1U);
    std::ignore = collect();
    receive(rst_at(peer_, next + 1101U));
    EXPECT_EQ(c->state(), State::Closed);
    EXPECT_TRUE(c->events().reset());
    EXPECT_EQ(tcp_->counters().connections_reset, 1U);
}

TEST_P(TcpChallenge, ARstAtTheEndOfWhatIsHeldIsChallengedWithoutSack) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_FALSE(c->sack());
    const TcpSequence next = peer_.snd_nxt();
    receive(peer_.segment(TcpFlags{TcpFlag::Psh, TcpFlag::Ack}, payload(100), next + 1000U, peer_.rcv_nxt()));
    ASSERT_EQ(c->out_of_order_segments(), 1U);
    std::ignore = collect();
    receive(rst_at(peer_, next + 1100U));
    EXPECT_EQ(c->state(), State::Established);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    expect_pure_ack(frames[0], c->committed(), next);
    EXPECT_EQ(tcp_->counters().dropped_reset_challenged, 1U);
    EXPECT_EQ(tcp_->counters().challenge_acks_sent, 1U);
}

TEST_P(TcpChallenge, TheChallengedPeerThatAnswersWithAnExactRstResets) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence next = peer_.snd_nxt();
    receive(rst_at(peer_, next + 5U));
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    expect_pure_ack(frames[0], c->committed(), next);
    EXPECT_EQ(c->state(), State::Established);
    receive(rst_at(peer_, frames[0].tcp->acknowledgement));  // a peer that restarted answers from what we acknowledge
    EXPECT_EQ(c->state(), State::Closed);
    EXPECT_TRUE(c->events().reset());
    EXPECT_EQ(tcp_->counters().connections_reset, 1U);
}

TEST_P(TcpChallenge, ASecondChallengeInsideTheIntervalIsHeldBack) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence at = peer_.snd_nxt() + 1U;
    receive(rst_at(peer_, at));
    EXPECT_EQ(collect().size(), 1U);
    advance(100ms);
    receive(rst_at(peer_, at));
    EXPECT_TRUE(collect().empty()) << "100 ms after the first answer";
    advance(400ms);
    receive(rst_at(peer_, at));
    EXPECT_EQ(collect().size(), 1U) << "500 ms after the first answer";
    EXPECT_EQ(tcp_->counters().acks_rate_limited, 1U);
    EXPECT_EQ(tcp_->counters().challenge_acks_sent, 2U);
    EXPECT_EQ(tcp_->counters().dropped_reset_challenged, 3U);
    EXPECT_EQ(c->state(), State::Established);
}

TEST_P(TcpChallenge, AnEmptySegmentOutsideTheWindowSharesTheLimit) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence next = peer_.snd_nxt();
    receive(peer_.segment(TcpFlags{TcpFlag::Ack}, {}, back(next, 1), peer_.rcv_nxt()));
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "the first answer of the interval";
    expect_pure_ack(frames[0], c->committed(), next);
    advance(100ms);
    receive(rst_at(peer_, next + 1U));
    EXPECT_TRUE(collect().empty()) << "one stamp for every limited answer";
    EXPECT_EQ(tcp_->counters().acks_rate_limited, 1U);
    EXPECT_EQ(tcp_->counters().challenge_acks_sent, 0U);
    EXPECT_EQ(tcp_->counters().dropped_reset_challenged, 1U);
}

TEST_P(TcpChallenge, RefusedDataIsAnsweredEveryTime) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence start = peer_.snd_nxt();
    receive(peer_.data(payload(10)));
    std::ignore              = collect();
    const auto duplicate     = peer_.segment(TcpFlags{TcpFlag::Psh, TcpFlag::Ack}, payload(10), start, peer_.rcv_nxt());
    const TcpCounters before = tcp_->counters();
    for (int copy = 0; copy < 3; ++copy) {
        receive(duplicate);
    }
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 3U) << "data is not part of an ACK loop: no limit";
    for (const ParsedFrame& frame : frames) {
        expect_pure_ack(frame, c->committed(), peer_.snd_nxt());
    }
    EXPECT_EQ(tcp_->counters().dropped_duplicate, before.dropped_duplicate + 3);
    EXPECT_EQ(tcp_->counters().acks_rate_limited, 0U);
}

TEST_P(TcpChallenge, AFinSentAgainIsAnsweredEveryTime) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.fin());
    ASSERT_EQ(collect().size(), 1U) << "the FIN's ACK";
    ASSERT_EQ(c->state(), State::CloseWait);
    peer_.rewind(1);
    const auto again = peer_.fin();  // the same FIN, as when our ACK of it was lost
    receive(again);
    receive(again);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 2U) << "a FIN occupies sequence space: no limit";
    for (const ParsedFrame& frame : frames) {
        expect_pure_ack(frame, c->committed(), peer_.snd_nxt());
    }
    EXPECT_EQ(tcp_->counters().acks_rate_limited, 0U);
}

TEST_P(TcpChallenge, AZeroIntervalSendsEveryAnswer) {
    configure_tcp({.challenge_ack_interval = 0ms});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence at = peer_.snd_nxt() + 1U;
    for (int copy = 0; copy < 3; ++copy) {
        receive(rst_at(peer_, at));
    }
    EXPECT_EQ(collect().size(), 3U);
    EXPECT_EQ(tcp_->counters().challenge_acks_sent, 3U);
    EXPECT_EQ(tcp_->counters().acks_rate_limited, 0U);
}

TEST_P(TcpChallengeRefusing, AChallengeTheDeviceRefusesGoesAtTheFlushOnce) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence next = peer_.snd_nxt();
    refuse();
    receive(rst_at(peer_, next + 1U));
    EXPECT_EQ(tcp_->counters().send_refused, 1U);
    allow();
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "the refused challenge goes at the flush, as an ordinary ACK";
    expect_pure_ack(frames[0], c->committed(), next);
    advance(100ms);
    receive(rst_at(peer_, next + 1U));
    EXPECT_TRUE(collect().empty()) << "the refused challenge stamped the limiter all the same";
    EXPECT_EQ(tcp_->counters().acks_rate_limited, 1U);
}

TEST_P(TcpChallenge, TheNextConnectionOnTheSlotStartsClean) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence next = peer_.snd_nxt();
    receive(rst_at(peer_, next + 1U));
    ASSERT_EQ(collect().size(), 1U) << "challenged";
    receive(rst_at(peer_, next));
    ASSERT_EQ(c->state(), State::Closed);
    while (poll().has_value()) {}
    const std::uint32_t slot = c->index();
    c->release();
    TcpPeer second{aloe::testing::peer_spec(aloe::testing::tcp_peer_port + 1), TcpSequence{5000U}};
    auto* d = open_passive(second);  // in the same tick as the stamp the first connection left
    ASSERT_NE(d, nullptr);
    ASSERT_EQ(d->index(), slot) << "the freed slot is taken again";
    receive(rst_at(second, second.snd_nxt() + 1U));
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "challenged at once: the slot's limiter was cleared with it";
    expect_pure_ack(frames[0], d->committed(), second.snd_nxt());
    EXPECT_EQ(tcp_->counters().acks_rate_limited, 0U);
}

TEST_P(TcpChallenge, TheResetPositionsHoldAcrossTheTopOfTheSequenceSpace) {
    // 0x200 bytes from 0xffffff01: RCV.NXT wraps to 0x101.
    TcpPeer peer{aloe::testing::peer_spec(), TcpSequence{0xffffff00U}};
    auto* c = open_passive(peer);
    ASSERT_NE(c, nullptr);
    receive(peer.data(payload(0x200)));
    std::ignore            = collect();
    const TcpSequence next = peer.snd_nxt();
    ASSERT_EQ(next.value, 0x101U);
    receive(rst_at(peer, next + 1U));
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "challenged";
    expect_pure_ack(frames[0], c->committed(), next);
    receive(rst_at(peer, next));
    EXPECT_EQ(c->state(), State::Closed);
    EXPECT_TRUE(c->events().reset());
    while (poll().has_value()) {}

    TcpPeer other{aloe::testing::peer_spec(aloe::testing::tcp_peer_port + 1), TcpSequence{0xffffff00U}};
    auto* d = open_passive(other);
    ASSERT_NE(d, nullptr);
    receive(other.data(payload(0x200)));
    receive(other.fin());
    std::ignore = collect();
    ASSERT_EQ(d->state(), State::CloseWait);
    receive(rst_at(other, back(other.snd_nxt(), 1)));
    EXPECT_EQ(d->state(), State::Closed);
    EXPECT_TRUE(d->events().reset());
    EXPECT_EQ(tcp_->counters().connections_reset, 2U);
}
