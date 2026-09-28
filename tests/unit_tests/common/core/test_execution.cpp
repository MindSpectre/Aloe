#include <aloe/core>
#include <stdexcept>
#include <tuple>

#include <gtest/gtest.h>

namespace {

    aloe::task<int> add_one(int value) {
        co_return value + 1;
    }

    aloe::task<int> chain() {
        const int first  = co_await add_one(1);
        const int second = co_await (aloe::ex::just(first) | aloe::ex::then([](int value) { return value * 2; }));
        co_return second;
    }

    aloe::task<int> stops_before_returning() {
        co_await aloe::ex::just_stopped();
        co_return 1;
    }

}  // namespace

TEST(Execution, SenderPipelineRunsToCompletion) {
    const auto result = aloe::ex::sync_wait(aloe::ex::just(41) | aloe::ex::then([](int value) { return value + 1; }));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<0>(*result), 42);
}

TEST(Execution, CoroutineTaskAwaitsTasksAndSenders) {
    const auto result = aloe::ex::sync_wait(chain());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<0>(*result), 4);
}

TEST(Execution, StoppedTaskYieldsNoValue) {
    const auto result = aloe::ex::sync_wait(stops_before_returning());

    EXPECT_FALSE(result.has_value());
}

TEST(Execution, ErrorSurfacesAsAnException) {
    EXPECT_THROW(std::ignore = aloe::ex::sync_wait(
                     aloe::ex::just(1) | aloe::ex::then([](int) -> int { throw std::runtime_error{"boom"}; })),
                 std::runtime_error);
}
