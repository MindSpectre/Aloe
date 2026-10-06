#include <aloe/runtime>
#include <chrono>
#include <stdexcept>

#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using TimePoint = aloe::core::TimePoint;

    constexpr TimePoint start{};

    class ShardTaskTest : public testing::Test {
    protected:
        aloe::runtime::ShardContext context_{{.index = 0}, start};
        aloe::runtime::Scheduler scheduler_{context_};
        aloe::runtime::ShardContext::Current current_{context_};
    };

    aloe::runtime::task<void> wait_once(aloe::runtime::Scheduler scheduler) {
        co_await scheduler.schedule_after(1ms);
    }

    aloe::runtime::task<int> child(aloe::runtime::Scheduler expected, bool* stop_seen) {
        const auto here  = co_await aloe::execution::ex::read_env(aloe::execution::ex::get_scheduler);
        const auto token = co_await aloe::execution::ex::read_env(aloe::execution::ex::get_stop_token);
        *stop_seen       = token.stop_possible();
        co_return here == expected ? 1 : 0;
    }

    aloe::runtime::task<void> parent(aloe::runtime::Scheduler expected, int* matched, bool* stop_seen) {
        *matched = co_await child(expected, stop_seen);
    }

    aloe::runtime::task<void> parked(aloe::runtime::Scheduler scheduler) {
        co_await scheduler.schedule_after(10ms);
    }

    aloe::runtime::task<void> throws() {
        co_await aloe::execution::ex::just();
        throw std::runtime_error{"boom"};
    }

    aloe::runtime::task<void> reschedules(aloe::runtime::Scheduler scheduler) {
        co_await scheduler.schedule();
    }

    aloe::runtime::task<int> answers() {
        co_return 42;
    }

}  // namespace

// Done-when 3: the proof that the concrete scheduler keeps the task from rescheduling.
TEST_F(ShardTaskTest, ATaskAwaitingATimerCostsOneWheelEntryAndNoRunQueuePush) {
    scheduler_.spawn(wait_once(scheduler_));
    EXPECT_EQ(context_.timers().pending(), 1);
    EXPECT_TRUE(context_.ready().empty());
    EXPECT_EQ(context_.scope().size(), 1);

    EXPECT_TRUE(context_.run_once(start + 1ms));
    EXPECT_EQ(context_.counters().timers_fired, 1);
    EXPECT_EQ(context_.counters().work_run, 0);
    EXPECT_EQ(context_.counters().tasks_completed, 1);
    EXPECT_TRUE(context_.scope().empty());
}

TEST_F(ShardTaskTest, AChildTaskInheritsSchedulerAndStopToken) {
    int matched    = 0;
    bool stop_seen = false;
    scheduler_.spawn(parent(scheduler_, &matched, &stop_seen));
    EXPECT_EQ(matched, 1);
    EXPECT_TRUE(stop_seen) << "the scope's stop token, not an unstoppable one, reaches the child";
    EXPECT_EQ(context_.counters().work_run, 0) << "awaiting a child task reschedules nothing";
}

TEST_F(ShardTaskTest, AStopRequestUnwindsATaskParkedOnATimer) {
    scheduler_.spawn(parked(scheduler_));
    EXPECT_EQ(context_.timers().pending(), 1);

    context_.request_stop();
    EXPECT_EQ(context_.timers().pending(), 0) << "the timer was cancelled";
    EXPECT_EQ(context_.counters().tasks_stopped, 1);
    EXPECT_TRUE(context_.scope().empty());
    EXPECT_TRUE(context_.drained());
}

TEST_F(ShardTaskTest, AnExceptionInATaskIsCountedAsFailed) {
    scheduler_.spawn(throws());
    EXPECT_EQ(context_.counters().tasks_failed, 1);
    EXPECT_TRUE(context_.scope().empty());
}

TEST_F(ShardTaskTest, ATaskAwaitingScheduleRunsOnTheNextStep) {
    scheduler_.spawn(reschedules(scheduler_));
    EXPECT_FALSE(context_.ready().empty());
    EXPECT_EQ(context_.counters().tasks_completed, 0);

    EXPECT_TRUE(context_.run_once(start));
    EXPECT_EQ(context_.counters().work_run, 1);
    EXPECT_EQ(context_.counters().tasks_completed, 1);
}

TEST_F(ShardTaskTest, ATaskReturningAValueCompletesAndTheValueIsDropped) {
    scheduler_.spawn(answers());
    EXPECT_EQ(context_.counters().tasks_completed, 1);
}

// Spec, "Task scope": every spawn after request_stop starts with stop already requested; here for a task.
TEST_F(ShardTaskTest, ATaskSpawnedAfterStopNeverArmsItsTimerAndCompletesStopped) {
    context_.request_stop();
    scheduler_.spawn(parked(scheduler_));
    EXPECT_EQ(context_.timers().pending(), 0) << "the timer sender saw the stop before arming";
    EXPECT_EQ(context_.counters().tasks_stopped, 1);
    EXPECT_TRUE(context_.scope().empty());
    EXPECT_TRUE(context_.drained());
}
