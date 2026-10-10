#include <algorithm>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/runtime>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <echo_stack.hpp>
#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using TimePoint = aloe::core::TimePoint;

    constexpr TimePoint start{};
    constexpr aloe::wire::MacAddress server{0x02, 0, 0, 0, 0, 0x01};
    constexpr aloe::wire::MacAddress client{0x02, 0, 0, 0, 0, 0x02};

    /// A device that accepts at most `accept` packets per transmit call; everything else is the port's.
    class Throttled {
    public:
        using Packet = aloe::fabric::Packet;

        explicit Throttled(aloe::fabric::Port& port) noexcept
            : port_{&port} {
        }

        std::size_t accept = 64;

        [[nodiscard]] std::uint16_t queue_count() const noexcept {
            return port_->queue_count();
        }

        [[nodiscard]] aloe::wire::MacAddress mac() const noexcept {
            return port_->mac();
        }

        [[nodiscard]] std::uint16_t mtu() const noexcept {
            return port_->mtu();
        }

        [[nodiscard]] bool link_up() const noexcept {
            return port_->link_up();
        }

        [[nodiscard]] const aloe::device::Capabilities& capabilities() const noexcept {
            return port_->capabilities();
        }

        [[nodiscard]] const aloe::device::RssDescription& steering() const noexcept {
            return port_->steering();
        }

        [[nodiscard]] std::optional<Packet> allocate(const std::uint16_t queue) const noexcept {
            return port_->allocate(queue);
        }

        [[nodiscard]] std::size_t receive(const std::uint16_t queue, std::span<Packet> out) const noexcept {
            return port_->receive(queue, out);
        }

        [[nodiscard]] std::size_t transmit(const std::uint16_t queue, std::span<Packet> in) const noexcept {
            return port_->transmit(queue, in.first(std::min(in.size(), accept)));
        }

        [[nodiscard]] aloe::device::QueueCounters counters(const std::uint16_t queue) const noexcept {
            return port_->counters(queue);
        }

    private:
        aloe::fabric::Port* port_;
    };

    static_assert(aloe::device::IsDevice<Throttled>);
    static_assert(aloe::runtime::IsStack<aloe::testing::EchoStack<aloe::fabric::Port>, aloe::fabric::Port>);

    void send(aloe::fabric::Port& from, const std::span<const std::byte> frame) {
        auto packet = from.allocate(0);
        ASSERT_TRUE(packet.has_value());
        ASSERT_TRUE(aloe::frames::fill(*packet, frame));
        std::array<aloe::fabric::Packet, 1> burst{std::move(*packet)};
        ASSERT_EQ(from.transmit(0, burst), 1);
    }

    std::vector<std::vector<std::byte>> drain(aloe::fabric::Port& port) {
        std::vector<std::vector<std::byte>> frames;
        std::array<aloe::fabric::Packet, 16> burst;
        for (std::size_t count = port.receive(0, burst); count > 0; count = port.receive(0, burst)) {
            for (std::size_t index = 0; index < count; ++index) {
                frames.push_back(aloe::frames::bytes_of(burst[index]));
                burst[index] = aloe::fabric::Packet{};
            }
        }
        return frames;
    }

    class ShardTest : public testing::Test {
    protected:
        aloe::fabric::Fabric fabric_;
        aloe::fabric::Port& server_ = fabric_.add_port({.mac = server, .queues = 1, .pool_size = 16});
        aloe::fabric::Port& client_ = fabric_.add_port({.mac = client, .queues = 1, .pool_size = 64});
        aloe::runtime::ShardConfig config_{
            .receive_burst = 8, .transmit_ring = 4, .idle = aloe::runtime::IdlePolicy::Yield};
    };

    aloe::runtime::task<void> parked(aloe::runtime::Scheduler scheduler) {
        co_await scheduler.schedule_after(10s);
    }

    /// A work node that counts its runs.
    struct Counted : aloe::loop::Work {
        Counted() noexcept
            : aloe::loop::Work{&Counted::run_it} {
        }

        static void run_it(aloe::loop::Work& work) noexcept {
            ++static_cast<Counted&>(work).runs;
        }

        int runs = 0;
    };

    /**
     * A stack that records what the shard calls, in order, with the stamps it sees; `on_tick` queues work.
     * The ready work queues `follow_up` once, when it is set, to show that work queued by the chain waits.
     */
    class HookStack {
    public:
        using Packet = aloe::fabric::Packet;

        struct Ready : aloe::loop::Work {
            explicit Ready(HookStack* owner) noexcept
                : aloe::loop::Work{&Ready::run_it},
                  stack{owner} {
            }

            static void run_it(aloe::loop::Work& work) noexcept {
                auto& self = static_cast<Ready&>(work);
                self.stack->order.emplace_back("ready");
                self.stack->pending_at_ready = self.stack->queue_->pending();
                self.stack->transmit_one();
                if (aloe::loop::Work* const next = std::exchange(self.stack->follow_up, nullptr); next != nullptr) {
                    self.stack->context_->ready().push(*next);
                }
            }

            HookStack* stack = nullptr;
        };

        HookStack(aloe::runtime::ShardContext& context, aloe::loop::ShardQueue<aloe::fabric::Port>& queue) noexcept
            : context_{&context},
              queue_{&queue},
              ready_{this} {
        }

        void on_receive(std::span<Packet> burst) noexcept {
            order.emplace_back("receive");
            stamps.push_back(context_->now());
            received += burst.size();
        }

        void on_tick(const aloe::core::TimePoint now) noexcept {
            order.emplace_back("tick");
            stamps.push_back(now);
            transmit_one();                  // leaves at the early flush, before tasks run
            context_->ready().push(ready_);  // runs inside run_once, this step
        }

        void on_flush(const aloe::core::TimePoint now) noexcept {
            order.emplace_back("flush");
            stamps.push_back(now);
            pending_at_flush = queue_->pending();  // the ready work's frame is still in the ring
        }

        void transmit_one() noexcept {
            auto packet = queue_->allocate();
            if (packet && aloe::frames::fill(
                              *packet,
                              aloe::frames::ethernet_frame(
                                  client, server, aloe::frames::ethertype_experimental, aloe::frames::pattern(20)))) {
                std::ignore = queue_->transmit(std::move(*packet));
            }
        }

        std::vector<std::string> order;
        std::vector<aloe::core::TimePoint> stamps;
        std::size_t received         = 0;
        std::size_t pending_at_ready = 99;
        std::size_t pending_at_flush = 99;
        aloe::loop::Work* follow_up  = nullptr;

    private:
        aloe::runtime::ShardContext* context_              = nullptr;
        aloe::loop::ShardQueue<aloe::fabric::Port>* queue_ = nullptr;
        Ready ready_;
    };

    class TickOnly {
    public:
        using Packet = aloe::fabric::Packet;

        TickOnly(aloe::runtime::ShardContext& /*context*/,
                 aloe::loop::ShardQueue<aloe::fabric::Port>& /*queue*/) noexcept {
        }

        void on_receive(std::span<Packet> /*burst*/) noexcept {
        }

        void on_tick(aloe::core::TimePoint /*now*/) noexcept {
            ++ticks;
        }

        int ticks = 0;
    };

    class FlushOnly {
    public:
        using Packet = aloe::fabric::Packet;

        FlushOnly(aloe::runtime::ShardContext& /*context*/,
                  aloe::loop::ShardQueue<aloe::fabric::Port>& /*queue*/) noexcept {
        }

        void on_receive(std::span<Packet> /*burst*/) noexcept {
        }

        void on_flush(aloe::core::TimePoint /*now*/) noexcept {
            ++flushes;
        }

        int flushes = 0;
    };

    static_assert(aloe::runtime::IsStack<HookStack, aloe::fabric::Port>);
    static_assert(aloe::runtime::IsStack<TickOnly, aloe::fabric::Port>);
    static_assert(aloe::runtime::IsStack<FlushOnly, aloe::fabric::Port>);
    static_assert(aloe::runtime::detail::HasOnTick<HookStack> && aloe::runtime::detail::HasOnFlush<HookStack>);
    static_assert(aloe::runtime::detail::HasOnTick<TickOnly> && !aloe::runtime::detail::HasOnFlush<TickOnly>);
    static_assert(!aloe::runtime::detail::HasOnTick<FlushOnly> && aloe::runtime::detail::HasOnFlush<FlushOnly>);

}  // namespace

TEST_F(ShardTest, TheEchoRepliesInTheTickTheFrameArrived) {
    aloe::runtime::Shard<aloe::fabric::Port, aloe::testing::EchoStack<aloe::fabric::Port>> shard{
        config_, server_, 0, start};
    const auto frame =
        aloe::frames::ethernet_frame(server, client, aloe::frames::ethertype_experimental, aloe::frames::pattern(30));
    send(client_, frame);

    EXPECT_TRUE(shard.step(start));
    const auto replies = drain(client_);
    ASSERT_EQ(replies.size(), 1);
    const auto& reply = replies[0];
    EXPECT_EQ(std::vector<std::byte>(reply.begin(), reply.begin() + 6),
              std::vector<std::byte>(frame.begin() + 6, frame.begin() + 12))
        << "destination is the old source";
    EXPECT_EQ(std::vector<std::byte>(reply.begin() + 6, reply.begin() + 12),
              std::vector<std::byte>(frame.begin(), frame.begin() + 6))
        << "source is the old destination";
    EXPECT_EQ(aloe::testing::stamp_of(reply), 0);
    EXPECT_EQ(std::vector<std::byte>(reply.begin() + 12, reply.end() - 2),
              std::vector<std::byte>(frame.begin() + 12, frame.end() - 2));
    EXPECT_EQ(shard.context().counters().frames_received, 1);
    EXPECT_EQ(shard.context().counters().frames_transmitted, 1);
    EXPECT_EQ(shard.context().counters().ticks, 1);
    EXPECT_EQ(shard.stack().echoed(), 1);
}

TEST_F(ShardTest, PacketsTheStackLeavesAreFreedSoThePoolNeverRunsDry) {
    aloe::runtime::Shard<aloe::fabric::Port, aloe::testing::EchoStack<aloe::fabric::Port>> shard{
        config_, server_, 0, start};
    const auto too_short =
        aloe::frames::ethernet_frame(server, client, aloe::frames::ethertype_experimental, aloe::frames::pattern(1));
    constexpr std::size_t rounds    = 5;
    constexpr std::size_t per_round = 12;  // less than the 16-packet pool, more than one 8-packet burst

    for (std::size_t round = 0; round < rounds; ++round) {
        for (std::size_t index = 0; index < per_round; ++index) {
            send(client_, too_short);
        }
        for (int step = 0; step < 4; ++step) {
            std::ignore = shard.step(start);
        }
    }
    EXPECT_EQ(shard.context().counters().frames_received, rounds * per_round)
        << "a leaked packet would have starved the pool";
    EXPECT_EQ(shard.stack().dropped(), rounds * per_round);
    EXPECT_EQ(shard.context().counters().frames_transmitted, 0);
    EXPECT_TRUE(drain(client_).empty());
}

// The ring fills, the device refuses, and the caller keeps its packet.
TEST_F(ShardTest, TheTransmitRingFlushesWhenFullAndRefusesWhenTheDeviceDoes) {
    Throttled throttled{server_};
    aloe::runtime::Shard<Throttled, aloe::testing::EchoStack<Throttled>> shard{config_, throttled, 0, start};
    const auto frame =
        aloe::frames::ethernet_frame(server, client, aloe::frames::ethertype_experimental, aloe::frames::pattern(20));
    for (int index = 0; index < 6; ++index) {
        send(client_, frame);
    }

    throttled.accept = 0;
    EXPECT_TRUE(shard.step(start));
    EXPECT_EQ(shard.stack().echoed(), 4) << "the ring holds four";
    EXPECT_EQ(shard.stack().refused(), 2)
        << "the fifth filled it, the flush took nothing, so it and the sixth were refused";
    EXPECT_EQ(shard.context().counters().transmit_refused, 2);
    EXPECT_EQ(shard.queue().pending(), 4);
    EXPECT_TRUE(drain(client_).empty());

    throttled.accept = 64;
    EXPECT_TRUE(shard.step(start)) << "the flush at the end of the tick sent the ring";
    EXPECT_EQ(shard.queue().pending(), 0);
    EXPECT_EQ(shard.context().counters().frames_transmitted, 4);
    EXPECT_EQ(drain(client_).size(), 4);
}

TEST_F(ShardTest, StopUnwindsParkedTasksAndTheShardDrains) {
    aloe::runtime::Shard<aloe::fabric::Port, aloe::testing::EchoStack<aloe::fabric::Port>> shard{
        config_, server_, 0, start};
    {
        const aloe::runtime::ShardContext::Current current{shard.context()};
        shard.scheduler().spawn(parked(shard.scheduler()));
    }
    EXPECT_EQ(shard.context().scope().size(), 1);
    EXPECT_FALSE(shard.context().drained());

    {
        const aloe::runtime::ShardContext::Current current{shard.context()};
        shard.context().request_stop();
    }
    EXPECT_EQ(shard.context().counters().tasks_stopped, 1);
    EXPECT_TRUE(shard.context().drained());
}

TEST_F(ShardTest, AnEmptyStepIsIdle) {
    aloe::runtime::Shard<aloe::fabric::Port, aloe::testing::EchoStack<aloe::fabric::Port>> shard{
        config_, server_, 0, start};
    EXPECT_FALSE(shard.step(start));
    EXPECT_EQ(shard.context().counters().ticks, 1);
    EXPECT_EQ(shard.context().counters().idle_ticks, 1);
    EXPECT_EQ(shard.config().idle, aloe::runtime::IdlePolicy::Yield);
}

TEST_F(ShardTest, TickHooksSeeCurrentStampAndRunInOrder) {
    aloe::runtime::Shard<aloe::fabric::Port, HookStack> shard{config_, server_, 0, start};
    send(client_,
         aloe::frames::ethernet_frame(server, client, aloe::frames::ethertype_experimental, aloe::frames::pattern(30)));
    EXPECT_TRUE(shard.step(start + 7ms));
    EXPECT_EQ(shard.stack().order, (std::vector<std::string>{"receive", "tick", "ready", "flush"}));
    EXPECT_EQ(shard.stack().stamps, (std::vector<TimePoint>{start + 7ms, start + 7ms, start + 7ms}))
        << "on_receive sees the step's stamp through the context, before run_once";
    EXPECT_EQ(shard.stack().received, 1U);
    EXPECT_EQ(shard.stack().pending_at_ready, 0U) << "what on_tick queued left at the early flush";
    EXPECT_EQ(shard.stack().pending_at_flush, 1U) << "what the ready work queued is still in the ring at on_flush";
    EXPECT_EQ(shard.queue().pending(), 0U) << "and leaves at the final flush";
    EXPECT_EQ(drain(client_).size(), 2U);
}

TEST_F(ShardTest, EmptyReceiveTicksStillRunBothHooks) {
    aloe::runtime::Shard<aloe::fabric::Port, HookStack> shard{config_, server_, 0, start};
    EXPECT_TRUE(shard.step(start + 1ms)) << "the hook sent something";
    EXPECT_EQ(shard.stack().order, (std::vector<std::string>{"tick", "ready", "flush"}));
    EXPECT_EQ(shard.context().now(), start + 1ms);
}

TEST_F(ShardTest, WorkQueuedByReadyWorkWaitsForTheNextStep) {
    aloe::runtime::Shard<aloe::fabric::Port, HookStack> shard{config_, server_, 0, start};
    Counted second;
    shard.stack().follow_up = &second;
    std::ignore             = shard.step(start + 1ms);
    EXPECT_EQ(shard.stack().follow_up, nullptr) << "the ready work queued the second item";
    EXPECT_EQ(second.runs, 0) << "queued while the chain ran: it waits for the next step";
    EXPECT_EQ(shard.context().counters().work_run, 1U);
    std::ignore = shard.step(start + 2ms);
    EXPECT_EQ(second.runs, 1) << "one chain per step, never a recursive drain";
    EXPECT_EQ(shard.context().counters().work_run, 3U) << "the second item and the next step's ready work";
}

TEST_F(ShardTest, OptionalHooksAreIndependent) {
    aloe::runtime::Shard<aloe::fabric::Port, TickOnly> ticking{config_, server_, 0, start};
    aloe::runtime::Shard<aloe::fabric::Port, FlushOnly> flushing{config_, server_, 0, start};
    aloe::runtime::Shard<aloe::fabric::Port, aloe::testing::EchoStack<aloe::fabric::Port>> neither{
        config_, server_, 0, start};
    std::ignore = ticking.step(start);
    std::ignore = flushing.step(start);
    EXPECT_FALSE(neither.step(start)) << "unchanged: an empty step is idle";
    EXPECT_EQ(ticking.stack().ticks, 1);
    EXPECT_EQ(flushing.stack().flushes, 1);
    EXPECT_TRUE(ticking.context().siblings().empty()) << "a standalone shard has no siblings";
}
