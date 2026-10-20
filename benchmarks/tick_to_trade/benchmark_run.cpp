#include "benchmark_run.hpp"

#include <algorithm>
#include <aloe/ethdev>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <clock.hpp>
#include <distribution.hpp>
#include <hop.hpp>
#include <level_bricks.hpp>
#include <level_floor.hpp>
#include <level_runtime.hpp>
#include <ring_pair.hpp>
#include <stamped_device.hpp>
#include <thread_pinning.hpp>
#include <tsc_clock.hpp>
#include <venue_peer.hpp>

namespace aloe::benchmarks {

    namespace {

        /**
         * @brief The peer's thread for the whole run, so DPDK registers one peer thread.
         *
         * Pinned to the peer core, or widened to every CPU when there is none, since it would otherwise inherit the
         * main thread's mask and share the stack's core; then registered with DPDK, then running one job at a time.
         */
        class PeerWorker {
        public:
            /// Starts the thread and waits until it is placed and registered; throws what refused either.
            explicit PeerWorker(const std::optional<unsigned> peer_core)
                : thread_{[this, peer_core] { serve(peer_core); }} {
                std::unique_lock lock{mutex_};
                changed_.wait(lock, [this] { return ready_ || refusal_ != nullptr; });
                if (refusal_ != nullptr) {
                    std::rethrow_exception(refusal_);  // the thread has returned; the member joins it
                }
            }

            PeerWorker(const PeerWorker&)            = delete;
            PeerWorker& operator=(const PeerWorker&) = delete;
            PeerWorker(PeerWorker&&)                 = delete;
            PeerWorker& operator=(PeerWorker&&)      = delete;

            /// Posts a stop; the thread finishes the job it has, if any, and the member joins it.
            ~PeerWorker() {
                {
                    const std::scoped_lock lock{mutex_};
                    stopping_ = true;
                }
                changed_.notify_all();
            }

            /// Hands `job` over and returns; the previous job has finished.
            void run(std::function<void()> job) {
                {
                    const std::scoped_lock lock{mutex_};
                    assert(!busy_ && "a peer job is still running");
                    job_  = std::move(job);
                    busy_ = true;
                }
                changed_.notify_all();
            }

            /// Blocks until the job has finished; rethrows what it threw.
            void wait() {
                std::unique_lock lock{mutex_};
                changed_.wait(lock, [this] { return !busy_; });
                if (failure_ != nullptr) {
                    std::rethrow_exception(std::exchange(failure_, nullptr));
                }
            }

        private:
            void serve(const std::optional<unsigned> peer_core) {
                try {
                    if (peer_core) {
                        pin_current_thread(*peer_core);
                    } else {
                        widen_to_all_cpus();
                    }
                    ethdev::register_thread();
                } catch (...) {
                    const std::scoped_lock lock{mutex_};
                    refusal_ = std::current_exception();
                    changed_.notify_all();
                    return;
                }
                std::unique_lock lock{mutex_};
                ready_ = true;
                changed_.notify_all();
                while (true) {
                    changed_.wait(lock, [this] { return busy_ || stopping_; });
                    if (!busy_) {
                        return;
                    }
                    const std::function<void()> job = std::exchange(job_, nullptr);
                    lock.unlock();
                    std::exception_ptr failure;
                    try {
                        job();
                    } catch (...) {
                        failure = std::current_exception();
                    }
                    lock.lock();
                    failure_ = failure;
                    busy_    = false;
                    changed_.notify_all();
                }
            }

            std::mutex mutex_;
            std::condition_variable changed_;
            std::function<void()> job_;
            std::exception_ptr refusal_;  ///< What refused the pinning or the registration.
            std::exception_ptr failure_;  ///< What the last job threw.
            bool ready_    = false;
            bool busy_     = false;
            bool stopping_ = false;
            std::jthread thread_;  ///< Last: it starts once every member it reads is constructed, and joins first.
        };

        /// Follows a refusal of a pair stamped on two cores: the tick round's start, the peer's, against the stack's.
        constexpr std::string_view tsc_note =
            "; across cores this means the TSC is not synchronised between the stack's and the peer's core";

        /// The ticks from `begins` to `ends` in every round. Throws std::runtime_error naming the round and the leg
        /// when an end stamp is missing, or when it does not follow its beginning, then with `note` appended.
        [[nodiscard]] Distribution leg_ticks(const std::span<const std::uint64_t> begins,
                                             const std::span<const std::uint64_t> ends,
                                             const std::string_view leg,
                                             const std::string_view note) {
            Distribution ticks{ends.size()};
            for (std::size_t round = 0; round < ends.size(); ++round) {
                if (ends[round] == 0) {
                    throw std::runtime_error{std::format("round {} has no {} stamp", round, leg)};
                }
                if (ends[round] <= begins[round]) {
                    throw std::runtime_error{
                        std::format("round {}: the {} leg ends before it begins{}", round, leg, note)};
                }
                ticks.record(ends[round] - begins[round]);
            }
            return ticks;
        }

        [[nodiscard]] std::string core_text(const std::optional<unsigned> core) {
            return core ? std::to_string(*core) : std::string{"none"};
        }

        /// The configuration as every failure names it: "tick level0 cross-core payload 64".
        [[nodiscard]] std::string describe(const RoundConfig& config) {
            return std::format("{} {} {} payload {}",
                               to_string(config.round),
                               to_string(config.level),
                               to_string(config.topology),
                               config.payload);
        }

        [[nodiscard]] RunInfo run_info(const Options& options, const core::TscScale& scale, std::string command) {
            constexpr std::uint64_t probe = std::uint64_t{1} << 32U;
            const auto now                = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
            return RunInfo{.command    = std::move(command),
                           .timestamp  = std::format("{:%FT%TZ}", now),
                           .tsc_ghz    = static_cast<double>(probe) / static_cast<double>(scale.nanoseconds(probe)),
                           .rounds     = options.measured,
                           .warmup     = options.warmup,
                           .gap_ns     = static_cast<std::uint64_t>(options.gap.count()),
                           .stack_core = core_text(options.stack_core),
                           .peer_core  = core_text(options.peer_core),
                           .no_huge    = options.no_huge};
        }

    }  // namespace

    std::vector<Row> summarise_round(const RunState& state, const core::TscScale& scale, const RoundConfig& config) {
        const std::size_t total = config.total();
        assert(state.stamps.start.size() == total && state.stamps.stop.size() == total &&
               "the stamps were sized for another configuration");
        // Only the tick round's start is stamped off the stack's core: by the peer, on its own core when cross-core.
        const std::string_view cross_core_note =
            config.round == Round::Tick && config.topology == Topology::CrossCore ? tsc_note : "";
        Distribution round_trip{total};
        for (std::size_t round = 0; round < total; ++round) {
            const std::uint64_t start = state.stamps.start[round];
            const std::uint64_t stop  = state.stamps.stop[round];
            if (start == 0 || stop == 0) {
                throw std::runtime_error{std::format("round {} has no {} stamp", round, start == 0 ? "start" : "stop")};
            }
            if (stop <= start) {
                throw std::runtime_error{
                    std::format("round {}: the stop stamp precedes the start{}", round, cross_core_note)};
            }
            round_trip.record(stop - start);
        }
        const auto label = [&config](std::string metric, const Summary summary) {
            return Row{.round    = std::string{to_string(config.round)},
                       .level    = std::string{to_string(config.level)},
                       .topology = std::string{to_string(config.topology)},
                       .payload  = config.payload,
                       .metric   = std::move(metric),
                       .summary  = summary};
        };
        std::vector<Row> rows;
        rows.push_back(label("round", round_trip.summarise(scale, config.warmup)));
        if (!config.breakdown) {
            return rows;
        }
        // The legs between the stamps that are present: start, each phase with any nonzero entry, stop.
        std::vector<std::pair<std::string, const std::vector<std::uint64_t>*>> present;
        for (std::size_t index = 0; index < phase_count; ++index) {
            const auto& phase = state.stamps.phases[index];
            if (std::ranges::any_of(phase, [](const std::uint64_t t) { return t != 0; })) {
                present.emplace_back(std::string{to_string(static_cast<Phase>(index))}, &phase);
            }
        }
        // Every leg is checked as the round is; only the one that starts at the start can cross cores.
        const std::vector<std::uint64_t>* previous = &state.stamps.start;
        const auto note_after = [&state, cross_core_note](const std::vector<std::uint64_t>* const begins) {
            return begins == &state.stamps.start ? cross_core_note : std::string_view{};
        };
        for (const auto& [name, stamps] : present) {
            const Distribution leg = leg_ticks(*previous, *stamps, name, note_after(previous));
            rows.push_back(label(name, leg.summarise(scale, config.warmup)));
            previous = stamps;
        }
        const Distribution last = leg_ticks(*previous, state.stamps.stop, last_leg, note_after(previous));
        rows.push_back(label(std::string{last_leg}, last.summarise(scale, config.warmup)));
        return rows;
    }

    Report run_benchmark(const Options& options, const MachineInfo& machine, std::string command) {
        // The floor, level 0 and the hop's near end run on this thread, which the EAL placed on the main lcore of
        // `-l`; the result file records `--stack-core`, so the thread moves there. A mempool cache belongs to an
        // lcore id, not to a CPU, so the move leaves the thread's cache as it was.
        if (options.stack_core) {
            pin_current_thread(*options.stack_core);
        }
        core::calibrate_clock();
        const core::TscScale& scale = core::TscClock::scale();
        RingPair pair{
            {.name = options.ring_name, .pool_size = 8192}
        };
        // In the gateway round the peer awaits the next order through the stack's gap.
        const PeerConfig peer_config{.stack_mac = pair.stack().mac(),
                                     .deadline  = std::max<core::Duration>(std::chrono::seconds{1}, 2 * options.gap)};
        PeerWorker worker{options.peer_core};
        Report report{.machine = machine, .run = run_info(options, scale, std::move(command)), .rows = {}};
        for (const RoundConfig& config : configurations(options)) {
            RunState state{config};
            StampedDevice<ethdev::Port> device{pair.stack(), state.stamps.stop};
            PeerConfig this_peer = peer_config;
            this_peer.strict     = config.level != Level::Floor;
            VenuePeer peer{pair.peer(), this_peer, state.stamps.start};
            const bool handshake = config.level != Level::Floor;
            const auto run_level = [&config, &state, &device, &this_peer, &options](VenuePeer* const inline_peer) {
                switch (config.level) {
                    case Level::Floor:
                        run_floor(state, device, this_peer, inline_peer);
                        break;
                    case Level::Bricks:
                        run_bricks(state, device, this_peer, options.arp_on_wire, inline_peer);
                        break;
                    case Level::Runtime:
                        run_runtime(state, device, this_peer, options.arp_on_wire, options.stack_core);
                        break;
                }
            };
            if (config.topology == Topology::SameCore) {
                run_level(&peer);
            } else {
                // The peer's job starts first: `drive_peer` waits in `collect()` for the stack's first frame.
                worker.run([&peer, &state, handshake] { drive_peer(peer, state, handshake); });
                try {
                    run_level(nullptr);
                } catch (...) {
                    state.fail("the stack's side threw");  // ends the peer's job before its state goes
                    try {
                        worker.wait();
                    } catch (...) {
                        // The stack's exception is the one the caller gets; the peer's would only follow from it.
                    }
                    throw;
                }
                worker.wait();
            }
            if (state.failed.load()) {
                throw std::runtime_error{std::format("{}: {}", describe(config), state.failure)};
            }
            std::vector<Row> rows;
            try {
                rows = summarise_round(state, scale, config);
            } catch (const std::runtime_error& error) {
                throw std::runtime_error{std::format("{}: {} (stack core {}, peer core {})",
                                                     describe(config),
                                                     error.what(),
                                                     core_text(options.stack_core),
                                                     core_text(options.peer_core))};
            }
            for (Row& row : rows) {
                report.rows.push_back(std::move(row));
            }
            // After the level returned and its bricks are gone: a level 1 stream's RST and a gateway round's last ACK
            // are still on the rings.
            pair.drain();
        }
        if (options.hop) {
            const Distribution hop = measure_hop(options.peer_core, options.warmup + options.measured);
            report.rows.push_back(Row{.round    = "hop",
                                      .level    = "-",
                                      .topology = "cross-core",
                                      .payload  = 0,
                                      .metric   = "hop",
                                      .summary  = hop.summarise(scale, options.warmup)});
        }
        return report;
    }

}  // namespace aloe::benchmarks
