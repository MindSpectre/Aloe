#pragma once

#include <aloe/core>
#include <aloe/log>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

#include <counters.hpp>
#include <inbox.hpp>
#include <ipv4_address.hpp>
#include <mac_address.hpp>
#include <run_queue.hpp>
#include <task_scope.hpp>
#include <timer_wheel.hpp>

namespace aloe::runtime {

    struct ShardContextConfig {
        std::uint16_t index             = 0;  ///< The shard's, and its queue's, index.
        core::Duration timer_resolution = std::chrono::milliseconds{1};
    };

    /**
     * @brief Everything of a shard that does not touch the device.
     *
     * Owns the run queue, the inbox, the timer wheel, the task scope, the tick stamp and the
     * counters. One thread owns a context for its whole life; every member function except
     * `loop::Inbox::push` and `post_control` runs on that thread. A thread-local pointer, `current()`, names
     * the context the calling thread is running, which is how the scheduler tells same-shard from cross-shard.
     *
     * `run_once(now)` is its one verb: record the stamp, move the inbox's contents onto the run
     * queue, advance the wheel, run the work that was queued when the step began. Work queued
     * while the chain runs waits for the next step. Stop is a flag plus the scope's stop request;
     * `drained()` observes whether anything is left, and `try_finish()` is the loop's exit: it closes
     * control admission atomically with a successful drain check.
     */
    class ShardContext {
    public:
        /// `start` is where the wheel's tick zero begins; the shard passes the clock, tests pass any stamp.
        ShardContext(const ShardContextConfig& config, core::TimePoint start);
        ShardContext(const ShardContext&)            = delete;
        ShardContext& operator=(const ShardContext&) = delete;
        ShardContext(ShardContext&&)                 = delete;
        ShardContext& operator=(ShardContext&&)      = delete;
        ~ShardContext()                              = default;

        /// The context the calling thread is running, or null.
        [[nodiscard]] static ShardContext* current() noexcept;

        /// Makes a context current on this thread for the object's lifetime; restores the previous one after.
        class Current {
        public:
            explicit Current(ShardContext& context) noexcept;
            Current(const Current&)            = delete;
            Current& operator=(const Current&) = delete;
            Current(Current&&)                 = delete;
            Current& operator=(Current&&)      = delete;
            ~Current();

        private:
            ShardContext* previous_;
        };

        /// A cold-path sink for ARP resolutions learned by another shard; the stack above registers it.
        struct ArpSink {
            void* object                                                                        = nullptr;
            void (*learn)(void*, wire::Ipv4Address, wire::MacAddress, core::TimePoint) noexcept = nullptr;
        };

        /// Records the step's stamp before any stack callback; runs nothing and advances no timer.
        void set_now(const core::TimePoint now) noexcept {
            now_ = now;
        }

        /// Every other context of the runtime, bound before any thread starts; empty for a standalone shard.
        [[nodiscard]] std::span<ShardContext* const> siblings() const noexcept {
            return siblings_;
        }

        /// Setup only, before `run`: copies the pointers.
        void set_siblings(std::span<ShardContext* const> siblings);

        /**
         * Any thread. Posts a cold-path work node that will run once on this shard, or returns false when
         * the shard has closed admission, in which case the node stays the caller's. The ordinary
         * `Inbox::push` keeps its contract for same-shard and scheduler work; this guarded path is for
         * messages between shards whose producer must know whether the node was taken.
         */
        [[nodiscard]] bool post_control(loop::Work& work) noexcept;

        /**
         * Shard thread. False while no stop is requested, without taking the lock. Otherwise rechecks
         * `drained()` under the control lock and, when it holds, closes admission atomically with the
         * check: no later `post_control` succeeds. `Shard::run` exits on true.
         */
        [[nodiscard]] bool try_finish() noexcept;

        /// Setup and teardown by the stack above. A default sink discards deliveries.
        void set_arp_sink(const ArpSink sink) noexcept {
            arp_sink_ = sink;
        }

        /// Shard thread: hands a resolution to the sink with the current stamp.
        void deliver_arp(wire::Ipv4Address address, wire::MacAddress mac) noexcept;

        /// One step. Returns whether anything ran or fired.
        bool run_once(core::TimePoint now) noexcept;

        /**
         * Shard thread: sets the stop flag and stops the scope. Idempotent.
         *
         * The runtime delivers it through the inbox, so it runs inside `run_once`'s `Current`. A test
         * that calls it directly on a context holding a parked timer sender must hold a
         * `ShardContext::Current`, because the timer senders' stop callback asserts the current context.
         */
        void request_stop() noexcept;

        [[nodiscard]] bool stop_requested() const noexcept {
            return stopping_;
        }

        /// Stop requested, scope empty, both queues empty. Armed timers do not count. Observes; closes nothing.
        [[nodiscard]] bool drained() const noexcept {
            return stopping_ && scope_.empty() && ready_.empty() && inbox_.empty();
        }

        /// The stamp of the current or last step, never a clock read.
        [[nodiscard]] core::TimePoint now() const noexcept {
            return now_;
        }

        [[nodiscard]] std::uint16_t index() const noexcept {
            return index_;
        }

        [[nodiscard]] log::Logger logger() const noexcept {
            return logger_;
        }

        [[nodiscard]] loop::RunQueue& ready() noexcept {
            return ready_;
        }

        [[nodiscard]] loop::Inbox& inbox() noexcept {
            return inbox_;
        }

        [[nodiscard]] loop::TimerWheel& timers() noexcept {
            return timers_;
        }

        [[nodiscard]] TaskScope& scope() noexcept {
            return scope_;
        }

        [[nodiscard]] loop::ShardCounters& counters() noexcept {
            return counters_;
        }

        [[nodiscard]] const loop::ShardCounters& counters() const noexcept {
            return counters_;
        }

    private:
        std::uint16_t index_;
        log::Logger logger_;
        loop::ShardCounters counters_;
        core::TimePoint now_;
        loop::RunQueue ready_;
        loop::Inbox inbox_;
        loop::TimerWheel timers_;
        TaskScope scope_;
        bool stopping_ = false;
        std::vector<ShardContext*> siblings_{};
        std::mutex control_mutex_{};
        bool control_closed_ = false;  ///< Guarded by `control_mutex_`.
        ArpSink arp_sink_{};
    };

}  // namespace aloe::runtime
