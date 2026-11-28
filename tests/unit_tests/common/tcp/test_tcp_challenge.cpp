#include <algorithm>
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

// RFC 5961 sections 3, 4 and 7, as Linux's `tcp_validate_incoming`, `tcp_reset_check`, `tcp_send_challenge_ack` and
// `tcp_oow_rate_limited` apply them: a RST resets only at RCV.NXT, at RCV.NXT - 1 once the peer's FIN was taken, or at
// the end of what is held above a gap on a connection that uses SACK; any other RST inside the window draws a challenge
// ACK, and one outside it is dropped in silence. A SYN on an established connection draws a challenge ACK wherever it
// lies and whether or not it passed PAWS. Every challenge, and the ACK for a refused segment that occupies no sequence
// space, goes through one limiter per connection: one answer per `challenge_ack_interval`, 500 ms. RFC 5961 section 5,
// as Linux's `tcp_ack`: an ACK of what was never sent, or one below SND.UNA less the smaller of the largest window the
// peer offered and what it has acknowledged since the connection became established, drops the whole segment and
// draws a challenge ACK.
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
    using TcpChallengeRefusing = aloe::testing::TcpRefusingFixture;

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

    /// The peer's data at `sequence` acknowledging `acknowledgement`, with the peer's state unchanged.
    [[nodiscard]] std::vector<std::byte>
    data_at(const TcpPeer& peer, const TcpSequence sequence, const TcpSequence acknowledgement) {
        return peer.segment(TcpFlags{TcpFlag::Psh, TcpFlag::Ack}, payload(10), sequence, acknowledgement);
    }

    /// The brick over a fabric port, with a peer that acknowledges what the stack sends.
    class TcpChallenge : public aloe::testing::TcpFixture {
    protected:
        /// Has `c` send `count` bytes in as many rounds as the windows need, `peer` seeing each round's segments and
        /// acknowledging them whole: SND.UNA moves by `count`, and the sum of its advances with it.
        void send_acknowledged(Connection& c, TcpPeer& peer, const std::size_t count) {
            for (std::size_t sent = 0; sent < count;) {
                const std::size_t accepted = c.send(payload(std::min(count - sent, c.writable())));
                ASSERT_GT(accepted, 0U) << "the windows let nothing go";
                for (const ParsedFrame& frame : collect()) {
                    peer.see(frame);
                }
                receive(peer.ack());
                sent += accepted;
            }
            ASSERT_EQ(c.unacknowledged(), 0U);
        }
    };

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpChallenge,
                             ::testing::Values(EmulatedOffloads::None, EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpChallengeRefusing,
                             ::testing::Values(EmulatedOffloads::None, EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

    /// `frame` is a pure ACK at `sequence` acknowledging `acknowledgement`: what a challenge looks like, and the answer
    /// to a refused segment.
    void expect_pure_ack(const ParsedFrame& frame, const TcpSequence sequence, const TcpSequence acknowledgement) {
        ASSERT_TRUE(frame.tcp.has_value());
        EXPECT_EQ(frame.tcp->flags, TcpFlags{TcpFlag::Ack});
        EXPECT_EQ(frame.tcp->sequence, sequence);
        EXPECT_EQ(frame.tcp->acknowledgement, acknowledgement);
        EXPECT_EQ(frame.l4.size(), frame.tcp->data_offset) << "no payload";
    }

    /// The TSecr a frame carries; zero without the option.
    [[nodiscard]] std::uint32_t ts_echo_of(const ParsedFrame& frame) {
        const auto options = aloe::wire::TcpOptions::parse(aloe::frames::tcp_options_of(frame));
        return options && options->timestamp_echo ? *options->timestamp_echo : 0;
    }

    /// The peer's SYN at `sequence`, with no ACK, as a peer that restarted sends it; the peer's state unchanged.
    [[nodiscard]] std::vector<std::byte> syn_at(const TcpPeer& peer, const TcpSequence sequence) {
        return peer.segment(TcpFlags{TcpFlag::Syn}, {}, sequence, TcpSequence{}, true);
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

TEST_P(TcpChallenge, ARstWithDataAtRcvNxtAtAZeroWindowResets) {
    configure_tcp({.receive_segments = 4});  // two full segments of budget
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    receive(peer_.data(payload(aloe::testing::tcp_fabric_mss)));
    receive(peer_.data(payload(aloe::testing::tcp_fabric_mss)));
    std::ignore = collect();
    ASSERT_EQ(c->receive_window(), 0U);
    // No room for its payload, but it starts at RCV.NXT: it goes on to the RST step, which resets there.
    receive(peer_.segment(TcpFlags{TcpFlag::Rst, TcpFlag::Ack}, payload(10), peer_.snd_nxt(), peer_.rcv_nxt()));
    EXPECT_EQ(c->state(), State::Closed);
    EXPECT_TRUE(c->events().reset());
    EXPECT_EQ(tcp_->counters().connections_reset, 1U);
    EXPECT_EQ(tcp_->counters().dropped_out_of_window, 0U);
    EXPECT_TRUE(collect().empty());
}

TEST_P(TcpChallenge, ARstAtTheEdgeThatIsTheEndOfWhatIsHeldResets) {
    configure_tcp({.receive_segments = 4});  // two full segments of budget
    peer_.set_sack_permitted(true);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->sack());
    constexpr std::uint16_t full = aloe::testing::tcp_fabric_mss;
    const TcpSequence next       = peer_.snd_nxt();
    const TcpSequence edge       = next + c->receive_window();
    ASSERT_EQ(edge, next + (2U * full));
    // A hole of one full segment at RCV.NXT, and the segment held above it fills the window to the edge.
    receive(peer_.segment(TcpFlags{TcpFlag::Psh, TcpFlag::Ack}, payload(full), next + full, peer_.rcv_nxt()));
    ASSERT_EQ(c->out_of_order_segments(), 1U);
    std::ignore = collect();
    receive(rst_at(peer_, edge));  // empty, exactly at the edge: acceptable, and where what is held ends
    EXPECT_EQ(c->state(), State::Closed);
    EXPECT_TRUE(c->events().reset());
    EXPECT_EQ(tcp_->counters().connections_reset, 1U);
    EXPECT_TRUE(collect().empty());
}

TEST_P(TcpChallenge, ARstOnePastAHeldFinAtTheEdgeIsDroppedInSilence) {
    configure_tcp({.receive_segments = 4});  // two full segments of budget
    peer_.set_sack_permitted(true);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->sack());
    constexpr std::uint16_t full = aloe::testing::tcp_fabric_mss;
    const TcpSequence next       = peer_.snd_nxt();
    const TcpSequence edge       = next + c->receive_window();
    ASSERT_EQ(edge, next + (2U * full));
    // Above a hole of one full segment, payload that ends at the edge with a FIN one past it: held, the FIN remembered,
    // and the end of what is held, the FIN's number included, one past the edge.
    receive(
        peer_.segment(TcpFlags{TcpFlag::Fin, TcpFlag::Psh, TcpFlag::Ack}, payload(full), next + full, peer_.rcv_nxt()));
    ASSERT_EQ(c->out_of_order_segments(), 1U);
    std::ignore          = collect();
    TcpCounters expected = tcp_->counters();
    // An empty segment past the edge: the sequence check refuses it before the RST rule, as Linux's tcp_sequence does.
    receive(rst_at(peer_, edge + 1U));
    expected.segments_received     += 1;
    expected.dropped_out_of_window += 1;
    EXPECT_EQ(tcp_->counters(), expected) << "no counter moves but the drop's";
    EXPECT_EQ(c->state(), State::Established);
    EXPECT_FALSE(c->events().reset());
    EXPECT_TRUE(collect().empty()) << "nothing sent: a RST outside the window goes in silence";
}

TEST_P(TcpChallenge, ARstAtTheEdgeWithNothingHeldIsChallenged) {
    peer_.set_sack_permitted(true);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->sack());
    const TcpSequence next = peer_.snd_nxt();
    receive(rst_at(peer_, next + c->receive_window()));  // empty, exactly at the edge: acceptable, no exact position
    EXPECT_EQ(c->state(), State::Established);
    EXPECT_FALSE(c->events().reset());
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "challenged";
    expect_pure_ack(frames[0], c->committed(), next);
    EXPECT_EQ(tcp_->counters().dropped_reset_challenged, 1U);
    EXPECT_EQ(tcp_->counters().challenge_acks_sent, 1U);
    EXPECT_EQ(tcp_->counters().dropped_out_of_window, 0U);
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
    ASSERT_NO_FATAL_FAILURE(send_acknowledged(*c, peer_, 3000));  // the first connection's sum stands at 3000
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
    advance(500ms);
    receive(data_at(second, second.snd_nxt(), back(d->acknowledged(), 1)));
    EXPECT_EQ(tcp_->counters().dropped_old_ack, 1U) << "too old: the slot's sum of acknowledged bytes went with it";
    EXPECT_EQ(d->unread().size(), 0U);
    EXPECT_EQ(collect().size(), 1U) << "challenged";
}

TEST_P(TcpChallenge, TheResetPositionsHoldAcrossTheTopOfTheSequenceSpace) {
    // 0xfe bytes from 0xffffff01: RCV.NXT stands at 0xffffffff, and one past it is 0.
    TcpPeer peer{aloe::testing::peer_spec(), TcpSequence{0xffffff00U}};
    auto* c = open_passive(peer);
    ASSERT_NE(c, nullptr);
    receive(peer.data(payload(0xfe)));
    std::ignore            = collect();
    const TcpSequence next = peer.snd_nxt();
    ASSERT_EQ(next.value, 0xffffffffU);
    receive(rst_at(peer, next + 1U));
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "challenged at 0";
    expect_pure_ack(frames[0], c->committed(), next);
    receive(rst_at(peer, next));
    EXPECT_EQ(c->state(), State::Closed);
    EXPECT_TRUE(c->events().reset());
    while (poll().has_value()) {}

    // 0xfe bytes and a FIN: RCV.NXT wraps to 0, and the RST one back after the FIN stands at 0xffffffff.
    TcpPeer other{aloe::testing::peer_spec(aloe::testing::tcp_peer_port + 1), TcpSequence{0xffffff00U}};
    auto* d = open_passive(other);
    ASSERT_NE(d, nullptr);
    receive(other.data(payload(0xfe)));
    receive(other.fin());
    std::ignore = collect();
    ASSERT_EQ(d->state(), State::CloseWait);
    ASSERT_EQ(other.snd_nxt().value, 0U);
    receive(rst_at(other, back(other.snd_nxt(), 1)));
    EXPECT_EQ(d->state(), State::Closed);
    EXPECT_TRUE(d->events().reset());
    while (poll().has_value()) {}

    // Two ranges held above a gap on a connection that uses SACK, RCV.NXT at 0xffffff01: [0xffffff41, 0xffffff61) and
    // [0xffffffc1, 0x41). The higher ends across zero, at a number below the lower's end: its end resets, the lower's
    // is challenged.
    TcpPeer sack{aloe::testing::peer_spec(aloe::testing::tcp_peer_port + 2), TcpSequence{0xffffff00U}};
    sack.set_sack_permitted(true);
    auto* e = open_passive(sack);
    ASSERT_NE(e, nullptr);
    ASSERT_TRUE(e->sack());
    receive(
        sack.segment(TcpFlags{TcpFlag::Psh, TcpFlag::Ack}, payload(0x20), TcpSequence{0xffffff41U}, sack.rcv_nxt()));
    receive(
        sack.segment(TcpFlags{TcpFlag::Psh, TcpFlag::Ack}, payload(0x80), TcpSequence{0xffffffc1U}, sack.rcv_nxt()));
    ASSERT_EQ(e->out_of_order_segments(), 2U);
    std::ignore = collect();
    receive(rst_at(sack, TcpSequence{0xffffff61U}));
    EXPECT_EQ(e->state(), State::Established) << "the lower range's end";
    const auto challenge = collect();
    ASSERT_EQ(challenge.size(), 1U) << "challenged";
    expect_pure_ack(challenge[0], e->committed(), sack.snd_nxt());
    receive(rst_at(sack, TcpSequence{0x41U}));
    EXPECT_EQ(e->state(), State::Closed);
    EXPECT_TRUE(e->events().reset());
    EXPECT_EQ(tcp_->counters().connections_reset, 3U);
}

TEST_P(TcpChallenge, APawsAnswerGoesThroughTheLimitUnlessTheSegmentCarriesData) {
    peer_.set_timestamps(100);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence next = peer_.snd_nxt();
    peer_.set_timestamps(50);  // older than TS.Recent: every segment below fails PAWS
    const auto old_ack = peer_.segment(TcpFlags{TcpFlag::Ack}, {}, next, c->committed());
    receive(old_ack);
    EXPECT_EQ(collect().size(), 1U) << "the first answer of the interval";
    advance(100ms);
    receive(old_ack);
    EXPECT_TRUE(collect().empty()) << "an empty segment's answer, 100 ms after the first";
    EXPECT_EQ(tcp_->counters().acks_rate_limited, 1U);
    const auto old_data = peer_.segment(TcpFlags{TcpFlag::Psh, TcpFlag::Ack}, payload(10), next, c->committed());
    receive(old_data);
    receive(old_data);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 2U) << "data that fails PAWS is answered every time, twice in one tick too";
    for (const ParsedFrame& frame : frames) {
        expect_pure_ack(frame, c->committed(), next);
    }
    EXPECT_EQ(tcp_->counters().dropped_paws, 4U);
    EXPECT_EQ(tcp_->counters().acks_rate_limited, 1U);
    EXPECT_EQ(c->unread().size(), 0U);
}

TEST_P(TcpChallenge, ASynInsideTheWindowIsChallenged) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence next = peer_.snd_nxt();
    TcpCounters expected   = tcp_->counters();
    receive(syn_at(peer_, next + 10U));
    EXPECT_EQ(c->state(), State::Established);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "one challenge";
    expect_pure_ack(frames[0], c->committed(), next);
    expected.segments_received      += 1;
    expected.dropped_syn_challenged += 1;
    expected.challenge_acks_sent    += 1;
    expected.pure_acks_sent         += 1;
    EXPECT_EQ(tcp_->counters(), expected) << "counted in dropped_syn_challenged alone, not in dropped_unexpected";
}

TEST_P(TcpChallenge, ASynOutsideTheWindowIsChallenged) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence next        = peer_.snd_nxt();
    const TcpSequence edge        = next + c->receive_window();
    // Before RCV.NXT, then past the edge, past the interval so the limiter lets the second go.
    const TcpSequence positions[] = {back(next, 100), edge + 100U};
    for (std::size_t round = 0; round < 2; ++round) {
        SCOPED_TRACE(::testing::Message() << "round " << round);
        if (round > 0) {
            advance(500ms);
        }
        TcpCounters expected = tcp_->counters();
        receive(syn_at(peer_, positions[round]));
        EXPECT_EQ(c->state(), State::Established);
        const auto frames = collect();
        ASSERT_EQ(frames.size(), 1U) << "challenged";
        expect_pure_ack(frames[0], c->committed(), next);
        expected.segments_received      += 1;
        expected.dropped_syn_challenged += 1;
        expected.challenge_acks_sent    += 1;
        expected.pure_acks_sent         += 1;
        EXPECT_EQ(tcp_->counters(), expected) << "counted in dropped_syn_challenged alone, wherever it lies";
    }
}

TEST_P(TcpChallenge, ASynThatFailsPawsIsChallenged) {
    peer_.set_timestamps(100);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence next = peer_.snd_nxt();
    peer_.set_timestamps(50);  // older than TS.Recent
    receive(syn_at(peer_, next + 10U));
    EXPECT_EQ(c->state(), State::Established);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "challenged";
    expect_pure_ack(frames[0], c->committed(), next);
    EXPECT_EQ(tcp_->counters().dropped_syn_challenged, 1U);
    EXPECT_EQ(tcp_->counters().challenge_acks_sent, 1U);
    EXPECT_EQ(tcp_->counters().dropped_paws, 0U);
}

TEST_P(TcpChallenge, AChallengedSynLeavesTsRecentAlone) {
    peer_.set_timestamps(100);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence next = peer_.snd_nxt();
    peer_.set_timestamps(200);  // newer, at RCV.NXT, which is Last.ACK.sent: a segment that passed would take it
    receive(syn_at(peer_, next));
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "challenged";
    EXPECT_EQ(ts_echo_of(frames[0]), 100U) << "the challenge echoes TS.Recent as it stood";
    ASSERT_EQ(c->send(payload(1)), 1U);
    frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(ts_echo_of(frames[0]), 100U) << "the stack's next segment: the SYN never wrote TS.Recent";
}

TEST_P(TcpChallenge, ASynAckOnAnEstablishedConnectionIsChallenged) {
    auto* c = open_active(peer_);
    ASSERT_NE(c, nullptr);
    TcpCounters expected = tcp_->counters();
    // The peer's SYN-ACK again, as when our ACK of it was lost: the challenge is that ACK.
    receive(peer_.segment(TcpFlags{TcpFlag::Syn, TcpFlag::Ack}, {}, peer_.iss(), c->committed(), true));
    EXPECT_EQ(c->state(), State::Established);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "one challenge";
    expect_pure_ack(frames[0], c->committed(), peer_.snd_nxt());
    expected.segments_received      += 1;
    expected.dropped_syn_challenged += 1;
    expected.challenge_acks_sent    += 1;
    expected.pure_acks_sent         += 1;
    EXPECT_EQ(tcp_->counters(), expected) << "counted in dropped_syn_challenged alone, not in dropped_duplicate";
}

TEST_P(TcpChallenge, ASynThatOverlapsRcvNxtIsChallengedAndNothingIsTaken) {
    peer_.set_sack_permitted(true);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->sack());
    const TcpSequence next = peer_.snd_nxt();
    // The SYN five numbers back with 10 bytes: the trim takes its SYN and the four bytes RCV.NXT passed, and the rule
    // reads the flag as the segment arrived.
    receive(peer_.segment(TcpFlags{TcpFlag::Syn, TcpFlag::Ack}, payload(10), back(next, 5), c->committed()));
    EXPECT_EQ(c->state(), State::Established);
    EXPECT_EQ(c->unread().size(), 0U) << "a SYN's payload is never taken";
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "challenged";
    expect_pure_ack(frames[0], c->committed(), next);
    EXPECT_EQ(frames[0].tcp->data_offset, 20U) << "no duplicate reported: no SACK option";
    EXPECT_EQ(tcp_->counters().dropped_syn_challenged, 1U);
    EXPECT_EQ(tcp_->counters().challenge_acks_sent, 1U);
}

TEST_P(TcpChallenge, AnAckOfUnsentDataIsChallengedAndItsPayloadNotTaken) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence next = peer_.snd_nxt();
    TcpCounters expected   = tcp_->counters();
    receive(data_at(peer_, next, c->committed() + 1U));
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "one challenge";
    expect_pure_ack(frames[0], c->committed(), next);
    EXPECT_EQ(c->unread().size(), 0U) << "dropped whole";
    expected.segments_received   += 1;
    expected.dropped_unexpected  += 1;
    expected.challenge_acks_sent += 1;
    expected.pure_acks_sent      += 1;
    EXPECT_EQ(tcp_->counters(), expected) << "counted in dropped_unexpected, as before, and challenged";
}

TEST_P(TcpChallenge, AnAckBelowSndUnaBeforeAnythingWasAcknowledgedIsTooOld) {
    // The handshake's own ACK is not in the sum, on either open: the bound is SND.UNA itself.
    for (const bool active : {false, true}) {
        SCOPED_TRACE(active ? "an active open" : "a passive open");
        TcpPeer peer{aloe::testing::peer_spec(active ? aloe::testing::tcp_peer_port + 1 : aloe::testing::tcp_peer_port),
                     TcpSequence{aloe::testing::tcp_peer_isn}};
        auto* c = active ? open_active(peer) : open_passive(peer);
        ASSERT_NE(c, nullptr);
        const TcpSequence next = peer.snd_nxt();
        TcpCounters expected   = tcp_->counters();
        receive(data_at(peer, next, back(c->acknowledged(), 1)));
        const auto frames = collect();
        ASSERT_EQ(frames.size(), 1U) << "one challenge";
        expect_pure_ack(frames[0], c->committed(), next);
        EXPECT_EQ(c->unread().size(), 0U) << "dropped whole";
        expected.segments_received   += 1;
        expected.dropped_old_ack     += 1;
        expected.challenge_acks_sent += 1;
        expected.pure_acks_sent      += 1;
        EXPECT_EQ(tcp_->counters(), expected) << "counted in dropped_old_ack alone";
    }
}

TEST_P(TcpChallenge, AnAckJustInsideTheBoundIsOldAndItsPayloadIsTaken) {
    peer_.set_timestamps(100);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_NO_FATAL_FAILURE(send_acknowledged(*c, peer_, 3000));
    const TcpSequence next         = peer_.snd_nxt();
    const TcpSequence acknowledged = c->acknowledged();
    peer_.set_timestamps(200);  // newer, at RCV.NXT: an old ACK inside the bound passed every check, and takes it
    receive(data_at(peer_, next, back(acknowledged, 3000)));
    EXPECT_EQ(c->unread().size(), 10U) << "taken";
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "the ACK of the data, and no challenge";
    expect_pure_ack(frames[0], c->committed(), next + 10U);
    EXPECT_EQ(ts_echo_of(frames[0]), 200U) << "the segment wrote TS.Recent";
    EXPECT_EQ(c->acknowledged(), acknowledged) << "an old ACK moves no SND.UNA";
    EXPECT_EQ(tcp_->counters().dropped_old_ack, 0U);
    EXPECT_EQ(tcp_->counters().challenge_acks_sent, 0U);
}

TEST_P(TcpChallenge, AnAckOneBelowTheBoundIsChallengedWithItsFin) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_NO_FATAL_FAILURE(send_acknowledged(*c, peer_, 3000));
    const TcpSequence next = peer_.snd_nxt();
    TcpCounters expected   = tcp_->counters();
    receive(peer_.segment(
        TcpFlags{TcpFlag::Fin, TcpFlag::Psh, TcpFlag::Ack}, payload(10), next, back(c->acknowledged(), 3001)));
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "one challenge";
    expect_pure_ack(frames[0], c->committed(), next);
    EXPECT_FALSE(c->peer_closed()) << "the FIN is dropped with the payload";
    EXPECT_EQ(c->state(), State::Established);
    EXPECT_EQ(c->unread().size(), 0U);
    expected.segments_received   += 1;
    expected.dropped_old_ack     += 1;
    expected.challenge_acks_sent += 1;
    expected.pure_acks_sent      += 1;
    EXPECT_EQ(tcp_->counters(), expected);
}

TEST_P(TcpChallenge, TheBoundIsTheLargestWindowOnceMoreWasAcknowledged) {
    peer_.set_window(1000);  // throughout: the largest window the peer ever offered is 1000
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_NO_FATAL_FAILURE(send_acknowledged(*c, peer_, 3000));
    const TcpSequence next = peer_.snd_nxt();
    receive(data_at(peer_, next, back(c->acknowledged(), 1000)));
    EXPECT_EQ(c->unread().size(), 10U) << "at the bound: taken";
    EXPECT_EQ(tcp_->counters().dropped_old_ack, 0U);
    std::ignore = collect();
    receive(data_at(peer_, next + 10U, back(c->acknowledged(), 1001)));
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "one challenge";
    expect_pure_ack(frames[0], c->committed(), next + 10U);
    EXPECT_EQ(c->unread().size(), 10U) << "one below it: dropped whole";
    EXPECT_EQ(tcp_->counters().dropped_old_ack, 1U);
    EXPECT_EQ(tcp_->counters().challenge_acks_sent, 1U);
}

TEST_P(TcpChallenge, TheAckBoundHoldsAcrossTheTopOfTheSequenceSpace) {
    force_next_isn(TcpSequence{0xfffffff0U});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_NO_FATAL_FAILURE(send_acknowledged(*c, peer_, 3000));
    ASSERT_EQ(c->acknowledged().value, 0xba9U) << "0xfffffff1 + 3000: SND.UNA wrapped";
    const TcpSequence bound = back(c->acknowledged(), 3000);
    ASSERT_EQ(bound.value, 0xfffffff1U);
    const TcpSequence next = peer_.snd_nxt();
    receive(data_at(peer_, next, bound));
    EXPECT_EQ(c->unread().size(), 10U) << "at the bound, across zero: taken";
    EXPECT_EQ(tcp_->counters().dropped_old_ack, 0U);
    std::ignore = collect();
    receive(data_at(peer_, next + 10U, back(bound, 1)));
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "one challenge";
    expect_pure_ack(frames[0], c->committed(), next + 10U);
    EXPECT_EQ(c->unread().size(), 10U) << "one below it: dropped whole";
    EXPECT_EQ(tcp_->counters().dropped_old_ack, 1U);
    EXPECT_EQ(tcp_->counters().challenge_acks_sent, 1U);
}

TEST_P(TcpChallenge, AnAckOutsideItsBoundsLeavesTsRecentAlone) {
    peer_.set_timestamps(100);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const TcpSequence next = peer_.snd_nxt();
    // Newer values at RCV.NXT, which is Last.ACK.sent: a segment that passed the ACK's checks would take them.
    peer_.set_timestamps(200);
    receive(data_at(peer_, next, back(c->acknowledged(), 1)));  // too old
    peer_.set_timestamps(300);
    receive(data_at(peer_, next, c->committed() + 1U));  // of what was never sent
    EXPECT_EQ(tcp_->counters().dropped_old_ack, 1U);
    EXPECT_EQ(tcp_->counters().dropped_unexpected, 1U);
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "the first challenge; the limiter holds the second back";
    EXPECT_EQ(ts_echo_of(frames[0]), 100U);
    ASSERT_EQ(c->send(payload(1)), 1U);
    frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(ts_echo_of(frames[0]), 100U) << "the stack's next segment: neither segment wrote TS.Recent";
}

TEST_P(TcpChallenge, ABadAckInSynReceivedLeavesTsRecentAlone) {
    ASSERT_TRUE(tcp_->listen(aloe::testing::tcp_listen_port).has_value());
    peer_.set_timestamps(100);
    receive(peer_.syn());
    const auto syn_ack = collect();
    ASSERT_EQ(syn_ack.size(), 1U);
    peer_.see(syn_ack[0]);
    // A newer value at RCV.NXT, which is Last.ACK.sent: a segment that passed every check would take it.
    peer_.set_timestamps(300);
    receive(peer_.segment(TcpFlags{TcpFlag::Ack}, {}, peer_.snd_nxt(), peer_.rcv_nxt() + 1U));
    const auto reset = collect();
    ASSERT_EQ(reset.size(), 1U);
    EXPECT_TRUE(reset[0].tcp->flags.has(TcpFlag::Rst)) << "step 0's reset";
    peer_.set_timestamps(200);
    receive(peer_.ack());
    const auto event = poll();
    ASSERT_TRUE(event.has_value() && event->events.accepted()) << "the right ACK passed PAWS: TS.Recent was not 300";
    auto* c = event->connection;
    ASSERT_EQ(c->send(payload(1)), 1U);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(ts_echo_of(frames[0]), 200U) << "the stack's first segment after the handshake: the right ACK's value";
}

TEST_P(TcpChallenge, ASegmentAboveAGapWithAnAckBelowTheBoundIsNotHeldAndARstAtItsEndIsChallenged) {
    peer_.set_sack_permitted(true);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->sack());
    const TcpSequence next = peer_.snd_nxt();
    // Held, its end would be a position at which a RST resets: the ACK's check comes before the hold.
    receive(
        peer_.segment(TcpFlags{TcpFlag::Psh, TcpFlag::Ack}, payload(100), next + 1000U, back(c->acknowledged(), 1)));
    EXPECT_EQ(c->out_of_order_segments(), 0U) << "nothing held";
    EXPECT_EQ(tcp_->counters().dropped_old_ack, 1U);
    EXPECT_EQ(tcp_->counters().out_of_order_held, 0U);
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "challenged";
    expect_pure_ack(frames[0], c->committed(), next);
    EXPECT_EQ(frames[0].tcp->data_offset, 20U) << "no block reported";
    advance(500ms);  // past the interval, so the limiter lets the next challenge go
    receive(rst_at(peer_, next + 1100U));
    EXPECT_EQ(c->state(), State::Established);
    EXPECT_FALSE(c->events().reset());
    frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "challenged, as a RST at no exact position";
    expect_pure_ack(frames[0], c->committed(), next);
    EXPECT_EQ(tcp_->counters().dropped_reset_challenged, 1U);
    EXPECT_EQ(tcp_->counters().challenge_acks_sent, 2U);
    EXPECT_EQ(tcp_->counters().connections_reset, 0U);
}

TEST_P(TcpChallenge, ABareFinAboveAGapWithAnAckBelowTheBoundIsNotRememberedAndARstPastItIsChallenged) {
    peer_.set_sack_permitted(true);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->sack());
    const TcpSequence next = peer_.snd_nxt();
    // Remembered, its number would end what is held, and a RST one past it would reset: the ACK's check comes first.
    receive(peer_.segment(TcpFlags{TcpFlag::Fin, TcpFlag::Ack}, {}, next + 1000U, back(c->acknowledged(), 1)));
    EXPECT_EQ(tcp_->counters().dropped_old_ack, 1U);
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "challenged";
    expect_pure_ack(frames[0], c->committed(), next);
    EXPECT_EQ(frames[0].tcp->data_offset, 20U) << "no block reported";
    advance(500ms);  // past the interval, so the limiter lets the next challenge go
    receive(rst_at(peer_, next + 1001U));
    EXPECT_EQ(c->state(), State::Established);
    EXPECT_FALSE(c->events().reset());
    frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "challenged, as a RST at no exact position";
    expect_pure_ack(frames[0], c->committed(), next);
    EXPECT_EQ(tcp_->counters().dropped_reset_challenged, 1U);
    EXPECT_EQ(tcp_->counters().challenge_acks_sent, 2U);
    EXPECT_EQ(tcp_->counters().connections_reset, 0U);
    receive(peer_.segment(TcpFlags{TcpFlag::Psh, TcpFlag::Ack}, payload(1000), next, peer_.rcv_nxt()));
    EXPECT_EQ(c->unread().size(), 1000U) << "the gap filled";
    EXPECT_FALSE(c->peer_closed()) << "and no FIN was remembered behind it";
}

TEST(TcpSendSequence, TheSumOfAdvancesStopsAtTheTopOf32Bits) {
    aloe::tcp::detail::SendSequence send;
    send.una     = TcpSequence{1000U};
    send.max_wnd = 65535U << 14U;  // the largest window a peer can offer, below 2^30
    send.count_acked(100U);
    EXPECT_EQ(send.ack_floor(), TcpSequence{900U}) << "below the largest window the sum is the bound";
    send.count_acked(0xffffff00U);
    EXPECT_EQ(send.bytes_acked, 0xffffff64U);
    send.count_acked(0x100U);
    EXPECT_EQ(send.bytes_acked, 0xffffffffU) << "stopped at the top, not wrapped to 0x64";
    send.count_acked(1U);
    EXPECT_EQ(send.bytes_acked, 0xffffffffU);
    EXPECT_EQ(send.ack_floor(), TcpSequence{1000U - (65535U << 14U)})
        << "the largest window is the bound: nothing lost";
}

TEST_P(TcpChallenge, TheBlocksOfAnAckBelowTheBoundMarkNothing) {
    peer_.set_sack_permitted(true);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->sack());
    ASSERT_NO_FATAL_FAILURE(send_acknowledged(*c, peer_, 3000));  // the bound stands 3000 below SND.UNA
    const TcpSequence una = c->acknowledged();
    for (int segment = 0; segment < 2; ++segment) {
        ASSERT_EQ(c->send(payload(500)), 500U);
        for (const ParsedFrame& frame : collect()) {
            peer_.see(frame);
        }
    }
    ASSERT_EQ(c->queued_segments(), 2U);
    const aloe::wire::TcpSackBlock second[] = {
        {.first = una + 500U, .end = una + 1000U}
    };
    const TcpCounters before = tcp_->counters();
    receive(peer_.sack(back(una, 3001), second));
    EXPECT_EQ(c->reported_bytes(), 0U) << "the block over the second entry marks nothing";
    EXPECT_EQ(tcp_->counters().sack_acks_received, before.sack_acks_received) << "the option is never read";
    EXPECT_EQ(tcp_->counters().dropped_old_ack, before.dropped_old_ack + 1);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "one challenge";
    expect_pure_ack(frames[0], c->committed(), peer_.snd_nxt());
    EXPECT_EQ(tcp_->counters().challenge_acks_sent, before.challenge_acks_sent + 1);
    receive(peer_.sack(back(una, 3000), second));
    EXPECT_EQ(c->reported_bytes(), 500U) << "the same block on an ACK at the bound is read";
}
