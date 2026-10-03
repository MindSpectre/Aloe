#include <algorithm>
#include <aloe/ethdev>
#include <aloe/runtime>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <set>
#include <thread>
#include <utility>

#include <eal_environment.hpp>
#include <echo_stack.hpp>
#include <frames.hpp>
#include <gtest/gtest.h>
#include <logging_environment.hpp>
#include <rte_lcore.h>

namespace {

    using namespace std::chrono_literals;

    constexpr std::uint16_t queues = 4;
    constexpr auto patience        = 20s;
    constexpr aloe::device::MacAddress peer{0x02, 0, 0, 0, 0xfe, 0xed};

    const auto* const environment = ::testing::AddGlobalTestEnvironment(new aloe::testing::EalEnvironment{"net_ring0"});
    const auto* const logging     = ::testing::AddGlobalTestEnvironment(new aloe::testing::LoggingEnvironment{});

    using Queue = aloe::runtime::ShardQueue<aloe::ethdev::Port>;
    using Echo  = aloe::testing::EchoStack<aloe::ethdev::Port>;

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

    /// On its own shard: records the lcore id the hook gave the thread, injects one frame addressed
    /// to the port, and waits until the echo answered it and dropped the answer that came back.
    aloe::runtime::task<void> inject(Queue* queue,
                                     Echo* stack,
                                     aloe::runtime::Scheduler scheduler,
                                     std::atomic<unsigned>* lcore,
                                     std::atomic<int>* outcome) {
        lcore->store(rte_lcore_id());
        const auto frame = aloe::testing::ethernet_frame(
            queue->mac(), peer, aloe::testing::ethertype_experimental, aloe::testing::pattern(40));
        auto packet = queue->allocate();
        if (!packet || !aloe::testing::fill(*packet, frame) || !queue->transmit(std::move(*packet))) {
            outcome->store(-1);
            co_return;
        }
        for (int tick = 0; tick < 5000; ++tick) {
            if (stack->echoed() >= 1 && stack->dropped() >= 1) {
                outcome->store(1);
                co_return;
            }
            co_await scheduler.schedule_after(1ms);
        }
        outcome->store(-2);
    }

}  // namespace

// Done-when 2, ring half: each shard's transmit loops back onto its own queue, with real mbufs, on a
// thread the hook registered with DPDK, and nothing crosses between queues.
TEST(RuntimeRing, EachShardEchoesOnItsOwnQueueWithARegisteredThread) {
    // Declared before the runtime, so a failed ASSERT stops and joins the shards before the slots die.
    std::array<std::atomic<unsigned>, queues> lcores{};
    std::array<std::atomic<int>, queues> outcomes{};
    aloe::ethdev::Port port{
        {.name = aloe::testing::probe_vdev("net_ring"), .queues = queues, .pool_size = 512}
    };
    aloe::runtime::Runtime<aloe::ethdev::Port, Echo> runtime{
        {.shard       = {.idle = aloe::runtime::IdlePolicy::Yield, .yield_after = 10},
         .threads     = {},
         .thread_hook = aloe::ethdev::register_thread},
        port
    };
    runtime.start();

    for (std::uint16_t index = 0; index < queues; ++index) {
        runtime.spawn(index,
                      inject(&runtime.shard(index).queue(),
                             &runtime.shard(index).stack(),
                             runtime.scheduler(index),
                             &lcores[index],
                             &outcomes[index]));
    }
    ASSERT_TRUE(eventually([&] {
        return std::ranges::all_of(outcomes, [](const std::atomic<int>& outcome) { return outcome.load() != 0; });
    }));
    runtime.stop();
    runtime.join();

    std::set<unsigned> distinct;
    for (std::uint16_t index = 0; index < queues; ++index) {
        EXPECT_EQ(outcomes[index].load(), 1) << "shard " << index << ": -1 could not inject, -2 never saw the echo";
        EXPECT_NE(lcores[index].load(), LCORE_ID_ANY) << "the hook registered the thread";
        distinct.insert(lcores[index].load());
        EXPECT_EQ(runtime.counters(index).frames_received, 2)
            << "shard " << index << ": the injected frame and its own answer";
        EXPECT_EQ(runtime.counters(index).frames_transmitted, 2) << "shard " << index;
        EXPECT_EQ(runtime.shard(index).stack().echoed(), 1) << "shard " << index;
        EXPECT_EQ(runtime.shard(index).stack().dropped(), 1) << "shard " << index;
    }
    EXPECT_EQ(distinct.size(), queues) << "every shard thread got its own lcore id";
}
