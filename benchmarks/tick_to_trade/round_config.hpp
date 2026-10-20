#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <clock.hpp>

namespace aloe::benchmarks {

    /// The two critical paths the benchmark measures.
    enum class Round : std::uint8_t {
        Tick,     ///< Tick-to-trade: a market-data segment in, an order out.
        Gateway,  ///< The gateway round: an order composed in place, out to the device.
    };

    /// What stands between the device and the application.
    enum class Level : std::uint8_t {
        Floor,    ///< Nothing: raw frames in, a raw frame out.
        Bricks,   ///< Level 0: the hand-written loop over the bricks.
        Runtime,  ///< Level 1: runtime tasks over the same bricks.
    };

    enum class Topology : std::uint8_t {
        CrossCore,  ///< The peer on its own core: the default.
        SameCore,   ///< Peer and stack strictly sequential on one thread: the floor and level 0 only.
    };

    inline constexpr std::size_t order_size   = 128;  ///< Bytes of every order.
    inline constexpr std::size_t round_prefix = 8;    ///< The round number, little-endian, opens every tick and order.

    /// One configuration of the matrix: everything a level function needs to run its rounds.
    struct RoundConfig {
        Round round         = Round::Tick;
        Level level         = Level::Bricks;
        Topology topology   = Topology::CrossCore;
        std::size_t payload = 64;  ///< Tick payload in bytes, at least `round_prefix`; the gateway round ignores it.
        std::size_t rounds  = 100000;                  ///< Measured rounds.
        std::size_t warmup  = 10000;                   ///< Rounds before them, recorded and discarded.
        core::Duration gap  = core::Duration::zero();  ///< The pause between rounds.
        bool breakdown      = false;                   ///< Phase stamps, at level 0 only.

        [[nodiscard]] std::size_t total() const noexcept {
            return warmup + rounds;
        }
    };

    [[nodiscard]] constexpr std::string_view to_string(const Round round) noexcept {
        switch (round) {
            case Round::Tick:
                return "tick";
            case Round::Gateway:
                return "gateway";
        }
        return "?";
    }

    [[nodiscard]] constexpr std::string_view to_string(const Level level) noexcept {
        switch (level) {
            case Level::Floor:
                return "floor";
            case Level::Bricks:
                return "level0";
            case Level::Runtime:
                return "level1";
        }
        return "?";
    }

    [[nodiscard]] constexpr std::string_view to_string(const Topology topology) noexcept {
        switch (topology) {
            case Topology::CrossCore:
                return "cross-core";
            case Topology::SameCore:
                return "same-core";
        }
        return "?";
    }

}  // namespace aloe::benchmarks
