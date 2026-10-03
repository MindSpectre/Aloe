#pragma once

#include <aloe/core>
#include <cassert>
#include <chrono>
#include <optional>
#include <utility>

#include <shard_context.hpp>
#include <timer_wheel.hpp>
#include <work.hpp>

namespace aloe::runtime {

    class Scheduler;

    namespace detail {

        /// Attributes of every shard sender: it completes on its scheduler, asynchronously, so a
        /// task started on that scheduler awaits it without rescheduling.
        struct ShardSenderAttributes {
            ShardContext* context;

            template <typename Tag>
            [[nodiscard]] Scheduler query(core::ex::get_completion_scheduler_t<Tag>) const noexcept;

            template <typename Tag>
            [[nodiscard]] static constexpr auto query(core::get_completion_behavior_t<Tag>) noexcept {
                return core::completion_behavior::asynchronous_affine;
            }
        };

        /// The operation state of `schedule()`: a Work node the shard runs.
        template <typename Receiver>
        struct ScheduleOperation : Work {
            ShardContext* context;
            Receiver receiver;

            ScheduleOperation(ShardContext* owner, Receiver r) noexcept
                : Work{&ScheduleOperation::execute},
                  context{owner},
                  receiver{std::move(r)} {
            }

            void start() & noexcept {
                if (ShardContext::current() == context) {
                    context->ready().push(*this);
                } else {
                    context->inbox().push(*this);
                }
            }

            static void execute(Work& work) noexcept {
                auto& self = static_cast<ScheduleOperation&>(work);
                if (core::ex::get_stop_token(core::ex::get_env(self.receiver)).stop_requested()) {
                    core::ex::set_stopped(std::move(self.receiver));
                } else {
                    core::ex::set_value(std::move(self.receiver));
                }
            }
        };

        struct ScheduleSender {
            using sender_concept = core::ex::sender_t;
            using completion_signatures =
                core::ex::completion_signatures<core::ex::set_value_t(), core::ex::set_stopped_t()>;

            ShardContext* context;

            template <core::ex::receiver Receiver>
            [[nodiscard]] auto connect(Receiver receiver) const noexcept -> ScheduleOperation<Receiver> {
                return ScheduleOperation<Receiver>{context, std::move(receiver)};
            }

            [[nodiscard]] ShardSenderAttributes get_env() const noexcept {
                return ShardSenderAttributes{context};
            }
        };

        /**
         * The operation state of `schedule_after` and `schedule_at`: a Timer in the wheel, and a Work
         * node for the hop through the inbox when started off the shard. Started on the shard it arms
         * at once, `after` relative to the tick stamp. A stop request, which by contract arrives on
         * the shard thread, cancels the timer and completes stopped.
         */
        template <typename Receiver>
        struct TimerOperation : Work, Timer {
            using StopToken = core::ex::stop_token_of_t<core::ex::env_of_t<Receiver>>;

            struct OnStop {
                TimerOperation* self;

                void operator()() noexcept {
                    self->cancel();
                }
            };

            using Callback = core::ex::stop_callback_for_t<StopToken, OnStop>;

            ShardContext* context;
            Receiver receiver;
            std::chrono::nanoseconds after;
            std::optional<ShardContext::TimePoint> at;
            std::optional<Callback> callback;

            TimerOperation(ShardContext* owner,
                           Receiver r,
                           const std::chrono::nanoseconds delay,
                           const std::optional<ShardContext::TimePoint> target) noexcept
                : Work{&TimerOperation::arrive},
                  Timer{&TimerOperation::fired},
                  context{owner},
                  receiver{std::move(r)},
                  after{delay},
                  at{target} {
            }

            void start() & noexcept {
                if (ShardContext::current() == context) {
                    arm();
                } else {
                    context->inbox().push(static_cast<Work&>(*this));
                }
            }

            static void arrive(Work& work) noexcept {
                static_cast<TimerOperation&>(work).arm();
            }

            void arm() noexcept {
                const StopToken token = core::ex::get_stop_token(core::ex::get_env(receiver));
                if (token.stop_requested()) {
                    core::ex::set_stopped(std::move(receiver));
                    return;
                }
                const ShardContext::TimePoint when = at.has_value() ? *at : context->now() + after;
                context->timers().arm(static_cast<Timer&>(*this), when);
                callback.emplace(token, OnStop{this});
            }

            void cancel() noexcept {
                assert(ShardContext::current() == context && "a shard's operation is stopped on its own thread");
                auto& timer = static_cast<Timer&>(*this);
                if (!timer.armed()) {
                    return;
                }
                context->timers().cancel(timer);
                callback.reset();
                core::ex::set_stopped(std::move(receiver));
            }

            static void fired(Timer& timer) noexcept {
                auto& self = static_cast<TimerOperation&>(timer);
                self.callback.reset();
                core::ex::set_value(std::move(self.receiver));
            }
        };

        struct TimerSender {
            using sender_concept = core::ex::sender_t;
            using completion_signatures =
                core::ex::completion_signatures<core::ex::set_value_t(), core::ex::set_stopped_t()>;

            ShardContext* context;
            std::chrono::nanoseconds after;
            std::optional<ShardContext::TimePoint> at;

            template <core::ex::receiver Receiver>
            [[nodiscard]] auto connect(Receiver receiver) const noexcept -> TimerOperation<Receiver> {
                return TimerOperation<Receiver>{context, std::move(receiver), after, at};
            }

            [[nodiscard]] ShardSenderAttributes get_env() const noexcept {
                return ShardSenderAttributes{context};
            }
        };

        /// The environment spawned work starts with: the scheduler, under both names stdexec asks for.
        struct SchedulerEnv;

    }  // namespace detail

    /**
     * @brief A shard's scheduler: one pointer to its context, a value type, never type-erased.
     *
     * Models the stdexec scheduler and adds the timed shape: `now()`, `schedule_after`,
     * `schedule_at`. Same-shard `schedule()` pushes onto the run queue with no atomics; from any
     * other thread it goes through the inbox. Timers arm into the shard's wheel. Every sender
     * completes on the shard and says so, so a task bound to this scheduler never reschedules.
     * Deliberately not default-constructible: a task started without a shard fails to compile.
     */
    class Scheduler {
    public:
        using scheduler_concept = core::ex::scheduler_t;
        using TimePoint         = ShardContext::TimePoint;

        explicit Scheduler(ShardContext& context) noexcept
            : context_{&context} {
        }

        [[nodiscard]] detail::ScheduleSender schedule() const noexcept {
            return detail::ScheduleSender{context_};
        }

        /// Relative to the tick stamp at the moment the operation arms on the shard.
        [[nodiscard]] detail::TimerSender schedule_after(const std::chrono::nanoseconds delay) const noexcept {
            return detail::TimerSender{context_, delay, std::nullopt};
        }

        [[nodiscard]] detail::TimerSender schedule_at(const TimePoint deadline) const noexcept {
            return detail::TimerSender{context_, std::chrono::nanoseconds::zero(), deadline};
        }

        /// The tick stamp, never a clock read.
        [[nodiscard]] TimePoint now() const noexcept {
            return context_->now();
        }

        /// Spawns into the shard's scope with this scheduler in the environment. Shard thread only.
        template <core::ex::sender Sender>
        void spawn(Sender&& sender) const;

        [[nodiscard]] ShardContext& context() const noexcept {
            return *context_;
        }

        [[nodiscard]] static constexpr auto query(core::ex::get_forward_progress_guarantee_t) noexcept {
            return core::ex::forward_progress_guarantee::parallel;
        }

        friend bool operator==(const Scheduler&, const Scheduler&) noexcept = default;

    private:
        ShardContext* context_;
    };

    namespace detail {

        struct SchedulerEnv {
            Scheduler scheduler;

            [[nodiscard]] Scheduler query(core::ex::get_scheduler_t) const noexcept {
                return scheduler;
            }

            [[nodiscard]] Scheduler query(core::ex::get_start_scheduler_t) const noexcept {
                return scheduler;
            }
        };

        template <typename Tag>
        Scheduler ShardSenderAttributes::query(core::ex::get_completion_scheduler_t<Tag>) const noexcept {
            return Scheduler{*context};
        }

    }  // namespace detail

    template <core::ex::sender Sender>
    void Scheduler::spawn(Sender&& sender) const {
        assert(ShardContext::current() == context_ && "spawn on the shard's own thread; hop with schedule() first");
        context_->scope().spawn(std::forward<Sender>(sender), detail::SchedulerEnv{*this});
    }

}  // namespace aloe::runtime
