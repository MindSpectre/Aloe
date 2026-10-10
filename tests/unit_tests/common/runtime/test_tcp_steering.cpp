#include <algorithm>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/runtime>
#include <aloe/stream>
#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

// Four shards of TcpStack serve the task echo; a client runtime opens connections to them; every reply is
// stamped with the serving shard, which must be the queue the card's hash selects. Then a connect through a
// gateway only shard 0 can learn, and stop racing the ARP forwards. The tsan preset exists for this file.
namespace {

    using namespace std::chrono_literals;
    using Stack   = aloe::runtime::TcpStack<aloe::fabric::Port>;
    using Runtime = aloe::runtime::Runtime<aloe::fabric::Port, Stack>;
    using Stream  = Stack::StreamType;

    constexpr std::uint16_t queues    = 4;
    constexpr std::size_t connections = 24;
    constexpr auto patience           = 20s;
    constexpr aloe::wire::MacAddress server_mac{0x02, 0, 0, 0, 0, 0x01};
    constexpr aloe::wire::MacAddress client_mac{0x02, 0, 0, 0, 0, 0x02};
    constexpr aloe::wire::MacAddress gateway_mac{0x02, 0, 0, 0, 0, 0xfe};
    constexpr aloe::wire::Ipv4Address server_ip{10, 0, 0, 2};
    constexpr aloe::wire::Ipv4Address client_ip{10, 0, 0, 1};
    constexpr aloe::wire::Ipv4Address gateway_ip{10, 0, 0, 254};
    constexpr aloe::wire::Ipv4Address far_ip{192, 168, 7, 7};

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

    /// Echoes what it reads with the serving shard's index appended, then closes.
    aloe::runtime::task<void> echo(Stream stream, const std::uint16_t index) {
        const auto readable = co_await stream.readable(4);
        if (!readable) {
            co_return;
        }
        std::array<std::byte, 5> reply{};
        std::ranges::copy(stream.unread().front().first(4), reply.begin());
        reply[4] = static_cast<std::byte>(index);
        stream.consume(4);
        if (!co_await stream.send(reply)) {
            co_return;
        }
        std::ignore = co_await stream.close();
    }

    aloe::runtime::task<void>
    serve(Stack::StreamsType* streams, aloe::runtime::Scheduler scheduler, const std::uint16_t index) {
        for (;;) {
            auto stream = co_await streams->accept(7);
            if (!stream) {
                break;
            }
            scheduler.spawn(echo(std::move(*stream), index));
        }
    }

    /// One connect through the gateway, from the last shard; reports whether it reached Established.
    aloe::runtime::task<void> connect_far(Stack::StreamsType* streams, std::atomic<int>* done, std::atomic<bool>* ok) {
        auto stream = co_await streams->connect({.address = far_ip, .port = 7});
        ok->store(stream.has_value());
        done->fetch_add(1);
        if (stream) {
            stream->release();
        }
    }

    /// Opens `connections` one after another and checks each stamp against `queue_for`.
    aloe::runtime::task<void> client(Stack::StreamsType* streams,
                                     const aloe::device::RssDescription* steering,
                                     std::atomic<int>* done,
                                     std::atomic<int>* wrong) {
        for (std::size_t flow = 0; flow < connections; ++flow) {
            auto stream = co_await streams->connect({.address = server_ip, .port = 7});
            if (!stream) {
                wrong->fetch_add(100);
                break;
            }
            const std::array<std::byte, 4> ping{std::byte{'p'}, std::byte{'i'}, std::byte{'n'}, std::byte{'g'}};
            if (!co_await stream->send(ping)) {
                wrong->fetch_add(100);
                break;
            }
            const auto readable = co_await stream->readable(5);
            if (!readable) {
                wrong->fetch_add(100);
                break;
            }
            const std::uint16_t expected = aloe::device::queue_for(*steering,
                                                                   {.source           = client_ip,
                                                                    .destination      = server_ip,
                                                                    .source_port      = stream->local().port,
                                                                    .destination_port = 7,
                                                                    .protocol         = aloe::wire::Ipv4Protocol::Tcp});
            if (std::to_integer<std::uint16_t>(stream->unread().front()[4]) != expected) {
                wrong->fetch_add(1);
            }
            stream->consume(5);
            std::ignore = co_await stream->close();
            done->fetch_add(1);
        }
    }

}  // namespace

TEST(TcpSteering, AllConnectionsStayOnTheirShard) {
    aloe::fabric::Fabric fabric;
    auto& server_port = fabric.add_port({.mac = server_mac, .queues = queues, .pool_size = 256});
    auto& client_port = fabric.add_port({.mac = client_mac, .queues = 1, .pool_size = 256, .queue_depth = 4096});
    std::atomic<int> done{0};
    std::atomic<int> wrong{0};
    const aloe::runtime::RuntimeConfig config{
        .shard = {.idle = aloe::runtime::IdlePolicy::Yield, .yield_after = 10}
    };
    Runtime server{
        config, server_port, aloe::net::Ipv4Config{.address = server_ip, .prefix = 24},
          aloe::tcp::TcpConfig{}
    };
    Runtime client_runtime{
        config, client_port, aloe::net::Ipv4Config{.address = client_ip, .prefix = 24},
          aloe::tcp::TcpConfig{}
    };
    for (std::uint16_t index = 0; index < queues; ++index) {
        server.shard(index).stack().ip().learn(client_ip, client_mac, std::chrono::steady_clock::now());
    }
    client_runtime.shard(0).stack().ip().learn(server_ip, server_mac, std::chrono::steady_clock::now());
    server.start();
    client_runtime.start();
    for (std::uint16_t index = 0; index < queues; ++index) {
        server.spawn(index, serve(&server.shard(index).stack().streams(), server.scheduler(index), index));
    }
    client_runtime.spawn(0, client(&client_runtime.shard(0).stack().streams(), &server_port.steering(), &done, &wrong));
    EXPECT_TRUE(eventually([&] { return done.load() == connections || wrong.load() >= 100; }));
    client_runtime.stop();
    server.stop();
    client_runtime.join();
    server.join();
    EXPECT_EQ(done.load(), connections);
    EXPECT_EQ(wrong.load(), 0) << "every reply came from the queue the hash selects";
    std::uint64_t accepted = 0;
    for (std::uint16_t index = 0; index < queues; ++index) {
        const auto& counters  = server.shard(index).stack().tcp().counters();
        accepted             += counters.connections_accepted;
        EXPECT_EQ(counters.dropped_no_connection, 0U) << "shard " << index;
        EXPECT_EQ(counters.dropped_unexpected, 0U) << "shard " << index;
    }
    EXPECT_EQ(accepted, connections);
    EXPECT_EQ(client_runtime.shard(0).stack().tcp().counters().dropped_no_connection, 0U);
}

TEST(TcpSteering, GatewayKnownOnlyByQueueZero) {
    aloe::fabric::Fabric fabric;
    auto& server_port  = fabric.add_port({.mac = server_mac, .queues = queues, .pool_size = 256});
    auto& gateway_port = fabric.add_port({.mac = gateway_mac, .queues = 1, .pool_size = 128, .queue_depth = 4096});
    std::atomic<int> completions{0};
    std::atomic<bool> connected{false};
    std::atomic<bool> stop_gateway{false};
    const aloe::runtime::RuntimeConfig config{
        .shard = {.idle = aloe::runtime::IdlePolicy::Yield, .yield_after = 10}
    };
    Runtime server{
        config,
        server_port,
        aloe::net::Ipv4Config{.address = server_ip, .prefix = 24, .gateway = gateway_ip},
        aloe::tcp::TcpConfig{}
    };
    // The gateway, scripted on this thread: answers the ARP request (which lands on queue 0 of the server, being
    // ARP) and the SYN from far_ip behind it, so the SYN-ACK hashes back to the shard that chose the port.
    std::jthread gateway{[&] {
        std::vector<aloe::fabric::Packet> burst(16);
        while (!stop_gateway.load()) {
            const std::size_t count = gateway_port.receive(0, burst);
            for (std::size_t index = 0; index < count; ++index) {
                const auto frame = aloe::frames::parse_frame(aloe::frames::bytes_of(burst[index]));
                burst[index]     = aloe::fabric::Packet{};
                if (!frame) {
                    continue;
                }
                std::vector<std::byte> reply;
                if (frame->arp && frame->arp->operation == aloe::wire::ArpOperation::Request &&
                    frame->arp->target_ip == gateway_ip) {
                    reply = aloe::frames::arp_frame(
                        aloe::frames::arp_reply(gateway_mac, gateway_ip, frame->arp->sender_mac, frame->arp->sender_ip),
                        frame->arp->sender_mac);
                } else if (frame->tcp && frame->tcp->flags == aloe::wire::TcpFlags{aloe::wire::TcpFlag::Syn}) {
                    aloe::frames::TcpPeer far{
                        {.destination_mac  = server_mac,
                         .source_mac       = gateway_mac,
                         .source           = far_ip,
                         .destination      = server_ip,
                         .source_port      = 7,
                         .destination_port = frame->tcp->source_port,
                         .mss              = 1460},
                        aloe::wire::TcpSequence{5000U}
                    };
                    reply = far.syn_ack(*frame);
                } else {
                    continue;
                }
                auto packet = gateway_port.allocate(0);
                if (packet && aloe::frames::fill(*packet, reply)) {
                    std::array<aloe::fabric::Packet, 1> out{std::move(*packet)};
                    std::ignore = gateway_port.transmit(0, out);
                }
            }
            if (count == 0) {
                std::this_thread::sleep_for(1ms);
            }
        }
    }};
    server.start();
    server.spawn(queues - 1, connect_far(&server.shard(queues - 1).stack().streams(), &completions, &connected));
    EXPECT_TRUE(eventually([&] { return completions.load() == 1; }));
    server.stop();
    server.join();
    stop_gateway.store(true);
    gateway.join();
    EXPECT_TRUE(connected.load());
    EXPECT_GE(server.shard(queues - 1).stack().tcp().counters().send_unresolved, 1U) << "the first SYN waited for ARP";
    EXPECT_EQ(server.shard(0).stack().ip().counters().resolutions, 1U) << "queue 0 learned the gateway from the wire";
    EXPECT_GE(server.counters(queues - 1).inbox_received, 3U)
        << "shard 3 learned the gateway from shard 0: its spawn, its stop and at least one forward";
}

TEST(TcpSteering, StopRacesArpForward) {
    // Shard 0 receives every ARP reply; with four queues it learns them unsolicited and forwards each resolution
    // to the three siblings while the stop closes them. Every forward is accepted and run by a sibling, rejected
    // and freed by shard 0, or never allocated and counted: the counters on both sides account for every one,
    // ASan sees no leak and TSan no race between post_control and the final drain.
    std::uint64_t attempted      = 0;
    std::uint64_t accepted       = 0;
    std::uint64_t total_rejected = 0;
    for (int round = 0; round < 20; ++round) {
        aloe::fabric::Fabric fabric;
        auto& server_port = fabric.add_port({.mac = server_mac, .queues = queues, .pool_size = 128});
        auto& peer_port   = fabric.add_port({.mac = client_mac, .queues = 1, .pool_size = 128});
        const aloe::runtime::RuntimeConfig config{
            .shard = {.idle = aloe::runtime::IdlePolicy::Yield, .yield_after = 1}
        };
        Runtime server{
            config, server_port, aloe::net::Ipv4Config{.address = server_ip, .prefix = 24},
              aloe::tcp::TcpConfig{}
        };
        server.start();
        std::jthread replies{[&] {
            for (int i = 0; i < 200; ++i) {
                auto packet = peer_port.allocate(0);
                if (!packet) {
                    continue;
                }
                const aloe::wire::Ipv4Address who{10, 0, 0, static_cast<std::uint8_t>(10 + (i % 100))};
                if (aloe::frames::fill(
                        *packet,
                        aloe::frames::arp_frame(aloe::frames::arp_reply(client_mac, who, server_mac, server_ip),
                                                server_mac))) {
                    std::array<aloe::fabric::Packet, 1> out{std::move(*packet)};
                    std::ignore = peer_port.transmit(0, out);
                }
                if (i == 50 + round) {
                    server.stop();  // while forwards are in flight
                }
            }
        }};
        replies.join();
        server.stop();
        server.join();
        // Each round builds a fresh runtime, so every counter read here is that round's.
        const std::uint64_t learned  = server.shard(0).stack().ip().counters().resolutions;
        const std::uint64_t dropped  = server.shard(0).stack().forwards_dropped();
        const std::uint64_t rejected = server.shard(0).stack().forwards_rejected();
        EXPECT_EQ(server.counters(0).inbox_received, 1U) << "shard 0 receives its stop and no forward";
        std::uint64_t taken_this_round = 0;
        for (std::uint16_t index = 1; index < queues; ++index) {
            EXPECT_EQ(server.shard(index).stack().ip().counters().resolutions, 0U) << "ARP lands on queue 0 only";
            EXPECT_EQ(server.shard(index).stack().forwards_dropped(), 0U);
            EXPECT_EQ(server.shard(index).stack().forwards_rejected(), 0U);
            // A node a sibling accepted went through its inbox and ran before it drained; its stop is the extra one.
            const std::uint64_t taken = server.counters(index).inbox_received - 1;
            EXPECT_LE(taken, learned) << "shard " << index << " ran a forward nobody sent";
            taken_this_round += taken;
        }
        EXPECT_EQ(dropped, 0U);
        EXPECT_EQ(taken_this_round + rejected + dropped, learned * (queues - 1U))
            << "round " << round << ": every forward was accepted and run, rejected and freed, or never allocated";
        attempted      += learned * (queues - 1U);
        accepted       += taken_this_round;
        total_rejected += rejected;
    }
    EXPECT_GT(attempted, 0U) << "the race needs forwards in flight";
    ::testing::Test::RecordProperty("forwards_attempted", std::to_string(attempted));
    ::testing::Test::RecordProperty("forwards_accepted", std::to_string(accepted));
    ::testing::Test::RecordProperty("forwards_rejected", std::to_string(total_rejected));
}
