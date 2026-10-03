#pragma once

#include <aloe/core>
#include <aloe/utils>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <device.hpp>
#include <pthread.h>
#include <sched.h>
#include <scheduler.hpp>
#include <shard.hpp>
#include <work.hpp>

namespace aloe::runtime {

    /// Thrown by construction and `start`, never on the hot path.
    class RuntimeError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    struct ShardThread {
        std::optional<unsigned> cpu = std::nullopt;  ///< Pin the shard thread here; none means unpinned.
        std::string name{};                          ///< Thread name; empty means `aloe-shard-N`.
    };

    /// Every member has a default, so a designated initialiser may name only what it changes.
    struct RuntimeConfig {
        ShardConfig shard{};
        std::vector<ShardThread> threads{};   ///< One per queue, or empty for unpinned defaults.
        std::function<void()> thread_hook{};  ///< Runs first on every shard thread; `ethdev::register_thread` for DPDK.
    };

    /**
     * @brief N shards over an N-queue device, one thread each.
     *
     * `start` launches the threads and returns once every one has run its hook; a hook that throws
     * or a CPU that cannot be pinned stops what already started and throws. `stop` posts a stop into
     * every inbox, from any thread, idempotent; shards then drain and their threads end. `join`
     * waits. The destructor does both. `scheduler(i)` works from any thread, `spawn(i, sender)` hops
     * to shard i and spawns there. `shard(i)` and `counters(i)` are for the shard's own thread or
     * for after `join`.
     */
    template <device::IsDevice Device, typename Stack>
        requires IsStack<Stack, Device>
    class Runtime {
    public:
        using ShardType = Shard<Device, Stack>;

        /// One shard per device queue. `stack_args` are copied into every stack.
        template <typename... Args>
        Runtime(const RuntimeConfig& config, Device& device, const Args&... stack_args)
            : config_{config} {
            const std::uint16_t queues = device.queue_count();
            if (queues == 0) {
                throw RuntimeError{"a runtime needs a device with at least one queue"};
            }
            if (!config.threads.empty() && config.threads.size() != queues) {
                throw RuntimeError{"one ShardThread per queue, or none"};
            }
            const ShardContext::TimePoint start = ShardContext::Clock::now();
            shards_.reserve(queues);
            stops_.reserve(queues);
            for (std::uint16_t queue = 0; queue < queues; ++queue) {
                shards_.push_back(std::make_unique<ShardType>(config.shard, device, queue, start, stack_args...));
                stops_.push_back(std::make_unique<StopWork>(shards_.back()->context()));
            }
        }

        Runtime(const Runtime&)            = delete;
        Runtime& operator=(const Runtime&) = delete;
        Runtime(Runtime&&)                 = delete;
        Runtime& operator=(Runtime&&)      = delete;

        ~Runtime() {
            stop();
            join();
        }

        void start() {
            if (started_) {
                throw RuntimeError{"a runtime starts once"};
            }
            started_ = true;
            std::vector<std::future<void>> ready;
            ready.reserve(shards_.size());
            threads_.reserve(shards_.size());
            for (std::uint16_t index = 0; index < shard_count(); ++index) {
                std::promise<void> promise;
                ready.push_back(promise.get_future());
                auto body = [this, index, promise = std::move(promise)]() mutable {
                    try {
                        prepare_thread(index);
                        promise.set_value();
                    } catch (...) {
                        promise.set_exception(std::current_exception());
                        return;
                    }
                    shards_[index]->run();
                    const ShardCounters& counters = shards_[index]->context().counters();
                    shards_[index]
                        ->context()
                        .logger()
                        .template info<
                            "shard {} drained after {} ticks: {} frames in, {} out, {} tasks ran, {} failed">(
                            index,
                            counters.ticks,
                            counters.frames_received,
                            counters.frames_transmitted,
                            counters.tasks_completed + counters.tasks_stopped,
                            counters.tasks_failed);
                };
                try {
                    threads_.emplace_back(std::move(body));
                } catch (const std::system_error& error) {
                    // Spec, "Error handling": a thread that fails to start is a RuntimeError, like every other start
                    // failure.
                    stop();
                    join();
                    throw RuntimeError{"shard " + std::to_string(index) +
                                       ": thread could not be created: " + error.what()};
                }
            }
            std::exception_ptr first_failure;
            for (std::future<void>& future : ready) {
                try {
                    future.get();
                } catch (...) {
                    if (!first_failure) {
                        first_failure = std::current_exception();
                    }
                }
            }
            if (first_failure) {
                stop();
                join();
                try {
                    std::rethrow_exception(first_failure);
                } catch (const RuntimeError& error) {
                    log().template error<"a shard thread failed to start: {}">(error.what());
                    throw;
                } catch (const std::exception& exception) {
                    log().template error<"a shard thread failed to start: {}">(exception.what());
                    throw RuntimeError{std::string{"a shard thread failed to start: "} + exception.what()};
                }
            }
        }

        /// Any thread. Idempotent.
        void stop() noexcept {
            if (stopped_.exchange(true)) {
                return;
            }
            log().template info<"stop requested for {} shards">(shards_.size());
            for (std::unique_ptr<StopWork>& pending : stops_) {  // not `stop`: GCC's -Wshadow sees the member function
                pending->context->inbox().push(*pending);
            }
        }

        void join() {
            for (std::jthread& thread : threads_) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
            joined_ = !threads_.empty();
        }

        [[nodiscard]] Scheduler scheduler(const std::uint16_t index) const noexcept {
            return Scheduler{shards_[index]->context()};
        }

        /// Any thread. Posts the sender to shard `index`, which spawns it into its scope.
        template <core::ex::sender Sender>
        void spawn(const std::uint16_t index, Sender&& sender) {
            utils::force_non_const(this);
            assert(!joined_ && "spawn after join: nothing reads the inbox any more and the work would leak");
            using Plain = std::remove_cvref_t<Sender>;
            auto* work  = new SpawnWork<Plain>{shards_[index]->context(), Plain{std::forward<Sender>(sender)}};
            shards_[index]->context().inbox().push(*work);
        }

        [[nodiscard]] ShardType& shard(const std::uint16_t index) noexcept {
            utils::force_non_const(this);
            return *shards_[index];
        }

        [[nodiscard]] const ShardCounters& counters(const std::uint16_t index) const noexcept {
            return shards_[index]->context().counters();
        }

        [[nodiscard]] std::uint16_t shard_count() const noexcept {
            return static_cast<std::uint16_t>(shards_.size());
        }

    private:
        struct StopWork : Work {
            explicit StopWork(ShardContext& owner) noexcept
                : Work{&StopWork::execute},
                  context{&owner} {
            }

            static void execute(Work& work) noexcept {
                static_cast<StopWork&>(work).context->request_stop();
            }

            ShardContext* context;
        };

        template <typename Sender>
        struct SpawnWork : Work {
            SpawnWork(ShardContext& owner, Sender s)
                : Work{&SpawnWork::execute},
                  context{&owner},
                  sender{std::move(s)} {
            }

            static void execute(Work& work) noexcept {
                auto* self = static_cast<SpawnWork*>(&work);
                Scheduler{*self->context}.spawn(std::move(self->sender));
                delete self;
            }

            ShardContext* context;
            Sender sender;
        };

        void prepare_thread(const std::uint16_t index) {
            const ShardThread options = config_.threads.empty() ? ShardThread{} : config_.threads[index];
            const std::string name    = options.name.empty() ? "aloe-shard-" + std::to_string(index) : options.name;
            std::ignore               = pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
            if (options.cpu) {
                if (*options.cpu >= CPU_SETSIZE) {
                    throw RuntimeError{"shard " + std::to_string(index) + ": cpu " + std::to_string(*options.cpu) +
                                       " is out of range"};
                }
                cpu_set_t set;
                CPU_ZERO(&set);
                CPU_SET(*options.cpu, &set);
                if (const int error = pthread_setaffinity_np(pthread_self(), sizeof(set), &set); error != 0) {
                    throw RuntimeError{"shard " + std::to_string(index) + ": cannot pin to cpu " +
                                       std::to_string(*options.cpu) + ": " + std::strerror(error)};
                }
            }
            if (config_.thread_hook) {
                config_.thread_hook();
            }
            shards_[index]->context().logger().template info<"shard {} starting as {} on cpu {}">(
                index, name, options.cpu ? static_cast<int>(*options.cpu) : -1);
        }

        /// The runtime's cold-path logger: the first shard's, which is the module's.
        [[nodiscard]] core::Logger log() const noexcept {
            return shards_.front()->context().logger();
        }

        RuntimeConfig config_;
        std::vector<std::unique_ptr<ShardType>> shards_;
        std::vector<std::unique_ptr<StopWork>> stops_;
        std::vector<std::jthread> threads_;
        std::atomic<bool> stopped_{false};
        bool started_ = false;
        bool joined_  = false;
    };

}  // namespace aloe::runtime
