#include <aloe/ethdev>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>

#include <eal_environment.hpp>
#include <gtest/gtest.h>
#include <level_bricks.hpp>
#include <level_runtime.hpp>
#include <ring_pair.hpp>
#include <run_state.hpp>
#include <stamped_device.hpp>
#include <venue_peer.hpp>

// Level 0 and level 1 against the scripted venue over the ring pair: the handshake both ways, closed-loop
// rounds, the recycled mbufs, the counters at zero. Same-core inline, cross-core on an unpinned thread. No root.
namespace {

    using namespace aloe::benchmarks;
    using namespace std::chrono_literals;
    using Stamped = StampedDevice<aloe::ethdev::Port>;

    const auto* const environment = ::testing::AddGlobalTestEnvironment(new aloe::testing::EalEnvironment{"net_null0"});

    [[nodiscard]] RoundConfig small(const Round round,
                                    const Level level,
                                    const Topology topology,
                                    const std::size_t payload,
                                    const bool breakdown = false) {
        return {.round     = round,
                .level     = level,
                .topology  = topology,
                .payload   = payload,
                .rounds    = 40,
                .warmup    = 4,
                .breakdown = breakdown};
    }

    [[nodiscard]] PeerConfig strict_peer(RingPair& pair, const aloe::core::Duration deadline = 2s) {
        return {.stack_mac = pair.stack().mac(), .deadline = deadline};
    }

    void expect_closed_loop(const RunState& state, const Stamped& device, const VenuePeer& peer) {
        EXPECT_FALSE(state.failed.load()) << state.failure;
        EXPECT_EQ(device.stamps_taken(), state.config.total());
        EXPECT_EQ(peer.counters().orders_seen, state.config.total());
        for (std::size_t round = 0; round < state.config.total(); ++round) {
            EXPECT_NE(state.stamps.start[round], 0U) << round;
            EXPECT_GT(state.stamps.stop[round], state.stamps.start[round]) << round;
        }
    }

    /// Runs level 0 over a fresh pair: inline peer for same-core, a thread otherwise.
    void run_level0(const RoundConfig& config,
                    const char* name,
                    const bool arp_on_wire              = false,
                    const aloe::core::Duration deadline = 2s) {
        RingPair pair{
            {.name = name, .pool_size = 512}
        };
        RunState state{config};
        Stamped device{pair.stack(), state.stamps.stop};
        const PeerConfig peer_config = strict_peer(pair, deadline);
        VenuePeer peer{pair.peer(), peer_config, state.stamps.start};
        if (config.topology == Topology::SameCore) {
            run_bricks(state, device, peer_config, arp_on_wire, &peer);
        } else {
            std::jthread venue{[&peer, &state] { drive_peer(peer, state, true); }};
            run_bricks(state, device, peer_config, arp_on_wire, nullptr);
            venue.join();
        }
        expect_closed_loop(state, device, peer);
        EXPECT_EQ(peer.counters().arp_replies_sent > 0, arp_on_wire) << "learned unless ARP is on the wire";
        // Every order carries the ACK of its tick, and the brick's count of full segments restarts at each ACK it
        // sends, so with one tick in flight no second-segment ACK leaves, at 1460 either.
        EXPECT_EQ(peer.counters().pure_acks_seen, 0U);
        if (config.gap > aloe::core::Duration::zero()) {
            const std::uint64_t gap = ticks_for(config.gap);
            for (std::size_t round = 1; round < state.config.total(); ++round) {
                EXPECT_GT(state.stamps.start[round], state.stamps.stop[round - 1]) << round;  // no underflow below
                EXPECT_GE(state.stamps.start[round] - state.stamps.stop[round - 1], gap) << round;
            }
        }
        if (config.breakdown) {
            for (std::size_t round = 0; round < state.config.total(); ++round) {
                std::uint64_t previous = state.stamps.start[round];
                for (std::size_t index = 0; index < phase_count; ++index) {
                    const auto phase          = static_cast<Phase>(index);
                    const std::uint64_t stamp = state.stamps.phases[index][round];
                    if (config.round == Round::Gateway && phase != Phase::Application && phase != Phase::TcpFlush) {
                        EXPECT_EQ(stamp, 0U) << to_string(phase) << ", round " << round << ": gateway, no receive";
                        continue;
                    }
                    EXPECT_GE(stamp, previous) << to_string(phase) << ", round " << round;
                    previous = stamp;
                }
                EXPECT_GE(state.stamps.stop[round], previous) << round;
            }
        }
    }

}  // namespace

TEST(Levels, BricksTickSameCore) {
    run_level0(small(Round::Tick, Level::Bricks, Topology::SameCore, 64, true), "lv0-tick-same");
}

TEST(Levels, BricksTickSameCoreFullSegment) {
    run_level0(small(Round::Tick, Level::Bricks, Topology::SameCore, 1460), "lv0-tick-1460");
}

TEST(Levels, BricksTickSameCoreHonoursTheGap) {
    RoundConfig config = small(Round::Tick, Level::Bricks, Topology::SameCore, 64);
    config.rounds      = 5;
    config.warmup      = 0;
    config.gap         = 1ms;
    run_level0(config, "lv0-gap");
}

TEST(Levels, BricksTickCrossCore) {
    run_level0(small(Round::Tick, Level::Bricks, Topology::CrossCore, 256), "lv0-tick-cross");
}

TEST(Levels, BricksTickCrossCoreGapOutlastsTheDeadline) {
    RoundConfig config = small(Round::Tick, Level::Bricks, Topology::CrossCore, 64);
    config.rounds      = 3;
    config.warmup      = 0;
    config.gap         = 150ms;
    run_level0(config, "lv0-gap-dl", false, 100ms);  // the gap is silence by design, not a lost peer
}

TEST(Levels, BricksTickWithArpOnTheWire) {
    run_level0(small(Round::Tick, Level::Bricks, Topology::CrossCore, 64), "lv0-tick-arp", true);
}

TEST(Levels, BricksGatewaySameCore) {
    run_level0(small(Round::Gateway, Level::Bricks, Topology::SameCore, order_size, true), "lv0-gw-same");
}

TEST(Levels, BricksGatewaySameCoreHonoursTheGap) {
    RoundConfig config = small(Round::Gateway, Level::Bricks, Topology::SameCore, order_size);
    config.rounds      = 5;
    config.warmup      = 0;
    config.gap         = 1ms;
    run_level0(config, "lv0-gw-gap");
}

TEST(Levels, BricksGatewayCrossCore) {
    run_level0(small(Round::Gateway, Level::Bricks, Topology::CrossCore, order_size), "lv0-gw-cross");
}

namespace {

    /// Runs level 1 over a fresh pair with the peer on an unpinned thread; the shard thread is unpinned too.
    void run_level1(const RoundConfig& config, const char* name, const bool arp_on_wire = false) {
        RingPair pair{
            {.name = name, .pool_size = 512}
        };
        RunState state{config};
        Stamped device{pair.stack(), state.stamps.stop};
        const PeerConfig peer_config = strict_peer(pair);
        VenuePeer peer{pair.peer(), peer_config, state.stamps.start};
        std::jthread venue{[&peer, &state] { drive_peer(peer, state, true); }};
        run_runtime(state, device, peer_config, arp_on_wire, std::nullopt);
        venue.join();
        expect_closed_loop(state, device, peer);
        EXPECT_EQ(peer.counters().arp_replies_sent > 0, arp_on_wire) << "learned unless ARP is on the wire";
        // As at level 0: no pure ACK follows the handshake, since every order carries the ACK of its tick and a bare
        // ACK from the venue gets none back.
        EXPECT_EQ(peer.counters().pure_acks_seen, 0U);
    }

}  // namespace

TEST(Levels, RuntimeTickCrossCore) {
    run_level1(small(Round::Tick, Level::Runtime, Topology::CrossCore, 64), "lv1-tick");
}

TEST(Levels, RuntimeTickFullSegment) {
    run_level1(small(Round::Tick, Level::Runtime, Topology::CrossCore, 1460), "lv1-tick-1460");
}

TEST(Levels, RuntimeTickWithArpOnTheWire) {
    run_level1(small(Round::Tick, Level::Runtime, Topology::CrossCore, 64), "lv1-tick-arp", true);
}

TEST(Levels, RuntimeGatewayCrossCore) {
    run_level1(small(Round::Gateway, Level::Runtime, Topology::CrossCore, order_size), "lv1-gw");
}
