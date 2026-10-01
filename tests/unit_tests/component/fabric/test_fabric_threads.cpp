#include <aloe/fabric>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <frames.hpp>
#include <gtest/gtest.h>

namespace {

    constexpr std::uint16_t queues            = 4;
    constexpr std::size_t producers           = 6;
    constexpr std::size_t frames_per_producer = 500;
    constexpr aloe::device::MacAddress server{0x02, 0, 0, 0, 0, 0xee};
    constexpr aloe::device::Ipv4Address server_ip{10, 0, 0, 2};

    aloe::device::MacAddress producer_mac(std::size_t producer) {
        return {0x02, 0, 0, 0, 1, static_cast<std::uint8_t>(producer)};
    }

    /// A frame whose payload names its producer and sequence number, on a port that spreads the hash.
    std::vector<std::byte> frame_of(std::size_t producer, std::size_t sequence) {
        std::array<std::byte, 8> payload{};
        aloe::device::store_be32(std::span<std::byte>{payload}.first(4), static_cast<std::uint32_t>(producer));
        aloe::device::store_be32(std::span<std::byte>{payload}.subspan(4, 4), static_cast<std::uint32_t>(sequence));
        return aloe::testing::ipv4_frame(
            {
                .destination_mac  = server,
                .source_mac       = producer_mac(producer),
                .source           = aloe::device::Ipv4Address{10, 0, 1, static_cast<std::uint8_t>(producer)},
                .destination      = server_ip,
                .source_port      = static_cast<std::uint16_t>(40000 + sequence),
                .destination_port = 80,
                .protocol         = aloe::device::Ipv4Protocol::Udp
        },
            payload);
    }

}  // namespace

// The test the tsan preset exists for: every producer transmits from its own thread into one
// port whose queues are drained by their own threads, and every frame arrives exactly once.
TEST(FabricThreads, FramesFromManyThreadsArriveExactlyOnce) {
    aloe::fabric::Fabric fabric;
    auto& sink = fabric.add_port({.mac = server, .queues = queues, .pool_size = 64, .queue_depth = 100000});
    std::vector<aloe::fabric::Port*> sources;
    sources.reserve(producers);
    for (std::size_t producer = 0; producer < producers; ++producer) {
        sources.push_back(&fabric.add_port({.mac = producer_mac(producer), .pool_size = 64}));
    }

    std::atomic<std::size_t> producers_done{0};
    std::mutex seen_mutex;
    std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
    std::size_t received_total = 0;

    std::vector<std::jthread> consumers;
    consumers.reserve(queues);
    for (std::uint16_t queue = 0; queue < queues; ++queue) {
        consumers.emplace_back([&, queue] {
            std::array<aloe::fabric::Packet, 16> burst;
            while (true) {
                // Read before receiving: every delivery happens before its producer counts itself
                // done, so an empty queue after the producers finished stays empty.
                const bool done         = producers_done.load() == producers;
                const std::size_t count = sink.receive(queue, burst);
                if (count == 0) {
                    if (done) {
                        break;
                    }
                    std::this_thread::yield();
                    continue;
                }
                const std::lock_guard lock{seen_mutex};
                for (std::size_t index = 0; index < count; ++index) {
                    const auto payload = burst[index].data().last(8);
                    EXPECT_TRUE(seen.emplace(aloe::device::load_be32(payload.first(4)),
                                             aloe::device::load_be32(payload.subspan(4, 4)))
                                    .second)
                        << "a frame arrived twice";
                    burst[index] = aloe::fabric::Packet{};
                }
                received_total += count;
            }
        });
    }

    {
        std::vector<std::jthread> threads;
        threads.reserve(producers);
        for (std::size_t producer = 0; producer < producers; ++producer) {
            threads.emplace_back([&, producer] {
                aloe::fabric::Port& source = *sources[producer];
                const auto send_all        = [&] {
                    for (std::size_t sequence = 0; sequence < frames_per_producer; ++sequence) {
                        std::optional<aloe::fabric::Packet> packet;
                        while (!(packet = source.allocate(0))) {
                            std::this_thread::yield();
                        }
                        ASSERT_TRUE(aloe::testing::fill(*packet, frame_of(producer, sequence)));
                        std::array<aloe::fabric::Packet, 1> burst{std::move(*packet)};
                        ASSERT_EQ(source.transmit(0, burst), 1);
                    }
                };
                send_all();
                ++producers_done;  // even after a failed assertion, so the consumers stop
            });
        }
    }
    consumers.clear();

    EXPECT_EQ(received_total, producers * frames_per_producer);
    EXPECT_EQ(seen.size(), producers * frames_per_producer);
    std::uint64_t counted = 0;
    for (std::uint16_t queue = 0; queue < queues; ++queue) {
        const aloe::device::QueueCounters counters = sink.counters(queue);
        EXPECT_EQ(counters.dropped, 0);
        EXPECT_GT(counters.received, 0) << "queue " << queue << " saw nothing: the hash did not spread";
        counted += counters.received;
    }
    EXPECT_EQ(counted, producers * frames_per_producer);
    for (std::size_t producer = 0; producer < producers; ++producer) {
        EXPECT_EQ(sources[producer]->counters(0).transmitted, frames_per_producer) << "producer " << producer;
        for (std::size_t sequence = 0; sequence < frames_per_producer; ++sequence) {
            EXPECT_TRUE(seen.contains({static_cast<std::uint32_t>(producer), static_cast<std::uint32_t>(sequence)}));
        }
    }
}
