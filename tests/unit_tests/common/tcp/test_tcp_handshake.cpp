#include <algorithm>
#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <tuple>

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
    EXPECT_EQ(syn.tcp->data_offset, 24U);
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
    EXPECT_EQ(acks[0].tcp->data_offset, 20U);
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
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.retry_initial = std::chrono::seconds{0}}}),
                 std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.retries = 0}}), std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.unresolved_retry = std::chrono::milliseconds{-1}}}),
                 std::invalid_argument);
    EXPECT_THROW((std::ignore = Tcp{ip_, wheel_, Config{.connections = std::size_t{1} << 31U}}), std::invalid_argument);
    const Tcp capped{ip_, wheel_, Config{.receive_segments = 1000}};
    EXPECT_EQ(capped.byte_budget(), 65535U) << "the byte budget never passes the wire window";
    EXPECT_EQ(capped.mss(), aloe::testing::tcp_fabric_mss);
    EXPECT_EQ(capped.capacity(), 1024U);
    EXPECT_EQ(wheel_.pending(), 0U) << "construction arms nothing";
}
