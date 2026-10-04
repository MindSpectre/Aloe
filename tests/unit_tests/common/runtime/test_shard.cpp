#include <algorithm>
#include <aloe/fabric>
#include <aloe/runtime>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <echo_stack.hpp>
#include <frames.hpp>
#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using TimePoint = aloe::runtime::ShardContext::TimePoint;

    constexpr TimePoint start{};
    constexpr aloe::device::MacAddress server{0x02, 0, 0, 0, 0, 0x01};
    constexpr aloe::device::MacAddress client{0x02, 0, 0, 0, 0, 0x02};

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

        [[nodiscard]] aloe::device::MacAddress mac() const noexcept {
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
        ASSERT_TRUE(aloe::testing::fill(*packet, frame));
        std::array<aloe::fabric::Packet, 1> burst{std::move(*packet)};
        ASSERT_EQ(from.transmit(0, burst), 1);
    }

    std::vector<std::vector<std::byte>> drain(aloe::fabric::Port& port) {
        std::vector<std::vector<std::byte>> frames;
        std::array<aloe::fabric::Packet, 16> burst;
        for (std::size_t count = port.receive(0, burst); count > 0; count = port.receive(0, burst)) {
            for (std::size_t index = 0; index < count; ++index) {
                frames.push_back(aloe::testing::bytes_of(burst[index]));
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

}  // namespace

TEST_F(ShardTest, TheEchoRepliesInTheTickTheFrameArrived) {
    aloe::runtime::Shard<aloe::fabric::Port, aloe::testing::EchoStack<aloe::fabric::Port>> shard{
        config_, server_, 0, start};
    const auto frame = aloe::testing::ethernet_frame(
        server, client, aloe::testing::ethertype_experimental, aloe::testing::pattern(30));
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
        aloe::testing::ethernet_frame(server, client, aloe::testing::ethertype_experimental, aloe::testing::pattern(1));
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
    const auto frame = aloe::testing::ethernet_frame(
        server, client, aloe::testing::ethertype_experimental, aloe::testing::pattern(20));
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
