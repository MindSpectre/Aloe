#pragma once

#include <aloe/core>
#include <aloe/log>
#include <chrono>
#include <cstdint>

#include <counters.hpp>
#include <inbox.hpp>
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
     * `loop::Inbox::push` runs on that thread. A thread-local pointer, `current()`, names the context the
     * calling thread is running, which is how the scheduler tells same-shard from cross-shard.
     *
     * `run_once(now)` is its one verb: record the stamp, move the inbox's contents onto the run
     * queue, advance the wheel, run the work that was queued when the step began. Work queued
     * while the chain runs waits for the next step. Stop is a flag plus the scope's stop request;
     * `drained()` is the loop's exit condition.
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

        /// Stop requested, scope empty, both queues empty. Armed timers do not count.
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
    };

}  // namespace aloe::runtime
