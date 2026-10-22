#include <aloe/ethdev>
#include <aloe/frames>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <application_work.hpp>
#include <eal_environment.hpp>
#include <gtest/gtest.h>
#include <level_floor.hpp>
#include <ring_pair.hpp>
#include <run_state.hpp>
#include <stamped_device.hpp>
#include <venue_peer.hpp>

// The venue over the peer port and the floor over the stack port: the first closed-loop rounds, with the stack's
// mbufs recycled by the peer. Same-core inline, then cross-core on an unpinned thread. No root, no hugepages.
namespace {

    using namespace aloe::benchmarks;
    namespace ethdev = aloe::ethdev;
    namespace frames = aloe::frames;
    namespace wire   = aloe::wire;
    using namespace std::chrono_literals;
    using Packet  = ethdev::Packet;
    using Stamped = StampedDevice<ethdev::Port>;

    const auto* const environment = ::testing::AddGlobalTestEnvironment(new aloe::testing::EalEnvironment{"net_null0"});

    [[nodiscard]] RoundConfig small(const Round round, const Topology topology, const std::size_t payload) {
        return {
            .round = round, .level = Level::Floor, .topology = topology, .payload = payload, .rounds = 50, .warmup = 5};
    }

    [[nodiscard]] PeerConfig floor_peer(RingPair& pair) {
        return {.stack_mac = pair.stack().mac(), .strict = false, .deadline = 2s};
    }

    void expect_closed_loop(const RunState& state, const Stamped& device, const VenuePeer& peer) {
        EXPECT_FALSE(state.failed.load()) << state.failure;
        EXPECT_TRUE(state.stop.load()) << "the level finishes the run";
        EXPECT_EQ(device.stamps_taken(), state.config.total());
        EXPECT_EQ(peer.counters().orders_seen, state.config.total());
        for (std::size_t round = 0; round < state.config.total(); ++round) {
            EXPECT_NE(state.stamps.start[round], 0U) << round;
            EXPECT_GT(state.stamps.stop[round], state.stamps.start[round]) << round;
        }
    }

}  // namespace

TEST(VenuePeer, FloorTickRoundSameCore) {
    RingPair pair{
        {.name = "flr-tick-same", .pool_size = 512}
    };
    RunState state{small(Round::Tick, Topology::SameCore, 64)};
    Stamped device{pair.stack(), state.stamps.stop};
    VenuePeer peer{pair.peer(), floor_peer(pair), state.stamps.start};
    run_floor(state, device, floor_peer(pair), &peer);
    expect_closed_loop(state, device, peer);
    EXPECT_EQ(peer.counters().surplus_freed, 0U) << "every order came back as the next tick";
}

TEST(VenuePeer, FloorTickRoundCrossCore) {
    RingPair pair{
        {.name = "flr-tick-cross", .pool_size = 512}
    };
    RunState state{small(Round::Tick, Topology::CrossCore, 1460)};
    Stamped device{pair.stack(), state.stamps.stop};
    VenuePeer peer{pair.peer(), floor_peer(pair), state.stamps.start};
    std::jthread venue{[&peer, &state] { drive_peer(peer, state, false); }};
    run_floor(state, device, floor_peer(pair), nullptr);
    venue.join();
    expect_closed_loop(state, device, peer);
}

TEST(VenuePeer, FloorGatewayRoundBothTopologies) {
    {
        RingPair pair{
            {.name = "flr-gw-same", .pool_size = 512}
        };
        RunState state{small(Round::Gateway, Topology::SameCore, order_size)};
        Stamped device{pair.stack(), state.stamps.stop};
        VenuePeer peer{pair.peer(), floor_peer(pair), state.stamps.start};
        run_floor(state, device, floor_peer(pair), &peer);
        expect_closed_loop(state, device, peer);
    }
    {
        RingPair pair{
            {.name = "flr-gw-cross", .pool_size = 512}
        };
        RunState state{small(Round::Gateway, Topology::CrossCore, order_size)};
        Stamped device{pair.stack(), state.stamps.stop};
        VenuePeer peer{pair.peer(), floor_peer(pair), state.stamps.start};
        std::jthread venue{[&peer, &state] { drive_peer(peer, state, false); }};
        run_floor(state, device, floor_peer(pair), nullptr);
        venue.join();
        expect_closed_loop(state, device, peer);
    }
}

TEST(VenuePeer, FloorSameCoreTickRoundHonoursTheGap) {
    RingPair pair{
        {.name = "flr-gap", .pool_size = 256}
    };
    RoundConfig config = small(Round::Tick, Topology::SameCore, 64);
    config.rounds      = 5;
    config.warmup      = 0;
    config.gap         = 1ms;
    RunState state{config};
    Stamped device{pair.stack(), state.stamps.stop};
    VenuePeer peer{pair.peer(), floor_peer(pair), state.stamps.start};
    run_floor(state, device, floor_peer(pair), &peer);
    expect_closed_loop(state, device, peer);
    const std::uint64_t gap = ticks_for(config.gap);
    for (std::size_t round = 1; round < state.config.total(); ++round) {
        EXPECT_GE(state.stamps.start[round] - state.stamps.stop[round - 1], gap) << round;
    }
}

TEST(VenuePeer, FloorSameCoreFailsALostTickAtTheDeadline) {
    RingPair pair{
        {.name = "flr-lost", .pool_size = 256}
    };
    std::vector<Packet> taken;  // the peer's whole pool: send_tick finds no mbuf for its first carrier
    for (auto packet = pair.peer().allocate(0); packet.has_value(); packet = pair.peer().allocate(0)) {
        taken.push_back(std::move(*packet));
    }
    RunState state{small(Round::Tick, Topology::SameCore, 64)};
    Stamped device{pair.stack(), state.stamps.stop};
    PeerConfig config = floor_peer(pair);
    config.deadline   = 50ms;
    VenuePeer peer{pair.peer(), config, state.stamps.start};
    run_floor(state, device, config, &peer);
    EXPECT_TRUE(state.failed.load());
    EXPECT_EQ(state.failure, "floor, round 0: no tick within the deadline");
    EXPECT_EQ(device.stamps_taken(), 0U);
}

TEST(VenuePeer, AwaitOrderSkipsPureAcksChecksTheRoundAndTheChecksums) {
    RingPair pair{
        {.name = "venue-await", .pool_size = 256}
    };
    std::array<std::uint64_t, 4> starts{};
    PeerConfig config = floor_peer(pair);
    config.strict     = true;
    VenuePeer peer{pair.peer(), config, starts};
    // The stack's side, by hand: frames from the stack port's pool, as the brick would send them.
    const auto stack_sends = [&pair](const std::vector<std::byte>& frame) {
        auto packet = pair.stack().allocate(0);
        ASSERT_TRUE(packet.has_value());
        ASSERT_TRUE(frames::fill(*packet, frame));
        std::array<Packet, 1> burst{std::move(*packet)};
        ASSERT_EQ(pair.stack().transmit(0, burst), 1U);
    };
    const auto stack_receives = [&pair] {
        std::array<Packet, 4> burst{};
        const std::size_t got = pair.stack().receive(0, burst);
        std::vector<frames::ParsedFrame> parsed;
        for (Packet& packet : std::span{burst}.first(got)) {
            parsed.push_back(*frames::parse_frame(frames::bytes_of(packet)));
            packet = Packet{};
        }
        return parsed;
    };
    frames::TcpSpec from_stack{
        .destination_mac  = config.peer_mac,
        .source_mac       = pair.stack().mac(),
        .source           = config.stack_ip,
        .destination      = config.peer_ip,
        .source_port      = config.stack_port,
        .destination_port = config.peer_port,
        .sequence         = wire::TcpSequence{1000},
        .flags            = wire::TcpFlags{wire::TcpFlag::Syn, wire::TcpFlag::Ack},
        .mss              = 1460
    };
    // The handshake, the peer opening: it synchronises the script's view of the stack's sequence numbers.
    ASSERT_TRUE(peer.open_active().has_value());
    const auto syn = stack_receives();
    ASSERT_EQ(syn.size(), 1U);
    from_stack.acknowledgement = syn[0].tcp->sequence + 1;
    stack_sends(frames::tcp_frame(from_stack, {}));
    ASSERT_TRUE(peer.finish_active_open().has_value());
    ASSERT_EQ(stack_receives().size(), 1U) << "the handshake's ACK";
    from_stack.sequence        = peer.script().rcv_nxt();
    from_stack.acknowledgement = peer.script().snd_nxt();
    from_stack.flags           = wire::TcpFlags{wire::TcpFlag::Ack};
    from_stack.mss             = std::nullopt;

    // A pure ACK first: the closed-loop tick round produces none, but the peer skips one as a defence.
    stack_sends(frames::tcp_frame(from_stack, {}));
    frames::TcpSpec order = from_stack;
    order.flags           = wire::TcpFlags{wire::TcpFlag::Psh, wire::TcpFlag::Ack};
    std::vector<std::byte> bytes(order_size);
    write_order(bytes, 3);
    stack_sends(frames::tcp_frame(order, bytes));
    EXPECT_TRUE(peer.await_order(3).has_value());
    EXPECT_EQ(peer.counters().pure_acks_seen, 1U);
    EXPECT_EQ(peer.counters().orders_seen, 1U);
    EXPECT_EQ(peer.counters().surplus_freed, 1U) << "the pure ACK's mbuf";

    order.sequence = order.sequence + static_cast<std::uint32_t>(order_size);
    write_order(bytes, 9);
    stack_sends(frames::tcp_frame(order, bytes));
    const auto wrong = peer.await_order(4);
    ASSERT_FALSE(wrong.has_value());
    EXPECT_EQ(wrong.error(), PeerError::WrongRound);

    order.sequence  = order.sequence + static_cast<std::uint32_t>(order_size);
    order.checksums = frames::Checksums::Wrong;
    write_order(bytes, 5);
    stack_sends(frames::tcp_frame(order, bytes));
    const auto bad = peer.await_order(5);
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error(), PeerError::BadChecksum);

    PeerConfig impatient = config;
    impatient.deadline   = 20ms;
    VenuePeer waiting{pair.peer(), impatient, starts};
    const auto timeout = waiting.await_order(0);
    ASSERT_FALSE(timeout.has_value());
    EXPECT_EQ(timeout.error(), PeerError::Timeout);
}

TEST(VenuePeer, AnswersArpRequestsWhileWaiting) {
    RingPair pair{
        {.name = "venue-arp", .pool_size = 256}
    };
    std::array<std::uint64_t, 1> starts{};
    PeerConfig config = floor_peer(pair);
    config.deadline   = 50ms;
    VenuePeer peer{pair.peer(), config, starts};
    const auto request = frames::arp_frame(frames::arp_request(pair.stack().mac(), config.stack_ip, config.peer_ip),
                                           wire::MacAddress::broadcast());
    auto packet        = pair.stack().allocate(0);
    ASSERT_TRUE(packet.has_value());
    ASSERT_TRUE(frames::fill(*packet, request));
    std::array<Packet, 1> burst{std::move(*packet)};
    ASSERT_EQ(pair.stack().transmit(0, burst), 1U);
    const auto waited = peer.await_order(0);
    ASSERT_FALSE(waited.has_value());
    EXPECT_EQ(waited.error(), PeerError::Timeout) << "no order, but the request was answered meanwhile";
    EXPECT_EQ(peer.counters().arp_replies_sent, 1U);
    std::array<Packet, 4> at_stack{};
    ASSERT_EQ(pair.stack().receive(0, at_stack), 1U);
    const auto reply = frames::parse_frame(frames::bytes_of(at_stack[0]));
    ASSERT_TRUE(reply.has_value() && reply->arp.has_value());
    EXPECT_EQ(reply->arp->operation, wire::ArpOperation::Reply);
    EXPECT_EQ(reply->arp->sender_mac, config.peer_mac);
    EXPECT_EQ(reply->arp->sender_ip, config.peer_ip);
}

TEST(VenuePeer, AnswersAPassiveOpenInTheSynsOwnMbuf) {
    RingPair pair{
        {.name = "venue-passive", .pool_size = 256}
    };
    std::array<std::uint64_t, 1> starts{};
    const PeerConfig config = floor_peer(pair);
    VenuePeer peer{pair.peer(), config, starts};
    frames::TcpSpec from_stack{.destination_mac  = config.peer_mac,
                               .source_mac       = pair.stack().mac(),
                               .source           = config.stack_ip,
                               .destination      = config.peer_ip,
                               .source_port      = 50000,
                               .destination_port = config.peer_port,
                               .sequence         = wire::TcpSequence{5000},
                               .flags            = wire::TcpFlags{wire::TcpFlag::Syn},
                               .mss              = 1460};
    auto syn = pair.stack().allocate(0);
    ASSERT_TRUE(syn.has_value());
    ASSERT_TRUE(frames::fill(*syn, frames::tcp_frame(from_stack, {})));
    const auto* const syn_mbuf = syn->get();
    std::array<Packet, 1> out{std::move(*syn)};
    ASSERT_EQ(pair.stack().transmit(0, out), 1U);

    ASSERT_TRUE(peer.answer_passive_open().has_value());
    std::array<Packet, 4> at_stack{};
    ASSERT_EQ(pair.stack().receive(0, at_stack), 1U);
    EXPECT_EQ(at_stack[0].get(), syn_mbuf) << "the SYN-ACK rides in the SYN's own mbuf, back to the stack's pool";
    const auto syn_ack = frames::parse_frame(frames::bytes_of(at_stack[0]));
    ASSERT_TRUE(syn_ack.has_value() && syn_ack->tcp.has_value());
    EXPECT_TRUE(syn_ack->tcp->flags.has(wire::TcpFlag::Syn) && syn_ack->tcp->flags.has(wire::TcpFlag::Ack));
    EXPECT_EQ(syn_ack->tcp->acknowledgement, wire::TcpSequence{5001});

    from_stack.sequence        = wire::TcpSequence{5001};
    from_stack.acknowledgement = syn_ack->tcp->sequence + 1;
    from_stack.flags           = wire::TcpFlags{wire::TcpFlag::Ack};
    from_stack.mss             = std::nullopt;
    auto ack                   = pair.stack().allocate(0);
    ASSERT_TRUE(ack.has_value());
    ASSERT_TRUE(frames::fill(*ack, frames::tcp_frame(from_stack, {})));
    out[0] = std::move(*ack);
    ASSERT_EQ(pair.stack().transmit(0, out), 1U);
    ASSERT_TRUE(peer.finish_passive_open().has_value());
    EXPECT_EQ(peer.counters().surplus_freed, 1U) << "only the handshake's ACK, freed on the peer's core";
}
