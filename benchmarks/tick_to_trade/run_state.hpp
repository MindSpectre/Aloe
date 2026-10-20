#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <clock.hpp>
#include <round_config.hpp>
#include <tsc_clock.hpp>

namespace aloe::benchmarks {

    /// The phases of a level 0 round in breakdown mode, each stamped when it ends; the device's stop stamp ends the
    /// last leg, the queue flush.
    enum class Phase : std::uint8_t {
        Received,     ///< `queue.receive` returned the tick.
        Ip,           ///< `ip.process` done.
        Tcp,          ///< `tcp.process` done.
        Application,  ///< The order committed.
        TcpFlush,     ///< `tcp.flush` done; what remains is `queue.flush` and the device.
    };

    inline constexpr std::size_t phase_count = 5;

    [[nodiscard]] constexpr std::string_view to_string(const Phase phase) noexcept {
        switch (phase) {
            case Phase::Received:
                return "receive";
            case Phase::Ip:
                return "ip";
            case Phase::Tcp:
                return "tcp";
            case Phase::Application:
                return "application";
            case Phase::TcpFlush:
                return "tcp-flush";
        }
        return "?";
    }

    /// The name of the leg from the last phase stamp to the stop stamp.
    inline constexpr std::string_view last_leg = "queue-flush";

    /// `duration` in TSC ticks, by the calibrated scale: for gaps and deadlines, off the measured path.
    [[nodiscard]] inline std::uint64_t ticks_for(const core::Duration duration) {
        constexpr std::uint64_t probe         = std::uint64_t{1} << 32U;
        const std::uint64_t probe_nanoseconds = core::TscClock::scale().nanoseconds(probe);
        const auto nanoseconds                = static_cast<std::uint64_t>(duration.count());
        return static_cast<std::uint64_t>(static_cast<double>(nanoseconds) * static_cast<double>(probe) /
                                          static_cast<double>(probe_nanoseconds));
    }

    /// One configuration's stamps: raw ticks, one slot per round, each array written by exactly one thread.
    struct Stamps {
        Stamps(const std::size_t total, const bool breakdown)
            : start(total),
              stop(total) {
            if (breakdown) {
                for (std::vector<std::uint64_t>& phase : phases) {
                    phase.assign(total, 0);
                }
            }
        }

        std::vector<std::uint64_t> start;  ///< The peer (tick round) or the application (gateway round).
        std::vector<std::uint64_t> stop;   ///< The stamped device.
        std::array<std::vector<std::uint64_t>, phase_count> phases{};  ///< Level 0 breakdown; empty unless enabled.
    };

    /// Everything one configuration's threads share. Immovable; it lives where the configuration runs.
    struct RunState {
        explicit RunState(const RoundConfig& configuration)
            : config{configuration},
              stamps{configuration.total(), configuration.breakdown} {
        }

        RoundConfig config;
        Stamps stamps;
        std::atomic<bool> stop{false};  ///< Set by whoever finishes or fails first; every loop polls it.
        std::atomic<bool> failed{false};
        std::string failure;     ///< Written by the first to fail, before `failed`; read after the join.
        std::uint64_t sink = 0;  ///< The application's fold, kept so the work is not elided.

        /// Every round is out: stops the loops and wakes a thread sleeping on `stop`.
        void finish() noexcept {
            stop.store(true);
            stop.notify_all();
        }

        /// Records the first failure, then finishes.
        void fail(std::string why) noexcept {
            if (!failed.exchange(true)) {
                failure = std::move(why);
            }
            finish();
        }
    };

}  // namespace aloe::benchmarks
