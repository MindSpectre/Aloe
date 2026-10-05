#include <aloe/runtime>
#include <chrono>
#include <concepts>
#include <thread>
#include <tuple>
#include <utility>

#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using TimePoint = aloe::core::TimePoint;

    constexpr TimePoint start{};

    struct Outcome {
        bool value   = false;
        bool stopped = false;
        std::thread::id thread;
    };

    /// Records how and where it completed; its environment carries the stop token the test controls.
    struct RecordingReceiver {
        using receiver_concept = aloe::execution::ex::receiver_t;

        struct Env {
            aloe::execution::ex::inplace_stop_token token;

            [[nodiscard]] aloe::execution::ex::inplace_stop_token
            query(aloe::execution::ex::get_stop_token_t) const noexcept {
                return token;
            }
        };

        Outcome* outcome;
        aloe::execution::ex::inplace_stop_token token;

        void set_value() const noexcept {
            outcome->value  = true;
            outcome->thread = std::this_thread::get_id();
        }

        void set_stopped() const noexcept {
            outcome->stopped = true;
            outcome->thread  = std::this_thread::get_id();
        }

        [[nodiscard]] Env get_env() const noexcept {
            return Env{token};
        }
    };

    class SchedulerTest : public testing::Test {
    protected:
        aloe::runtime::ShardContext context_{{.index = 0}, start};
        aloe::runtime::Scheduler scheduler_{context_};
        aloe::execution::ex::inplace_stop_source stop_;
        Outcome outcome_;

        [[nodiscard]] RecordingReceiver receiver() noexcept {
            return RecordingReceiver{&outcome_, stop_.get_token()};
        }
    };

    /// A sender that records whether its receiver's environment answers get_scheduler with the expected scheduler.
    struct SeesScheduler {
        using sender_concept        = aloe::execution::ex::sender_t;
        using completion_signatures = aloe::execution::ex::completion_signatures<aloe::execution::ex::set_value_t()>;
        bool* matched;
        aloe::runtime::Scheduler expected;

        template <aloe::execution::ex::receiver Receiver>
        struct Operation {
            Receiver receiver;
            bool* matched;
            aloe::runtime::Scheduler expected;

            Operation(Receiver r, bool* m, const aloe::runtime::Scheduler e)
                : receiver{std::move(r)},
                  matched{m},
                  expected{e} {
            }

            void start() & noexcept {
                *matched = aloe::execution::ex::get_scheduler(aloe::execution::ex::get_env(receiver)) == expected;
                aloe::execution::ex::set_value(std::move(receiver));
            }
        };

        template <aloe::execution::ex::receiver Receiver>
        [[nodiscard]] auto connect(Receiver receiver) const -> Operation<Receiver> {
            return Operation<Receiver>{std::move(receiver), matched, expected};
        }
    };

    static_assert(aloe::execution::ex::scheduler<aloe::runtime::Scheduler>);
    static_assert(!std::default_initializable<aloe::runtime::Scheduler>);

}  // namespace

TEST_F(SchedulerTest, EqualityFollowsTheContext) {
    aloe::runtime::ShardContext other{{.index = 1}, start};
    EXPECT_EQ(scheduler_, aloe::runtime::Scheduler{context_});
    EXPECT_NE(scheduler_, aloe::runtime::Scheduler{other});
    EXPECT_EQ(&scheduler_.context(), &context_);
}

TEST_F(SchedulerTest, SameShardScheduleUsesTheRunQueueAndNeverTheInbox) {
    const aloe::runtime::ShardContext::Current current{context_};
    auto operation = aloe::execution::ex::connect(scheduler_.schedule(), receiver());
    aloe::execution::ex::start(operation);
    EXPECT_FALSE(context_.ready().empty());
    EXPECT_TRUE(context_.inbox().empty());

    EXPECT_TRUE(context_.run_once(start));
    EXPECT_TRUE(outcome_.value);
    EXPECT_EQ(context_.counters().work_run, 1);
    EXPECT_EQ(context_.counters().inbox_received, 0);
}

TEST_F(SchedulerTest, CrossThreadScheduleGoesThroughTheInboxAndCompletesOnTheShard) {
    auto operation = aloe::execution::ex::connect(scheduler_.schedule(), receiver());
    {
        std::jthread elsewhere{[&] { aloe::execution::ex::start(operation); }};
    }
    EXPECT_FALSE(context_.inbox().empty());
    EXPECT_TRUE(context_.ready().empty());

    EXPECT_TRUE(context_.run_once(start));
    EXPECT_TRUE(outcome_.value);
    EXPECT_EQ(outcome_.thread, std::this_thread::get_id()) << "completed on the thread that ran the step";
    EXPECT_EQ(context_.counters().inbox_received, 1);
}

// A stop requested after start, before the shard gets to the work, completes it stopped.
TEST_F(SchedulerTest, StopRequestedBeforeTheWorkRunsCompletesStopped) {
    const aloe::runtime::ShardContext::Current current{context_};
    auto operation = aloe::execution::ex::connect(scheduler_.schedule(), receiver());
    aloe::execution::ex::start(operation);
    stop_.request_stop();

    EXPECT_TRUE(context_.run_once(start));
    EXPECT_TRUE(outcome_.stopped);
    EXPECT_FALSE(outcome_.value);
}

TEST_F(SchedulerTest, ScheduleAfterFiresOnTheFirstStepThatReachesItWithNoPush) {
    const aloe::runtime::ShardContext::Current current{context_};
    auto operation = aloe::execution::ex::connect(scheduler_.schedule_after(5ms), receiver());
    aloe::execution::ex::start(operation);
    EXPECT_EQ(context_.timers().pending(), 1);
    EXPECT_TRUE(context_.ready().empty());

    EXPECT_FALSE(context_.run_once(start + 4ms));
    EXPECT_FALSE(outcome_.value);
    EXPECT_TRUE(context_.run_once(start + 5ms));
    EXPECT_TRUE(outcome_.value);
    EXPECT_EQ(context_.counters().timers_fired, 1);
    EXPECT_EQ(context_.counters().work_run, 0) << "a timer completion is not a run-queue push";
}

TEST_F(SchedulerTest, ScheduleAtIsAbsoluteAndNowIsTheStamp) {
    const aloe::runtime::ShardContext::Current current{context_};
    std::ignore = context_.run_once(start + 7ms);
    EXPECT_EQ(scheduler_.now(), start + 7ms);

    auto operation = aloe::execution::ex::connect(scheduler_.schedule_at(start + 20ms), receiver());
    aloe::execution::ex::start(operation);
    EXPECT_FALSE(context_.run_once(start + 19ms));
    EXPECT_TRUE(context_.run_once(start + 20ms));
    EXPECT_TRUE(outcome_.value);
}

// A stop request cancels an armed timer on the shard thread and completes stopped.
TEST_F(SchedulerTest, AStopRequestCancelsAnArmedTimer) {
    const aloe::runtime::ShardContext::Current current{context_};
    auto operation = aloe::execution::ex::connect(scheduler_.schedule_after(10ms), receiver());
    aloe::execution::ex::start(operation);
    EXPECT_EQ(context_.timers().pending(), 1);

    stop_.request_stop();
    EXPECT_TRUE(outcome_.stopped);
    EXPECT_EQ(context_.timers().pending(), 0);
    EXPECT_FALSE(context_.run_once(start + 10ms)) << "nothing left to fire";
    EXPECT_FALSE(outcome_.value);
}

TEST_F(SchedulerTest, ATimerStartedOffTheShardHopsThroughTheInboxThenArms) {
    auto operation = aloe::execution::ex::connect(scheduler_.schedule_after(5ms), receiver());
    {
        std::jthread elsewhere{[&] { aloe::execution::ex::start(operation); }};
    }
    EXPECT_FALSE(context_.inbox().empty());
    EXPECT_EQ(context_.timers().pending(), 0);

    EXPECT_TRUE(context_.run_once(start));
    EXPECT_EQ(context_.timers().pending(), 1) << "armed on arrival, relative to the shard's stamp";
    EXPECT_FALSE(outcome_.value);
    EXPECT_TRUE(context_.run_once(start + 5ms));
    EXPECT_TRUE(outcome_.value);
}

TEST_F(SchedulerTest, SpawnRunsOnTheShardWithTheSchedulerInTheEnvironment) {
    const aloe::runtime::ShardContext::Current current{context_};
    bool matched = false;
    scheduler_.spawn(SeesScheduler{&matched, scheduler_});
    EXPECT_TRUE(matched);
    EXPECT_EQ(context_.counters().tasks_completed, 1);
    EXPECT_TRUE(context_.scope().empty());
}
