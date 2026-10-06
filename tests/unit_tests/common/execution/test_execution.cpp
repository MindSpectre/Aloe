#include <aloe/execution>
#include <concepts>
#include <exception>
#include <optional>
#include <stdexcept>
#include <tuple>

#include <gtest/gtest.h>

namespace {

    aloe::execution::task<int> add_one(int value) {
        co_return value + 1;
    }

    aloe::execution::task<int> chain() {
        const int first  = co_await add_one(1);
        const int second = co_await (aloe::execution::ex::just(first) |
                                     aloe::execution::ex::then([](int value) { return value * 2; }));
        co_return second;
    }

    aloe::execution::task<int> stops_before_returning() {
        co_await aloe::execution::ex::just_stopped();
        co_return 1;
    }

    /// Stands in for a scope's spawn receiver: its environment names the scheduler a task starts on.
    struct LoopReceiver {
        using receiver_concept = aloe::execution::ex::receiver_t;

        struct Env {
            aloe::execution::ex::run_loop* loop;

            [[nodiscard]] auto query(aloe::execution::ex::get_scheduler_t) const noexcept {
                return loop->get_scheduler();
            }

            [[nodiscard]] auto query(aloe::execution::ex::get_start_scheduler_t) const noexcept {
                return loop->get_scheduler();
            }
        };

        aloe::execution::ex::run_loop* loop;
        std::optional<bool>* result;

        void set_value(const bool same) const noexcept {
            *result = same;
            loop->finish();
        }

        void set_error(const std::exception_ptr&) const noexcept {
            loop->finish();
        }

        void set_stopped() const noexcept {
            loop->finish();
        }

        [[nodiscard]] Env get_env() const noexcept {
            return Env{loop};
        }
    };

}  // namespace

TEST(Execution, SenderPipelineRunsToCompletion) {
    const auto result = aloe::execution::ex::sync_wait(aloe::execution::ex::just(41) |
                                                       aloe::execution::ex::then([](int value) { return value + 1; }));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<0>(*result), 42);
}

TEST(Execution, CoroutineTaskAwaitsTasksAndSenders) {
    const auto result = aloe::execution::ex::sync_wait(chain());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<0>(*result), 4);
}

TEST(Execution, StoppedTaskYieldsNoValue) {
    const auto result = aloe::execution::ex::sync_wait(stops_before_returning());

    EXPECT_FALSE(result.has_value());
}

TEST(Execution, ErrorSurfacesAsAnException) {
    EXPECT_THROW(std::ignore = aloe::execution::ex::sync_wait(
                     aloe::execution::ex::just(1) |
                     aloe::execution::ex::then([](int) -> int { throw std::runtime_error{"boom"}; })),
                 std::runtime_error);
}

TEST(Execution, TaskEnvironmentBindsAConcreteScheduler) {
    aloe::execution::ex::run_loop loop;
    using Scheduler = decltype(loop.get_scheduler());
    using Env       = aloe::execution::TaskEnvironment<Scheduler>;
    static_assert(std::same_as<aloe::execution::task<bool, Env>::start_scheduler_type, Scheduler>);

    const auto body = [](Scheduler expected) -> aloe::execution::task<bool, Env> {
        const auto here = co_await aloe::execution::ex::read_env(aloe::execution::ex::get_scheduler);
        co_return here == expected;
    };

    std::optional<bool> result;
    auto operation = aloe::execution::ex::connect(body(loop.get_scheduler()), LoopReceiver{&loop, &result});
    aloe::execution::ex::start(operation);
    loop.run();

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(*result) << "the task's scheduler is the one its receiver's environment named";
}
