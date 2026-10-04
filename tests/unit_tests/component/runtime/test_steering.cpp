#include <aloe/fabric>
#include <aloe/runtime>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <echo_stack.hpp>
#include <frames.hpp>
#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;

    constexpr std::uint16_t queues = 4;
    constexpr std::size_t flows    = 256;
    constexpr auto patience        = 20s;
    constexpr aloe::device::MacAddress server{0x02, 0, 0, 0, 0, 0x01};
    constexpr aloe::device::MacAddress client{0x02, 0, 0, 0, 0, 0x02};
    constexpr aloe::device::Ipv4Address server_ip{10, 0, 0, 2};
    constexpr aloe::device::Ipv4Address client_ip{10, 0, 0, 1};
    constexpr std::size_t payload_offset = 14 + 20 + 8;  ///< Ethernet, IPv4 without options, UDP.

    [[nodiscard]] aloe::testing::Ipv4Spec spec_of(const std::size_t flow) {
        return {.destination_mac  = server,
                .source_mac       = client,
                .source           = client_ip,
                .destination      = server_ip,
                .source_port      = static_cast<std::uint16_t>(40000 + flow),
                .destination_port = 80,
                .protocol         = aloe::device::Ipv4Protocol::Udp};
    }

    /// Payload: the flow number, then four spare bytes the echo stamps the last two of.
    [[nodiscard]] std::vector<std::byte> frame_of(const std::size_t flow) {
        std::array<std::byte, 8> payload{};
        aloe::device::store_be32(std::span<std::byte>{payload}.first(4), static_cast<std::uint32_t>(flow));
        return aloe::testing::ipv4_frame(spec_of(flow), payload);
    }

}  // namespace

// Done-when 2, fabric half: every frame lands on the shard `queue_for` predicts, and comes back once.
TEST(Steering, EveryFrameIsAnsweredOnceByTheShardItsHashSelects) {
    aloe::fabric::Fabric fabric;
    auto& server_port = fabric.add_port({.mac = server, .queues = queues, .pool_size = 128});
    auto& client_port = fabric.add_port({.mac = client, .queues = 1, .pool_size = 64, .queue_depth = 4096});
    ASSERT_TRUE(server_port.steering().enabled);

    aloe::runtime::Runtime<aloe::fabric::Port, aloe::testing::EchoStack<aloe::fabric::Port>> runtime{
        {.shard = {.idle = aloe::runtime::IdlePolicy::Yield, .yield_after = 10}, .threads = {}, .thread_hook = {}},
        server_port
    };
    runtime.start();

    std::vector<std::uint16_t> expected(flows);
    std::array<std::size_t, queues> expected_per_shard{};
    for (std::size_t flow = 0; flow < flows; ++flow) {
        expected[flow] = aloe::device::queue_for(server_port.steering(), aloe::testing::flow_of(spec_of(flow)));
        ++expected_per_shard[expected[flow]];
        std::optional<aloe::fabric::Packet> packet;
        while (!(packet = client_port.allocate(0))) {
            std::this_thread::yield();
        }
        ASSERT_TRUE(aloe::testing::fill(*packet, frame_of(flow)));
        std::array<aloe::fabric::Packet, 1> burst{std::move(*packet)};
        ASSERT_EQ(client_port.transmit(0, burst), 1);
    }

    std::vector<int> seen(flows, 0);
    std::set<std::uint16_t> answering_shards;
    std::size_t received = 0;
    const auto deadline  = std::chrono::steady_clock::now() + patience;
    while (received < flows && std::chrono::steady_clock::now() < deadline) {
        std::array<aloe::fabric::Packet, 16> burst;
        const std::size_t count = client_port.receive(0, burst);
        if (count == 0) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        for (std::size_t index = 0; index < count; ++index) {
            const std::span<const std::byte> data = burst[index].data();
            const std::uint32_t flow              = aloe::device::load_be32(data.subspan(payload_offset, 4));
            ASSERT_LT(flow, flows);
            ++seen[flow];
            const std::uint16_t stamp = aloe::testing::stamp_of(data);
            EXPECT_EQ(stamp, expected[flow]) << "flow " << flow << " was answered by the wrong shard";
            answering_shards.insert(stamp);
            burst[index] = aloe::fabric::Packet{};
        }
        received += count;
    }
    runtime.stop();
    runtime.join();

    EXPECT_EQ(received, flows);
    for (std::size_t flow = 0; flow < flows; ++flow) {
        EXPECT_EQ(seen[flow], 1) << "flow " << flow;
    }
    EXPECT_EQ(answering_shards.size(), queues) << "the 256 flows spread over every shard; widen the port range if not";
    for (std::uint16_t shard = 0; shard < queues; ++shard) {
        EXPECT_EQ(runtime.counters(shard).frames_received, expected_per_shard[shard]) << "shard " << shard;
        EXPECT_EQ(runtime.counters(shard).frames_transmitted, expected_per_shard[shard]) << "shard " << shard;
    }
}
