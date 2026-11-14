#include <algorithm>
#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>

namespace {

    using namespace std::chrono_literals;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;
    using aloe::wire::TcpSequence;
    using TcpHandshake = aloe::testing::TcpFixture;

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpHandshake,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

    [[nodiscard]] std::uint16_t mss_option(const aloe::frames::ParsedFrame& frame) {
        const auto options = aloe::wire::TcpOptions::parse(
            std::span<const std::byte>{frame.l4}.subspan(20, frame.tcp->data_offset - 20U));
        return options && options->mss ? *options->mss : 0;
    }

    [[nodiscard]] std::vector<std::byte> option_bytes(const aloe::frames::ParsedFrame& frame) {
        const auto options = aloe::frames::tcp_options_of(frame);
        return {options.begin(), options.end()};
    }

    /// The option bytes with the timestamp's TSval zeroed: a resent segment carries the clock's value then.
    [[nodiscard]] std::vector<std::byte> without_ts_value(const aloe::frames::ParsedFrame& frame) {
        std::vector<std::byte> bytes = option_bytes(frame);
        for (std::size_t at = 0; at < bytes.size();) {
            const auto kind = std::to_integer<std::uint8_t>(bytes[at]);
            if (kind == 0 || kind == 1 || at + 1 >= bytes.size()) {
                ++at;
                continue;
            }
            if (kind == 8 && at + 6 <= bytes.size()) {
                std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(at + 2), 4, std::byte{0});
            }
            at += std::max<std::size_t>(2, std::to_integer<std::size_t>(bytes[at + 1]));
        }
        return bytes;
    }

    /// The handshake's options in Linux's layout: the MSS; `sackOK, TS` or `nop, nop, TS` or `nop, nop, sackOK`;
    /// `nop, wscale`. `ts` carries the value and the echo, when the segment has the option.
    [[nodiscard]] std::vector<std::byte> linux_layout(const std::optional<std::pair<std::uint32_t, std::uint32_t>> ts,
                                                      const bool sack,
                                                      const std::optional<std::uint8_t> shift) {
        std::vector<std::vector<std::byte>> parts{aloe::frames::mss_option(aloe::testing::tcp_fabric_mss)};
        if (ts) {
            parts.push_back(sack ? aloe::frames::sack_permitted_timestamps_option(ts->first, ts->second)
                                 : aloe::frames::timestamps_option(ts->first, ts->second));
        } else if (sack) {
            parts.push_back(aloe::frames::sack_permitted_option());
        }
        if (shift) {
            parts.push_back(aloe::frames::window_scale_option(*shift));
        }
        return aloe::frames::joined(parts);
    }

    /// The TSval a frame carries; zero without the option.
    [[nodiscard]] std::uint32_t ts_value_of(const aloe::frames::ParsedFrame& frame) {
        const auto options = aloe::wire::TcpOptions::parse(aloe::frames::tcp_options_of(frame));
        return options && options->timestamp_value ? *options->timestamp_value : 0;
    }

    /// The handshake's options from both ends: each combination of what the peer offers, against the configuration.
    class TcpHandshakeOptions : public aloe::testing::TcpFixture {
    protected:
        /// A passive open per combination of the peer's offers, with and without its timestamps: the SYN-ACK's
        /// option bytes are Linux's layout of what both ends sent, and the connection agrees.
        void expect_negotiation(const bool sack_on, const bool scaling_on) {
            std::uint16_t peer_port = 41000;
            for (const bool ts : {false, true}) {
                for (const bool sack : {false, true}) {
                    for (const bool scaling : {false, true}) {
                        SCOPED_TRACE(::testing::Message()
                                     << "timestamps " << ts << ", sack " << sack << ", window scaling " << scaling);
                        aloe::frames::TcpPeer peer{aloe::testing::peer_spec(peer_port++),
                                                   TcpSequence{aloe::testing::tcp_peer_isn}};
                        if (ts) {
                            peer.set_timestamps(100);
                        }
                        peer.set_sack_permitted(sack);
                        if (scaling) {
                            peer.set_window_scale(7);
                        }
                        if (!tcp_->listening(aloe::testing::tcp_listen_port)) {
                            ASSERT_TRUE(tcp_->listen(aloe::testing::tcp_listen_port).has_value());
                        }
                        receive(peer.syn());
                        const auto syn_ack = collect();
                        ASSERT_EQ(syn_ack.size(), 1U);
                        const bool sack_used    = sack && sack_on;
                        const bool scaling_used = scaling && scaling_on;
                        const auto ts_pair =
                            ts ? std::optional{std::pair{ts_value_of(syn_ack[0]), 100U}} : std::nullopt;
                        EXPECT_EQ(option_bytes(syn_ack[0]),
                                  linux_layout(ts_pair,
                                               sack_used,
                                               scaling_used ? std::optional<std::uint8_t>{3} : std::nullopt));
                        peer.see(syn_ack[0]);
                        receive(peer.ack());
                        const auto event = poll();
                        ASSERT_TRUE(event.has_value() && event->events.accepted());
                        const auto& c = *event->connection;
                        EXPECT_EQ(c.sack(), sack_used);
                        EXPECT_EQ(c.window_scaling(), scaling_used);
                        EXPECT_EQ(c.window_shift(), scaling_used ? 3U : 0U);
                        EXPECT_EQ(c.peer_window_shift(), scaling_used ? 7U : 0U);
                    }
                }
            }
        }
    };

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpHandshakeOptions,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

}  // namespace

TEST_P(TcpHandshake, PassiveOpen) {
    ASSERT_TRUE(tcp_->listen(aloe::testing::tcp_listen_port).has_value());
    receive(peer_.syn());
    EXPECT_TRUE(burst_empty());
    EXPECT_EQ(tcp_->pending_events(), 0U) << "nobody owns the connection before the handshake completes";
    EXPECT_EQ(tcp_->table_size(), 1U);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    const auto& syn_ack = frames[0];
    ASSERT_TRUE(syn_ack.tcp.has_value());
    EXPECT_EQ(syn_ack.tcp->flags, (TcpFlags{TcpFlag::Syn, TcpFlag::Ack}));
    EXPECT_EQ(syn_ack.tcp->source_port, aloe::testing::tcp_listen_port);
    EXPECT_EQ(syn_ack.tcp->destination_port, aloe::testing::tcp_peer_port);
    EXPECT_EQ(syn_ack.tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 1});
    EXPECT_EQ(syn_ack.tcp->data_offset, 24U);
    EXPECT_EQ(mss_option(syn_ack), aloe::testing::tcp_fabric_mss);
    EXPECT_EQ(syn_ack.tcp->window, aloe::testing::tcp_fabric_budget);
    EXPECT_EQ(syn_ack.ethernet.destination, aloe::testing::harness_mac);
    EXPECT_EQ(aloe::frames::l4_checksum_residue(*syn_ack.ipv4, syn_ack.l4), 0U);
    EXPECT_EQ(wheel_.pending(), 1U) << "the SYN-ACK retransmit timer";
    const TcpSequence iss = syn_ack.tcp->sequence;

    peer_.see(syn_ack);
    receive(peer_.ack());
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.accepted());
    EXPECT_EQ(event->events, aloe::stream::Events{aloe::stream::Event::Accepted});
    auto& c = *event->connection;
    EXPECT_EQ(c.state(), aloe::tcp::State::Established);
    EXPECT_EQ(c.index(), 0U);
    EXPECT_EQ(&tcp_->connection(0), &c);
    EXPECT_EQ(c.local(), (aloe::tcp::Endpoint{aloe::testing::stack_ip, aloe::testing::tcp_listen_port}));
    EXPECT_EQ(c.remote(), (aloe::tcp::Endpoint{aloe::testing::harness_ip, aloe::testing::tcp_peer_port}));
    EXPECT_EQ(c.mss(), aloe::testing::tcp_fabric_mss);
    EXPECT_EQ(c.committed(), iss + 1U);
    EXPECT_EQ(c.acknowledged(), iss + 1U);
    EXPECT_EQ(c.unacknowledged(), 0U);
    EXPECT_FALSE(c.events().any()) << "poll_event cleared the snapshot";
    EXPECT_EQ(wheel_.pending(), 0U) << "the handshake ACK cancelled the timer";
    EXPECT_FALSE(poll().has_value());
    EXPECT_TRUE(collect().empty()) << "a pure ACK is not acknowledged";
    EXPECT_EQ(tcp_->counters().connections_accepted, 1U);
    EXPECT_EQ(tcp_->counters().control_segments_sent, 1U);
    EXPECT_EQ(tcp_->counters().segments_received, 2U);
}

TEST_P(TcpHandshake, ActiveOpen) {
    const auto connected =
        tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    auto& c = **connected;
    EXPECT_EQ(c.state(), aloe::tcp::State::SynSent);
    EXPECT_EQ(c.index(), 0U);
    EXPECT_EQ(wheel_.pending(), 1U);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    const auto& syn = frames[0];
    ASSERT_TRUE(syn.tcp.has_value());
    EXPECT_EQ(syn.tcp->flags, TcpFlags{TcpFlag::Syn});
    EXPECT_EQ(syn.tcp->destination_port, aloe::testing::tcp_peer_port);
    EXPECT_GE(syn.tcp->source_port, 32768U);
    EXPECT_LE(syn.tcp->source_port, 60999U);
    EXPECT_EQ(c.local().port, syn.tcp->source_port);
    EXPECT_EQ(syn.tcp->data_offset, 40U) << "mss, sackOK, TS, nop, wscale: the SYN offers every option";
    EXPECT_EQ(mss_option(syn), aloe::testing::tcp_fabric_mss);
    EXPECT_EQ(syn.tcp->window, aloe::testing::tcp_fabric_budget);
    EXPECT_EQ(syn.tcp->acknowledgement, TcpSequence{});
    EXPECT_EQ(aloe::frames::l4_checksum_residue(*syn.ipv4, syn.l4), 0U);
    const TcpSequence iss = syn.tcp->sequence;
    EXPECT_EQ(c.committed(), iss + 1U);

    receive(peer_.syn_ack(syn));
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.connected());
    EXPECT_EQ(event->connection, &c);
    EXPECT_EQ(c.state(), aloe::tcp::State::Established);
    EXPECT_EQ(c.remote(), (aloe::tcp::Endpoint{aloe::testing::harness_ip, aloe::testing::tcp_peer_port}));
    EXPECT_EQ(c.acknowledged(), iss + 1U);
    EXPECT_EQ(wheel_.pending(), 0U);
    EXPECT_TRUE(collect_queued().empty()) << "the handshake ACK waits for flush, so application data can carry it";
    const auto acks = collect();
    ASSERT_EQ(acks.size(), 1U);
    EXPECT_EQ(acks[0].tcp->flags, TcpFlags{TcpFlag::Ack});
    EXPECT_EQ(acks[0].tcp->sequence, iss + 1U);
    EXPECT_EQ(acks[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 1});
    EXPECT_EQ(acks[0].tcp->window, aloe::testing::tcp_fabric_budget);
    EXPECT_EQ(acks[0].tcp->data_offset, 20U) << "the peer's SYN-ACK carried no timestamps: off";
    EXPECT_EQ(tcp_->counters().connections_opened, 1U);
    EXPECT_EQ(tcp_->counters().pure_acks_sent, 1U);
    EXPECT_FALSE(poll().has_value());
}

TEST_P(TcpHandshake, DuplicateSyn) {
    ASSERT_TRUE(tcp_->listen(aloe::testing::tcp_listen_port).has_value());
    receive(peer_.syn());
    const auto first = collect();
    ASSERT_EQ(first.size(), 1U);
    aloe::frames::TcpPeer again{aloe::testing::peer_spec(), TcpSequence{aloe::testing::tcp_peer_isn}};
    receive(again.syn());
    const auto second = collect();
    ASSERT_EQ(second.size(), 1U);
    EXPECT_EQ(second[0].tcp->flags, (TcpFlags{TcpFlag::Syn, TcpFlag::Ack}));
    EXPECT_EQ(second[0].tcp->sequence, first[0].tcp->sequence) << "the same ISN, re-sent";
    EXPECT_EQ(tcp_->table_size(), 1U);
    EXPECT_EQ(tcp_->counters().control_segments_sent, 2U);
    EXPECT_EQ(tcp_->counters().dropped_table_full, 0U);
    EXPECT_FALSE(poll().has_value());
}

TEST_P(TcpHandshake, RefusedOpen) {
    receive(peer_.syn());  // nobody listens on 7
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->flags, (TcpFlags{TcpFlag::Rst, TcpFlag::Ack}));
    EXPECT_EQ(frames[0].tcp->sequence, TcpSequence{});
    EXPECT_EQ(frames[0].tcp->acknowledgement, TcpSequence{aloe::testing::tcp_peer_isn + 1}) << "SYN counts one";
    EXPECT_EQ(frames[0].tcp->source_port, aloe::testing::tcp_listen_port);
    EXPECT_EQ(frames[0].tcp->destination_port, aloe::testing::tcp_peer_port);
    EXPECT_EQ(tcp_->counters().dropped_no_connection, 1U);
    EXPECT_EQ(tcp_->counters().resets_sent, 1U);
    EXPECT_EQ(tcp_->table_size(), 0U);
}

TEST_P(TcpHandshake, ASynTheFabricDelayedIsAnsweredOnTheTickItArrives) {
    ASSERT_TRUE(tcp_->listen(aloe::testing::tcp_listen_port).has_value());
    harness_->set_impairment({.delay = 5ms});
    receive(peer_.syn());
    ASSERT_TRUE(collect().empty()) << "the SYN is still on the fabric";
    advance(5ms);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "the SYN-ACK on the tick the SYN arrives";
    EXPECT_EQ(frames[0].tcp->flags, (TcpFlags{TcpFlag::Syn, TcpFlag::Ack}));
}

TEST_P(TcpHandshake, StrayAckGetsRstWithRfc793Numbers) {
    receive(peer_.segment({TcpFlag::Ack}, {}, TcpSequence{1001U}, TcpSequence{777U}));
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->flags, TcpFlags{TcpFlag::Rst}) << "with ACK: a bare RST";
    EXPECT_EQ(frames[0].tcp->sequence, TcpSequence{777U}) << "at the stray segment's acknowledgement";
    EXPECT_EQ(tcp_->counters().dropped_no_connection, 1U);
    receive(peer_.rst());
    EXPECT_TRUE(collect().empty()) << "a RST is never answered with a RST";
    EXPECT_EQ(tcp_->counters().dropped_no_connection, 2U);
    EXPECT_EQ(tcp_->counters().resets_sent, 1U);
}

TEST_P(TcpHandshake, ResetInSynSentIsRefused) {
    const auto connected =
        tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    peer_.see(frames[0]);
    receive(peer_.rst());
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.reset());
    EXPECT_EQ((*connected)->state(), aloe::tcp::State::Closed);
    EXPECT_EQ(wheel_.pending(), 0U);
    EXPECT_EQ(tcp_->counters().connections_reset, 1U);
    EXPECT_EQ(tcp_->table_size(), 1U) << "a failed active open keeps its slot until release";
    (*connected)->release();
    EXPECT_EQ(tcp_->table_size(), 0U);
    EXPECT_TRUE(collect().empty()) << "release of a closed connection sends nothing";
}

TEST_P(TcpHandshake, ControlRetrySchedule) {
    const auto connected =
        tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    const auto first = collect();
    ASSERT_EQ(first.size(), 1U);
    const TcpSequence iss = first[0].tcp->sequence;
    advance(999ms);
    EXPECT_TRUE(collect().empty());
    std::uint64_t retransmits = 0;
    for (const auto at : {1000ms, 3000ms, 7000ms, 15000ms, 31000ms}) {
        advance(at - now_.time_since_epoch());  // the fixture's time starts at the epoch
        const auto frames = collect();
        ASSERT_EQ(frames.size(), 1U) << "a retransmission at " << at.count() << " ms";
        EXPECT_EQ(frames[0].tcp->flags, TcpFlags{TcpFlag::Syn});
        EXPECT_EQ(frames[0].tcp->sequence, iss) << "the original sequence";
        EXPECT_EQ(tcp_->counters().retransmits, ++retransmits);
        EXPECT_FALSE(poll().has_value());
    }
    advance(31999ms - 31000ms);
    EXPECT_TRUE(collect().empty());
    advance(63000ms - 31999ms);
    EXPECT_TRUE(collect().empty()) << "no sixth retransmission";
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.timed_out());
    EXPECT_EQ((*connected)->state(), aloe::tcp::State::Closed);
    EXPECT_EQ(wheel_.pending(), 0U);
    EXPECT_EQ(tcp_->counters().connections_timed_out, 1U);
    EXPECT_EQ(tcp_->counters().control_segments_sent, 6U);
}

TEST_P(TcpHandshake, PassiveRetryAndUnownedTimeout) {
    ASSERT_TRUE(tcp_->listen(aloe::testing::tcp_listen_port).has_value());
    receive(peer_.syn());
    const auto first = collect();
    ASSERT_EQ(first.size(), 1U);
    // The wheel fires a timer once per advance, so time moves in steps of one second: retries at 1, 3, 7, 15 and
    // 31 s, the timeout at 63 s.
    std::uint64_t sent = 1;
    for (std::int64_t second = 1; second < 63; ++second) {
        advance(1s);
        const auto frames = collect();
        if (second == 1 || second == 3 || second == 7 || second == 15 || second == 31) {
            ASSERT_EQ(frames.size(), 1U) << "a retransmission at " << second << " s";
            EXPECT_EQ(frames[0].tcp->flags, (TcpFlags{TcpFlag::Syn, TcpFlag::Ack}));
            EXPECT_EQ(frames[0].tcp->sequence, first[0].tcp->sequence);
            ++sent;
        } else {
            EXPECT_TRUE(frames.empty()) << "nothing at " << second << " s";
        }
        EXPECT_EQ(tcp_->counters().control_segments_sent, sent);
    }
    EXPECT_EQ(tcp_->table_size(), 1U) << "still waiting for the handshake before 63 s";
    advance(1s);
    EXPECT_TRUE(collect().empty()) << "no sixth retransmission";
    EXPECT_FALSE(poll().has_value()) << "nobody owned it: no event";
    EXPECT_EQ(tcp_->counters().handshakes_failed, 1U);
    EXPECT_EQ(tcp_->counters().connections_timed_out, 0U);
    EXPECT_EQ(tcp_->table_size(), 0U) << "the slot was freed";
    EXPECT_EQ(wheel_.pending(), 0U);
    std::ignore = collect();
    receive(peer_.syn());
    // Past 60 s the harness's ARP entry is stale, so IP refreshes it alongside the reply: count the TCP frames.
    EXPECT_EQ(
        std::ranges::count_if(collect(), [](const aloe::frames::ParsedFrame& frame) { return frame.tcp.has_value(); }),
        1)
        << "a fresh SYN gets a fresh SYN-ACK from the freed slot";
}

TEST_P(TcpHandshake, UnresolvedDoesNotSpendRetries) {
    // far_ip is behind the gateway, whose MAC nobody has learned: the SYN waits for ARP.
    const auto connected = tcp_->connect({.address = aloe::testing::far_ip, .port = 7}, now_);
    ASSERT_TRUE(connected.has_value());
    EXPECT_EQ((*connected)->state(), aloe::tcp::State::SynSent);
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_TRUE(frames[0].arp.has_value()) << "an ARP request for the gateway, no SYN";
    EXPECT_EQ(tcp_->counters().send_unresolved, 1U);
    EXPECT_EQ(wheel_.pending(), 1U);
    advance(10ms);
    EXPECT_TRUE(collect().empty()) << "still unresolved; the IP brick rate-limits its ARP requests";
    EXPECT_EQ(tcp_->counters().send_unresolved, 2U);
    EXPECT_EQ(tcp_->counters().retransmits, 0U);
    // The harness plays the gateway: it answers for gateway_ip with its own MAC, so the fabric delivers the SYN to it.
    receive(aloe::frames::arp_frame(
        aloe::frames::arp_reply(
            aloe::testing::harness_mac, aloe::testing::gateway_ip, aloe::testing::stack_mac, aloe::testing::stack_ip),
        aloe::testing::stack_mac));
    advance(10ms);
    frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    ASSERT_TRUE(frames[0].tcp.has_value());
    EXPECT_EQ(frames[0].tcp->flags, TcpFlags{TcpFlag::Syn});
    EXPECT_EQ(frames[0].ethernet.destination, aloe::testing::harness_mac) << "the gateway's MAC";
    EXPECT_EQ(frames[0].ipv4->destination, aloe::testing::far_ip);
    EXPECT_EQ(tcp_->counters().retransmits, 0U) << "the first successful send is not a retry";
    EXPECT_EQ(tcp_->counters().control_segments_sent, 1U);
    advance(999ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);
    EXPECT_EQ(collect().size(), 1U) << "normal retry timing starts from the successful send";
    EXPECT_EQ(tcp_->counters().retransmits, 1U);
}

TEST_P(TcpHandshake, UnresolvedPollsExhaustIntoUnresolved) {
    // far_ip is behind the gateway, whose MAC nobody ever answers for: the SYN never leaves.
    const auto connected = tcp_->connect({.address = aloe::testing::far_ip, .port = 7}, now_);
    ASSERT_TRUE(connected.has_value());
    std::ignore = collect();  // the ARP request
    for (int poll = 1; poll < 300; ++poll) {
        advance(10ms);
    }
    EXPECT_EQ((*connected)->state(), aloe::tcp::State::SynSent) << "299 polls: still waiting";
    EXPECT_EQ(tcp_->counters().send_unresolved, 300U) << "the first send and 299 polls";
    EXPECT_FALSE(poll().has_value());
    advance(10ms);  // the 300th poll, three seconds after the first send: the bound Linux's probes give
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.unresolved());
    EXPECT_FALSE(event->events.timed_out()) << "a different failure from a SYN that left and was ignored";
    EXPECT_EQ((*connected)->state(), aloe::tcp::State::Closed);
    EXPECT_EQ(wheel_.pending(), 0U);
    EXPECT_EQ(tcp_->counters().send_unresolved, 301U);
    EXPECT_EQ(tcp_->counters().connections_unresolved, 1U);
    EXPECT_EQ(tcp_->counters().connections_timed_out, 0U);
    EXPECT_EQ(tcp_->counters().retransmits, 0U) << "polls are not tries";
    EXPECT_EQ(tcp_->counters().control_segments_sent, 0U) << "the SYN never left";
    EXPECT_EQ(std::ranges::count_if(collect(), [](const aloe::frames::ParsedFrame& f) { return f.tcp.has_value(); }), 0)
        << "ARP requests only: no SYN and no RST";
    (*connected)->release();
    EXPECT_EQ(tcp_->table_size(), 0U);
}

TEST_P(TcpHandshake, UnresolvedSynAckFreesTheUnownedSlotSilently) {
    ASSERT_TRUE(tcp_->listen(aloe::testing::tcp_listen_port).has_value());
    aloe::frames::TcpSpec spec = aloe::testing::peer_spec();
    spec.source                = aloe::testing::far_ip;  // a SYN from beyond a gateway nobody answers for
    aloe::frames::TcpPeer far{spec, TcpSequence{5000U}};
    receive(far.syn());
    EXPECT_EQ(tcp_->table_size(), 1U);
    EXPECT_EQ(tcp_->counters().send_unresolved, 1U) << "the SYN-ACK waits for the gateway's MAC";
    for (int poll = 0; poll < 300; ++poll) {
        advance(10ms);
    }
    EXPECT_FALSE(poll().has_value()) << "nobody owned it: no event";
    EXPECT_EQ(tcp_->table_size(), 0U) << "the slot was freed";
    EXPECT_EQ(tcp_->counters().handshakes_failed, 1U);
    EXPECT_EQ(tcp_->counters().connections_unresolved, 0U);
    EXPECT_EQ(wheel_.pending(), 0U);
}

TEST_P(TcpHandshake, UnresolvedFinClosesAnEstablishedConnection) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    advance(121s);  // past arp_expire: the harness's MAC is forgotten and nobody answers for it again
    c->close();
    EXPECT_EQ(c->state(), aloe::tcp::State::FinWait1);
    EXPECT_EQ(tcp_->counters().send_unresolved, 1U) << "the FIN waits for ARP";
    for (int poll = 0; poll < 300; ++poll) {
        advance(10ms);
    }
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.unresolved());
    EXPECT_EQ(c->state(), aloe::tcp::State::Closed);
    EXPECT_EQ(tcp_->counters().connections_unresolved, 1U);
    EXPECT_EQ(tcp_->counters().connections_closed, 0U) << "not a normal close";
    EXPECT_EQ(wheel_.pending(), 0U);
}

TEST_P(TcpHandshake, AbortInSynSentSendsNoReset) {
    const auto connected =
        tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    ASSERT_EQ(collect().size(), 1U) << "the SYN";
    (*connected)->abort();
    EXPECT_EQ((*connected)->state(), aloe::tcp::State::Closed);
    EXPECT_TRUE(collect().empty()) << "RFC 9293's ABORT in SYN-SENT: nothing to take back, so no RST";
    EXPECT_EQ(tcp_->counters().resets_sent, 0U);
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.closed());
    EXPECT_EQ(wheel_.pending(), 0U);
}

TEST_P(TcpHandshake, NoRouteTakesNoSlot) {
    Ipv4 bare{
        queue_, {.address = aloe::testing::stack_ip, .prefix = 24}
    };
    Tcp tcp{bare, wheel_, {.connections = 1}};
    const auto failed = tcp.connect({.address = aloe::testing::far_ip, .port = 7}, now_);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error(), aloe::tcp::ConnectError::NoRoute);
    EXPECT_EQ(tcp.table_size(), 0U);
    EXPECT_EQ(wheel_.pending(), 0U);
    const auto ok = tcp.connect({.address = aloe::testing::harness_ip, .port = 7}, now_);
    ASSERT_TRUE(ok.has_value()) << "the one slot is still free";
    EXPECT_EQ((*ok)->index(), 0U);
    (*ok)->release();
    std::ignore = collect();
}

TEST_P(TcpHandshake, TableFullAndUnlisten) {
    configure_tcp({.connections = 1});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    aloe::frames::TcpPeer second{aloe::testing::peer_spec(40001), TcpSequence{2000U}};
    receive(second.syn());
    EXPECT_TRUE(collect().empty()) << "a full table drops the SYN silently";
    EXPECT_EQ(tcp_->counters().dropped_table_full, 1U);
    tcp_->unlisten(aloe::testing::tcp_listen_port);
    EXPECT_FALSE(tcp_->listening(aloe::testing::tcp_listen_port));
    EXPECT_EQ(c->state(), aloe::tcp::State::Established) << "unlisten touches no open connection";
    c->release();
    std::ignore = collect();
    receive(second.syn());
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_TRUE(frames[0].tcp->flags.has(TcpFlag::Rst)) << "nobody listens any more";
    ASSERT_TRUE(tcp_->listen(aloe::testing::tcp_listen_port).has_value());
    aloe::frames::TcpPeer third{aloe::testing::peer_spec(40002), TcpSequence{3000U}};
    EXPECT_NE(open_passive(third), nullptr) << "the released slot serves the next open";
}

TEST_P(TcpHandshake, ListenErrors) {
    ASSERT_TRUE(tcp_->listen(7).has_value());
    const auto again = tcp_->listen(7);
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error(), aloe::tcp::ListenError::InUse);
    configure_tcp({.listeners = 1});
    ASSERT_TRUE(tcp_->listen(7).has_value());
    const auto full = tcp_->listen(8);
    ASSERT_FALSE(full.has_value());
    EXPECT_EQ(full.error(), aloe::tcp::ListenError::TableFull);
    tcp_->unlisten(7);
    EXPECT_TRUE(tcp_->listen(8).has_value());
}

TEST_P(TcpHandshake, APassiveOpenStartsAtTheInitialWindow) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->congestion_window(), 14600U) << "RFC 6928 at MSS 1460";
}

TEST_P(TcpHandshake, ConfigValidation) {
    using Config = aloe::tcp::TcpConfig;
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.connections = 0}}), std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.listeners = 0}}), std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.receive_segments = 0}}), std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.receive_pool = 0}}), std::invalid_argument);
    EXPECT_THROW((std::ignore =
                      Tcp{
                          ip_, wheel_, Config{.ephemeral_first = 50000, .ephemeral_last = 40000}
    }),
                 std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.ephemeral_first = 0}}), std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.rto_initial = std::chrono::seconds{0}}}),
                 std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.retries = 0}}), std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.retries = 40}}), std::invalid_argument)
        << "the doubled delay would overflow core::Duration";
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.rto_initial = std::chrono::hours{24 * 365 * 100}}}),
                 std::invalid_argument)
        << "five doublings of a century overflow core::Duration";
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.unresolved_retry = std::chrono::milliseconds{-1}}}),
                 std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.unresolved_polls = 0}}), std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.connections = std::size_t{1} << 31U}}), std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.send_segments = 0}}), std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.send_pool = 0}}), std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.rto_min = std::chrono::microseconds{500}}}),
                 std::invalid_argument)
        << "below the wheel's 1 ms resolution";
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.rto_initial = std::chrono::milliseconds{100}}}),
                 std::invalid_argument)
        << "rto_min must not exceed rto_initial";
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.rto_max = std::chrono::milliseconds{500}}}),
                 std::invalid_argument)
        << "rto_initial must not exceed rto_max";
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.data_retries = 0}}), std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.user_timeout = std::chrono::seconds{-1}}}),
                 std::invalid_argument);
    EXPECT_NO_THROW(
        (std::ignore = Tcp{
             ip_, wheel_, Config{.rto_min = std::chrono::milliseconds{1}, .user_timeout = std::chrono::seconds{2}}
    }));
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, {.receive_segments = 2049}}), std::invalid_argument)
        << "above the pool";
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, {.receive_window = (65535U << 14U) + 1}}), std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, {.delayed_ack = -1ms}}), std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, {.delayed_ack = 501ms}}), std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, {.delayed_ack = 500us}}), std::invalid_argument)
        << "below the 1 ms wheel";
    EXPECT_NO_THROW((std::ignore = Tcp{
                         ip_, wheel_, {.receive_window = 65535U << 14U, .delayed_ack = 500ms}
    }));
    EXPECT_NO_THROW((std::ignore = Tcp{ip_, wheel_, {.receive_segments = 2048}})) << "a cap equal to the pool";
    EXPECT_NO_THROW((std::ignore = Tcp{ip_, wheel_, {.delayed_ack = 1ms}}))
        << "a delay equal to the wheel's resolution";
    const Tcp capped{
        ip_, wheel_, Config{.receive_segments = 1000, .window_scaling = false}
    };
    EXPECT_EQ(capped.byte_budget(), 65535U) << "the byte budget never passes the wire window";
    const Tcp scaled{ip_, wheel_, Config{.receive_segments = 1000}};
    EXPECT_EQ(scaled.byte_budget(), 730000U) << "1000 * 1460 / 2, within 65535 << 4";
    EXPECT_EQ(capped.mss(), aloe::testing::tcp_fabric_mss);
    EXPECT_EQ(capped.capacity(), 1024U);
    EXPECT_EQ(wheel_.pending(), 0U) << "construction arms nothing";
}

TEST_P(TcpHandshakeOptions, TheOptionsAreInUseOnlyWhenBothEndsSendThem) {
    expect_negotiation(true, true);
}

TEST_P(TcpHandshakeOptions, SackOffByConfigIsNeverOffered) {
    configure_tcp({.sack = false});
    expect_negotiation(false, true);
}

TEST_P(TcpHandshakeOptions, WindowScalingOffByConfigIsNeverOffered) {
    configure_tcp({.window_scaling = false});
    expect_negotiation(true, false);
}

TEST_P(TcpHandshake, AnActiveOpenOffersBothAndTakesThePeersAnswer) {
    std::uint16_t peer_port = 42000;
    for (const bool sack : {false, true}) {
        for (const bool scaling : {false, true}) {
            SCOPED_TRACE(::testing::Message() << "sack " << sack << ", window scaling " << scaling);
            aloe::frames::TcpSpec spec = aloe::testing::peer_spec(peer_port++);
            aloe::frames::TcpPeer peer{spec, TcpSequence{aloe::testing::tcp_peer_isn}};
            peer.set_timestamps(100);
            peer.set_sack_permitted(sack);
            if (scaling) {
                peer.set_window_scale(7);
            }
            const auto connected =
                tcp_->connect({.address = aloe::testing::harness_ip, .port = spec.source_port}, now_);
            ASSERT_TRUE(connected.has_value());
            auto& c        = **connected;
            const auto syn = collect();
            ASSERT_EQ(syn.size(), 1U);
            EXPECT_EQ(syn[0].tcp->data_offset, 40U) << "mss, sackOK, TS, nop, wscale";
            EXPECT_EQ(option_bytes(syn[0]), linux_layout(std::pair{ts_value_of(syn[0]), 0U}, true, std::uint8_t{3}))
                << "mss 1460, sackOK, TS, nop, wscale 3";
            EXPECT_TRUE(c.sack()) << "in SynSent, the SYN's offer";
            receive(peer.syn_ack(syn[0]));
            const auto event = poll();
            ASSERT_TRUE(event.has_value() && event->events.connected());
            EXPECT_EQ(c.sack(), sack);
            EXPECT_EQ(c.window_scaling(), scaling);
            EXPECT_EQ(c.window_shift(), scaling ? 3U : 0U);
            EXPECT_EQ(c.peer_window_shift(), scaling ? 7U : 0U);
            const auto ack = collect();
            ASSERT_EQ(ack.size(), 1U);
            EXPECT_EQ(ack[0].tcp->window, scaling ? 46336U : 65535U) << "370688 >> 3, or the unscaled cap";
        }
    }
}

TEST_P(TcpHandshake, AnActiveOpenWithoutTimestampsOffersBothInTwelveBytes) {
    configure_tcp({.timestamps = false});
    const auto connected =
        tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    const auto syn = collect();
    ASSERT_EQ(syn.size(), 1U);
    EXPECT_EQ(syn[0].tcp->data_offset, 32U);
    EXPECT_EQ(option_bytes(syn[0]), linux_layout(std::nullopt, true, std::uint8_t{3}))
        << "mss, nop, nop, sackOK, nop, wscale 3";
}

TEST_P(TcpHandshake, ASynAndASynAckStateTheWindowUnscaled) {
    const auto connected =
        tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    const auto syn = collect();
    ASSERT_EQ(syn.size(), 1U);
    EXPECT_EQ(syn[0].tcp->window, 65535U) << "min(373760, 65535), unscaled";
    peer_.set_sack_permitted(true);
    peer_.set_window_scale(7);
    ASSERT_TRUE(tcp_->listen(aloe::testing::tcp_listen_port).has_value());
    receive(peer_.syn());
    const auto syn_ack = collect();
    ASSERT_EQ(syn_ack.size(), 1U);
    ASSERT_TRUE(aloe::testing::is_flags(syn_ack[0], {TcpFlag::Syn, TcpFlag::Ack}));
    EXPECT_EQ(syn_ack[0].tcp->window, 65535U) << "min(budget, 65535) to a scaling peer, unscaled";
    peer_.see(syn_ack[0]);
    EXPECT_EQ(peer_.stack_window_scale(), std::optional<std::uint8_t>{3});
    EXPECT_EQ(peer_.stack_window_bytes(), 65535U) << "the SYN-ACK's field is bytes";
}

TEST_P(TcpHandshake, TheFirstAckAfterTheHandshakeStatesTheBudgetScaled) {
    peer_.set_window_scale(7);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->receive_budget(), 373760U) << "512 * 1460 / 2: no timestamps";
    receive(peer_.data(aloe::frames::pattern(1)));
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, (373760U - 1U + 7U) >> 3U) << "46720";
    peer_.see(frames[0]);
    EXPECT_EQ(peer_.stack_window_bytes(), 46720U << 3U);
    EXPECT_EQ(c->receive_window(), 46720U << 3U) << "the edge the peer reconstructs";
    ASSERT_TRUE(poll().has_value()) << "the byte's Readable, drained before the next open";

    aloe::frames::TcpPeer stamped{aloe::testing::peer_spec(40001), TcpSequence{aloe::testing::tcp_peer_isn}};
    stamped.set_timestamps(100);
    stamped.set_window_scale(7);
    auto* d = open_passive(stamped);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->receive_budget(), 370688U) << "512 * 1448 / 2";
}

TEST_P(TcpHandshake, ABudgetTheFieldStatesUnscaledOffersAZeroShift) {
    configure_tcp({.receive_segments = 32});
    // The largest budget, 32 * 1460 / 2 = 23360, fits the field unscaled: the shift is 0, offered all the same.
    const auto connected =
        tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    const auto syn = collect();
    ASSERT_EQ(syn.size(), 1U);
    EXPECT_EQ(option_bytes(syn[0]), linux_layout(std::pair{ts_value_of(syn[0]), 0U}, true, std::uint8_t{0}))
        << "mss 1460, sackOK, TS, nop, wscale 0";
    EXPECT_EQ(syn[0].tcp->window, 23360U);

    peer_.set_timestamps(100);
    peer_.set_sack_permitted(true);
    peer_.set_window_scale(7);
    ASSERT_TRUE(tcp_->listen(aloe::testing::tcp_listen_port).has_value());
    receive(peer_.syn());
    const auto syn_ack = collect();
    ASSERT_EQ(syn_ack.size(), 1U);
    ASSERT_TRUE(aloe::testing::is_flags(syn_ack[0], {TcpFlag::Syn, TcpFlag::Ack}));
    EXPECT_EQ(option_bytes(syn_ack[0]), linux_layout(std::pair{ts_value_of(syn_ack[0]), 100U}, true, std::uint8_t{0}))
        << "mss 1460, sackOK, TS, nop, wscale 0";
    EXPECT_EQ(syn_ack[0].tcp->window, 23168U) << "32 * 1448 / 2 with timestamps";
}

TEST_P(TcpHandshake, APeerShiftAboveFourteenIsUsedAsFourteen) {
    peer_.set_window_scale(15);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->peer_window_shift(), 14U);
    EXPECT_TRUE(c->window_scaling());
}

TEST_P(TcpHandshake, ARetransmittedSynOffersEveryOptionAgain) {
    const auto connected =
        tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    const auto first = collect();
    ASSERT_EQ(first.size(), 1U);
    advance(1s);
    const auto again = collect();
    ASSERT_EQ(again.size(), 1U);
    ASSERT_TRUE(aloe::testing::is_flags(again[0], {TcpFlag::Syn}));
    EXPECT_EQ(again[0].tcp->data_offset, first[0].tcp->data_offset);
    EXPECT_EQ(without_ts_value(again[0]), without_ts_value(first[0]));
    EXPECT_EQ(without_ts_value(again[0]), linux_layout(std::pair{0U, 0U}, true, std::uint8_t{3}));
    EXPECT_EQ(again[0].tcp->window, first[0].tcp->window);
}

TEST_P(TcpHandshake, ARetransmittedSynAckKeepsItsOptions) {
    peer_.set_timestamps(100);
    peer_.set_sack_permitted(true);
    peer_.set_window_scale(7);
    ASSERT_TRUE(tcp_->listen(aloe::testing::tcp_listen_port).has_value());
    receive(peer_.syn());
    const auto first = collect();
    ASSERT_EQ(first.size(), 1U);
    advance(1s);
    const auto again = collect();
    ASSERT_EQ(again.size(), 1U);
    ASSERT_TRUE(aloe::testing::is_flags(again[0], {TcpFlag::Syn, TcpFlag::Ack}));
    EXPECT_EQ(again[0].tcp->data_offset, first[0].tcp->data_offset);
    EXPECT_EQ(without_ts_value(again[0]), without_ts_value(first[0]));
    EXPECT_EQ(without_ts_value(again[0]), linux_layout(std::pair{0U, 100U}, true, std::uint8_t{3}));
    EXPECT_EQ(again[0].tcp->window, 65535U);
}

TEST_P(TcpHandshake, ASynAckSentAgainAfterARestatingAckNeverMovesTheEdgeLeft) {
    ASSERT_TRUE(tcp_->listen(aloe::testing::tcp_listen_port).has_value());
    // Two passive opens to a scaling peer, each answered with the edge restated while still SynReceived: the restating
    // ACK states 65535 rounded up to the unit of 8, so the recorded edge stands one byte past the SYN-ACK's.
    const auto restated = [this](aloe::frames::TcpPeer& peer, const std::uint32_t slot) {
        peer.set_window_scale(7);
        receive(peer.syn());
        const auto syn_ack = collect();
        ASSERT_EQ(syn_ack.size(), 1U);
        peer.see(syn_ack[0]);
        receive(peer.segment(TcpFlags{TcpFlag::Ack}, {}, peer.snd_nxt() + 70000U, peer.rcv_nxt()));
        const auto ack = collect();
        ASSERT_EQ(ack.size(), 1U) << "a segment past the edge, answered at once";
        EXPECT_TRUE(aloe::testing::is_flags(ack[0], {TcpFlag::Ack}));
        EXPECT_EQ(ack[0].tcp->window, 8192U) << "(65535 + 7) >> 3";
        EXPECT_EQ(tcp_->connection(slot).state(), aloe::tcp::State::SynReceived);
        EXPECT_EQ(tcp_->connection(slot).receive_window(), 65536U);
    };

    // The SYN again: the SYN-ACK from its entry.
    restated(peer_, 0);
    aloe::frames::TcpPeer again{aloe::testing::peer_spec(), TcpSequence{aloe::testing::tcp_peer_isn}};
    again.set_window_scale(7);
    receive(again.syn());
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    ASSERT_TRUE(aloe::testing::is_flags(frames[0], {TcpFlag::Syn, TcpFlag::Ack}));
    EXPECT_EQ(frames[0].tcp->window, 65535U) << "the handshake's field, unscaled";
    EXPECT_EQ(tcp_->connection(0).receive_window(), 65536U) << "the edge did not move left";

    // The SYN-ACK's timer, on a second connection that did not see its SYN again.
    aloe::frames::TcpPeer timed{aloe::testing::peer_spec(40001), TcpSequence{aloe::testing::tcp_peer_isn}};
    restated(timed, 1);
    advance(1s);
    frames = collect();
    ASSERT_EQ(frames.size(), 2U) << "both SYN-ACKs, each by its timer";
    for (const auto& frame : frames) {
        ASSERT_TRUE(aloe::testing::is_flags(frame, {TcpFlag::Syn, TcpFlag::Ack}));
        EXPECT_EQ(frame.tcp->window, 65535U);
    }
    EXPECT_EQ(tcp_->connection(0).receive_window(), 65536U);
    EXPECT_EQ(tcp_->connection(1).receive_window(), 65536U);

    // The handshake completes, and the first ACK of data states the budget as before.
    peer_.see(frames[0].tcp->destination_port == peer_.spec().source_port ? frames[0] : frames[1]);
    receive(peer_.ack());
    const auto event = poll();
    ASSERT_TRUE(event.has_value() && event->events.accepted());
    receive(peer_.data(aloe::frames::pattern(1)));
    frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->window, (373760U - 1U + 7U) >> 3U) << "46720, as after any passive open";
}

TEST_P(TcpHandshake, AnActiveOpenThatEndsUnansweredNegotiatedNothing) {
    configure_tcp({.retries = 1});
    const auto connected =
        tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    auto& c = **connected;
    ASSERT_EQ(collect().size(), 1U);
    EXPECT_TRUE(c.sack()) << "the SYN offers it";
    advance(1s);
    ASSERT_EQ(collect().size(), 1U) << "the one retry";
    advance(2s);
    const auto event = poll();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.timed_out());
    EXPECT_EQ(c.state(), aloe::tcp::State::Closed);
    EXPECT_FALSE(c.sack()) << "the offer was never answered";
    EXPECT_FALSE(c.window_scaling());
    EXPECT_EQ(c.peer_window_shift(), 0U);
}

TEST_P(TcpHandshake, ASynAckWithDataKeepsItsWindowUnscaled) {
    peer_.set_window_scale(7);
    peer_.set_window(1000);
    const auto connected =
        tcp_->connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}, now_);
    ASSERT_TRUE(connected.has_value());
    auto& c        = **connected;
    const auto syn = collect();
    ASSERT_EQ(syn.size(), 1U);
    peer_.see(syn[0]);
    receive(peer_.segment(
        TcpFlags{TcpFlag::Syn, TcpFlag::Ack}, aloe::frames::pattern(1), peer_.snd_nxt(), peer_.rcv_nxt(), true));
    ASSERT_EQ(c.state(), aloe::tcp::State::Established);
    EXPECT_EQ(c.unread().size(), 1U) << "the SYN-ACK's byte, through the synchronized path";
    EXPECT_EQ(c.peer_window_shift(), 7U);
    EXPECT_EQ(c.writable(), 1000U) << "the SYN-ACK's field is bytes, its payload notwithstanding";
    std::ignore = collect();
    receive(peer_.segment(TcpFlags{TcpFlag::Ack}, {}, peer_.snd_nxt() + 2U, c.committed()));
    EXPECT_EQ(c.writable(), 1460U) << "the next segment's field of 1000 is 128000 bytes";
}
