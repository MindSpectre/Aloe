#include <algorithm>
#include <aloe/fabric>
#include <aloe/runtime>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <echo_stack.hpp>
#include <frames.hpp>
#include <gtest/gtest.h>
#include <sched.h>

namespace {

    using namespace std::chrono_literals;

    constexpr std::uint16_t queues = 4;
    constexpr aloe::device::MacAddress server{0x02, 0, 0, 0, 0, 0x01};
    constexpr aloe::device::MacAddress client{0x02, 0, 0, 0, 0, 0x02};
    constexpr aloe::device::Ipv4Address server_ip{10, 0, 0, 2};
    constexpr aloe::device::Ipv4Address client_ip{10, 0, 0, 1};
    constexpr auto patience = 5s;

    using EchoRuntime = aloe::runtime::Runtime<aloe::fabric::Port, aloe::testing::EchoStack<aloe::fabric::Port>>;

    /// Polls `condition` until it holds or `patience` runs out.
    [[nodiscard]] bool eventually(const std::function<bool()>& condition) {
        const auto deadline = std::chrono::steady_clock::now() + patience;
        while (std::chrono::steady_clock::now() < deadline) {
            if (condition()) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return condition();
    }

    aloe::runtime::task<void> mark(std::atomic<int>* slot, std::atomic<int>* ran_on) {
        const auto scheduler = co_await aloe::core::ex::read_env(aloe::core::ex::get_scheduler);
        ran_on->store(scheduler.context().index());
        slot->store(1);
    }

    aloe::runtime::task<void> park(aloe::runtime::Scheduler scheduler, std::atomic<int>* parked) {
        parked->store(1);
        co_await scheduler.schedule_after(10s);
    }

    class RuntimeTest : public testing::Test {
    protected:
        aloe::fabric::Fabric fabric_;
        aloe::fabric::Port& server_ = fabric_.add_port({.mac = server, .queues = queues, .pool_size = 64});
        aloe::fabric::Port& client_ = fabric_.add_port({.mac = client, .queues = 1, .pool_size = 64});
        aloe::runtime::RuntimeConfig config_{
            .shard = {.idle = aloe::runtime::IdlePolicy::Yield, .yield_after = 10},
              .threads = {},
              .thread_hook = {}
        };
    };

}  // namespace

TEST_F(RuntimeTest, OneShardPerQueueRunsSpawnedWorkOnTheRightShardThenStopsAndJoins) {
    // Declared before the runtime, so a failed ASSERT stops and joins the shards before the flags die.
    std::array<std::atomic<int>, queues> done{};
    std::array<std::atomic<int>, queues> ran_on{};
    EchoRuntime runtime{config_, server_};
    EXPECT_EQ(runtime.shard_count(), queues);
    runtime.start();

    for (std::uint16_t index = 0; index < queues; ++index) {
        ran_on[index].store(-1);
        runtime.spawn(index, mark(&done[index], &ran_on[index]));
    }
    ASSERT_TRUE(eventually(
        [&] { return std::ranges::all_of(done, [](const std::atomic<int>& flag) { return flag.load() != 0; }); }));
    for (std::uint16_t index = 0; index < queues; ++index) {
        EXPECT_EQ(ran_on[index].load(), static_cast<int>(index)) << "spawned onto shard " << index;
    }

    runtime.stop();
    runtime.join();
    for (std::uint16_t index = 0; index < queues; ++index) {
        EXPECT_EQ(runtime.counters(index).tasks_completed, 1) << "shard " << index;
        EXPECT_GT(runtime.counters(index).ticks, 0U);
    }
}

TEST_F(RuntimeTest, TheHookRunsOnceOnEveryShardThreadBeforeStartReturns) {
    std::atomic<int> hook_calls{0};
    config_.thread_hook = [&hook_calls] { ++hook_calls; };
    EchoRuntime runtime{config_, server_};
    runtime.start();
    EXPECT_EQ(hook_calls.load(), queues);
}

TEST_F(RuntimeTest, AFailingHookOrAnImpossibleCpuThrowsFromStartAndLeavesNothingRunning) {
    {
        config_.thread_hook = [] { throw std::runtime_error{"no lcore for you"}; };
        EchoRuntime runtime{config_, server_};
        EXPECT_THROW(runtime.start(), aloe::runtime::RuntimeError);
    }
    {
        config_.thread_hook = {};
        config_.threads.assign(
            queues, aloe::runtime::ShardThread{.cpu = static_cast<unsigned>(CPU_SETSIZE - 1), .name = "nowhere"});
        EchoRuntime runtime{config_, server_};
        EXPECT_THROW(runtime.start(), aloe::runtime::RuntimeError);
    }
}

// The hook fails on one thread only, so start must stop and join the shards that already entered run().
TEST_F(RuntimeTest, AHookThatFailsOnOneThreadStopsTheShardsAlreadyRunning) {
    std::atomic<int> hook_calls{0};
    config_.thread_hook = [&hook_calls] {
        if (++hook_calls == 2) {
            throw std::runtime_error{"second hook fails"};
        }
    };
    EchoRuntime runtime{config_, server_};
    EXPECT_THROW(runtime.start(), aloe::runtime::RuntimeError);

    // start() stopped and joined every thread before it threw, so the counters are safe to read here.
    EXPECT_EQ(hook_calls.load(), queues);
    std::uint64_t ticks = 0;
    for (std::uint16_t index = 0; index < queues; ++index) {
        ticks += runtime.counters(index).ticks;
    }
    EXPECT_GT(ticks, 0U) << "the shards whose hook passed ran before the stop reached them";
}

TEST_F(RuntimeTest, ConstructionRejectsAThreadListThatDoesNotMatchTheQueues) {
    config_.threads.assign(queues - 1, aloe::runtime::ShardThread{});
    EXPECT_THROW((EchoRuntime{config_, server_}), aloe::runtime::RuntimeError);
}

TEST_F(RuntimeTest, StartTwiceThrows) {
    EchoRuntime runtime{config_, server_};
    runtime.start();
    EXPECT_THROW(runtime.start(), aloe::runtime::RuntimeError);
}

// Stop with tasks parked on timers returns promptly, every task stopped.
TEST_F(RuntimeTest, StopUnwindsParkedTasksOnEveryShardAndJoinReturnsPromptly) {
    // Declared before the runtime, so a failed ASSERT stops and joins the shards before the flags die.
    std::array<std::atomic<int>, queues> parked{};
    EchoRuntime runtime{config_, server_};
    runtime.start();
    for (std::uint16_t index = 0; index < queues; ++index) {
        runtime.spawn(index, park(runtime.scheduler(index), &parked[index]));
    }
    ASSERT_TRUE(eventually(
        [&] { return std::ranges::all_of(parked, [](const std::atomic<int>& flag) { return flag.load() != 0; }); }));

    const auto before = std::chrono::steady_clock::now();
    runtime.stop();
    runtime.join();
    EXPECT_LT(std::chrono::steady_clock::now() - before, 2s) << "the ten-second timers were cancelled, not awaited";
    for (std::uint16_t index = 0; index < queues; ++index) {
        EXPECT_EQ(runtime.counters(index).tasks_stopped, 1) << "shard " << index;
    }
}

TEST_F(RuntimeTest, AFrameIsAnsweredByTheShardItsHashSelects) {
    EchoRuntime runtime{config_, server_};
    runtime.start();
    const aloe::testing::Ipv4Spec spec{.destination_mac  = server,
                                       .source_mac       = client,
                                       .source           = client_ip,
                                       .destination      = server_ip,
                                       .source_port      = 40000,
                                       .destination_port = 80,
                                       .protocol         = aloe::device::Ipv4Protocol::Udp};
    const auto frame             = aloe::testing::ipv4_frame(spec, aloe::testing::pattern(16));
    const std::uint16_t expected = aloe::device::queue_for(server_.steering(), aloe::testing::flow_of(spec));

    auto packet = client_.allocate(0);
    ASSERT_TRUE(packet.has_value());
    ASSERT_TRUE(aloe::testing::fill(*packet, frame));
    std::array<aloe::fabric::Packet, 1> burst{std::move(*packet)};
    ASSERT_EQ(client_.transmit(0, burst), 1);

    std::optional<std::vector<std::byte>> reply;
    ASSERT_TRUE(eventually([&] {
        std::array<aloe::fabric::Packet, 1> received;
        if (client_.receive(0, received) == 1) {
            reply = aloe::testing::bytes_of(received[0]);
            return true;
        }
        return false;
    }));
    EXPECT_EQ(aloe::testing::stamp_of(*reply), expected);
}
