#include <algorithm>
#include <aloe/device>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <net_fixture.hpp>
#include <tcp_test_device.hpp>

// A four-queue port with one brick per queue, a peer port opposite: every connection, inbound or outbound, is
// served by the queue the card's hash selects, and nothing lands on a brick that does not know it.
namespace {

    using namespace std::chrono_literals;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpSequence;
    using Ipv4      = aloe::net::Ipv4<aloe::fabric::Port>;
    using Tcp       = aloe::tcp::Stack<Ipv4>;
    using TimePoint = aloe::core::TimePoint;

    constexpr std::uint16_t queues = 4;
    constexpr std::size_t flows    = 24;
    constexpr aloe::wire::MacAddress stack_mac{0x02, 0, 0, 0, 0, 0x01};
    constexpr aloe::wire::MacAddress peer_mac{0x02, 0, 0, 0, 0, 0x02};
    constexpr aloe::wire::Ipv4Address stack_ip{10, 0, 0, 2};
    constexpr aloe::wire::Ipv4Address peer_ip{10, 0, 0, 1};

    struct Brick {
        Brick(aloe::fabric::Port& port, const std::uint16_t index)
            : queue{
                  port, index, 64, counters
        },
              ip{queue, {.address = stack_ip, .prefix = 24}}, tcp{ip, wheel, {}} {
            ip.learn(peer_ip, peer_mac, TimePoint{});
        }

        aloe::loop::ShardCounters counters;
        aloe::loop::ShardQueue<aloe::fabric::Port> queue;
        Ipv4 ip;
        aloe::loop::TimerWheel wheel{1ms, TimePoint{}};
        Tcp tcp;
        std::vector<aloe::fabric::Packet> burst = std::vector<aloe::fabric::Packet>(64);

        void tick(const TimePoint now) {
            const std::size_t received = queue.receive(burst);
            ip.process(std::span<aloe::fabric::Packet>{burst}.first(received), now);
            tcp.process(ip.received(aloe::wire::Ipv4Protocol::Tcp), now);
            std::ignore = wheel.advance(now);
            tcp.flush(now);
            std::ignore = queue.flush();
        }
    };

    [[nodiscard]] aloe::frames::TcpSpec peer_spec(const std::uint16_t peer_port) {
        return {.destination_mac  = stack_mac,
                .source_mac       = peer_mac,
                .source           = peer_ip,
                .destination      = stack_ip,
                .source_port      = peer_port,
                .destination_port = 7,
                .mss              = 1460};
    }

    class TcpPlacement : public ::testing::Test {
    protected:
        aloe::fabric::Fabric fabric_;
        aloe::fabric::Port& port_ = fabric_.add_port({.mac = stack_mac, .queues = queues, .pool_size = 128});
        aloe::fabric::Port& peer_ = fabric_.add_port({.mac = peer_mac, .pool_size = 128, .queue_depth = 4096});
        std::vector<std::unique_ptr<Brick>> bricks_;
        TimePoint now_{};

        void SetUp() override {
            ASSERT_TRUE(port_.steering().enabled);
            for (std::uint16_t index = 0; index < queues; ++index) {
                bricks_.push_back(std::make_unique<Brick>(port_, index));
                ASSERT_TRUE(bricks_.back()->tcp.listen(7).has_value());
            }
        }

        void tick_all() {
            now_ += 1ms;
            for (auto& brick : bricks_) {
                brick->tick(now_);
            }
        }

        void inject(const std::span<const std::byte> frame) {
            auto packet = peer_.allocate(0);
            ASSERT_TRUE(packet.has_value());
            ASSERT_TRUE(aloe::frames::fill(*packet, frame));
            std::array<aloe::fabric::Packet, 1> out{std::move(*packet)};
            ASSERT_EQ(peer_.transmit(0, out), 1U);
        }

        [[nodiscard]] std::vector<aloe::frames::ParsedFrame> peer_received() {
            std::vector<aloe::frames::ParsedFrame> frames;
            std::array<aloe::fabric::Packet, 16> out;
            for (std::size_t count = peer_.receive(0, out); count > 0; count = peer_.receive(0, out)) {
                for (std::size_t index = 0; index < count; ++index) {
                    frames.push_back(aloe::frames::parse_frame(aloe::frames::bytes_of(out[index])).value());
                    out[index] = aloe::fabric::Packet{};
                }
            }
            return frames;
        }

        /// The queue the card delivers a segment from the peer at `peer_port` to our `local_port` to.
        [[nodiscard]] std::uint16_t expected_queue(const std::uint16_t peer_port,
                                                   const std::uint16_t local_port) const {
            return aloe::device::queue_for(port_.steering(),
                                           {.source           = peer_ip,
                                            .destination      = stack_ip,
                                            .source_port      = peer_port,
                                            .destination_port = local_port,
                                            .protocol         = aloe::wire::Ipv4Protocol::Tcp});
        }

        [[nodiscard]] std::uint64_t stray_drops() const {
            std::uint64_t drops = 0;
            for (const auto& brick : bricks_) {
                drops += brick->tcp.counters().dropped_no_connection + brick->tcp.counters().dropped_unexpected;
            }
            return drops;
        }
    };

}  // namespace

TEST_F(TcpPlacement, InboundSynsAreTakenByTheHashedQueue) {
    std::vector<aloe::frames::TcpPeer> peers;
    for (std::size_t flow = 0; flow < flows; ++flow) {
        peers.emplace_back(peer_spec(static_cast<std::uint16_t>(40000 + flow)), TcpSequence{1000U});
        inject(peers.back().syn());
    }
    tick_all();
    const auto syn_acks = peer_received();
    ASSERT_EQ(syn_acks.size(), flows);
    std::vector<std::uint16_t> owners(flows, queues);
    for (const auto& frame : syn_acks) {
        const std::size_t flow = frame.tcp->destination_port - 40000U;
        peers[flow].see(frame);
        inject(peers[flow].ack());
    }
    tick_all();
    std::size_t accepted = 0;
    for (std::uint16_t index = 0; index < queues; ++index) {
        while (const auto event = bricks_[index]->tcp.poll_event()) {
            ASSERT_TRUE(event->events.accepted());
            const std::size_t flow = event->connection->remote().port - 40000U;
            owners[flow]           = index;
            ++accepted;
        }
    }
    EXPECT_EQ(accepted, flows);
    for (std::size_t flow = 0; flow < flows; ++flow) {
        EXPECT_EQ(owners[flow], expected_queue(static_cast<std::uint16_t>(40000 + flow), 7)) << "flow " << flow;
    }
    std::set<std::uint16_t> used(owners.begin(), owners.end());
    EXPECT_EQ(used.size(), queues) << "24 flows exercise every queue";
    EXPECT_EQ(stray_drops(), 0U);
}

TEST_F(TcpPlacement, OutboundConnectsChoosePortsWhoseRepliesComeBackToTheirQueue) {
    std::vector<std::pair<std::uint16_t, std::uint16_t>> opened;  // queue, local port
    for (std::size_t flow = 0; flow < flows; ++flow) {
        const auto index     = static_cast<std::uint16_t>(flow % queues);
        const auto connected = bricks_[index]->tcp.connect({.address = peer_ip, .port = 7}, now_);
        ASSERT_TRUE(connected.has_value()) << "flow " << flow;
        opened.emplace_back(index, (*connected)->local().port);
        EXPECT_EQ(expected_queue(7, (*connected)->local().port), index) << "the port was chosen for this queue";
    }
    tick_all();
    const auto syns = peer_received();
    ASSERT_EQ(syns.size(), flows);
    std::vector<aloe::frames::TcpPeer> peers;
    for (const auto& syn : syns) {
        aloe::frames::TcpPeer peer{peer_spec(7), TcpSequence{2000U}};
        inject(peer.syn_ack(syn));
        peers.push_back(peer);
    }
    tick_all();
    std::size_t connected = 0;
    for (std::uint16_t index = 0; index < queues; ++index) {
        while (const auto event = bricks_[index]->tcp.poll_event()) {
            EXPECT_TRUE(event->events.connected());
            const auto found = std::ranges::find(opened, std::pair{index, event->connection->local().port});
            EXPECT_NE(found, opened.end()) << "the reply reached the brick that opened";
            ++connected;
        }
    }
    EXPECT_EQ(connected, flows);
    EXPECT_EQ(stray_drops(), 0U);
    EXPECT_EQ(peer_received().size(), flows) << "one handshake ACK per connection";
}

TEST_F(TcpPlacement, AddressesOnlyRssIsUnplaceableOffItsQueue) {
    aloe::testing::TcpTestDevice device{port_};
    aloe::device::RssDescription addresses_only = aloe::device::round_robin_rss(queues);
    addresses_only.types.ipv4_tcp               = false;  // addresses only: one queue for every port
    device.steering_override                    = addresses_only;
    const std::uint16_t picked                  = aloe::device::queue_for(
        addresses_only, {.source = peer_ip, .destination = stack_ip, .protocol = aloe::wire::Ipv4Protocol::Tcp});
    const auto other = static_cast<std::uint16_t>((picked + 1) % queues);
    aloe::loop::ShardCounters counters;
    aloe::loop::ShardQueue<aloe::testing::TcpTestDevice> queue{device, other, 16, counters};
    aloe::net::Ipv4<aloe::testing::TcpTestDevice> ip{
        queue, {.address = stack_ip, .prefix = 24}
    };
    aloe::loop::TimerWheel wheel{1ms, TimePoint{}};
    aloe::tcp::Stack<aloe::net::Ipv4<aloe::testing::TcpTestDevice>> tcp{ip, wheel, {}};
    const auto failed = tcp.connect({.address = peer_ip, .port = 7}, now_);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error(), aloe::tcp::ConnectError::Unplaceable);
    EXPECT_EQ(tcp.table_size(), 0U);
    aloe::loop::ShardQueue<aloe::testing::TcpTestDevice> right_queue{device, picked, 16, counters};
    aloe::net::Ipv4<aloe::testing::TcpTestDevice> right_ip{
        right_queue, {.address = stack_ip, .prefix = 24}
    };
    aloe::tcp::Stack<aloe::net::Ipv4<aloe::testing::TcpTestDevice>> right{right_ip, wheel, {}};
    right_ip.learn(peer_ip, peer_mac, now_);
    EXPECT_TRUE(right.connect({.address = peer_ip, .port = 7}, now_).has_value())
        << "on the picked queue any port does";
}

TEST_F(TcpPlacement, DisabledSteeringCannotReachNonzeroQueue) {
    aloe::testing::TcpTestDevice device{port_};
    device.steering_override = aloe::device::RssDescription{};  // off: every frame lands on queue 0
    aloe::loop::ShardCounters counters;
    aloe::loop::ShardQueue<aloe::testing::TcpTestDevice> queue1{device, 1, 16, counters};
    aloe::net::Ipv4<aloe::testing::TcpTestDevice> ip1{
        queue1, {.address = stack_ip, .prefix = 24}
    };
    aloe::loop::TimerWheel wheel{1ms, TimePoint{}};
    aloe::tcp::Stack<aloe::net::Ipv4<aloe::testing::TcpTestDevice>> on_one{ip1, wheel, {}};
    const auto failed = on_one.connect({.address = peer_ip, .port = 7}, now_);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error(), aloe::tcp::ConnectError::Unplaceable);
    aloe::loop::ShardQueue<aloe::testing::TcpTestDevice> queue0{device, 0, 16, counters};
    aloe::net::Ipv4<aloe::testing::TcpTestDevice> ip0{
        queue0, {.address = stack_ip, .prefix = 24}
    };
    aloe::tcp::Stack<aloe::net::Ipv4<aloe::testing::TcpTestDevice>> on_zero{ip0, wheel, {}};
    ip0.learn(peer_ip, peer_mac, now_);
    EXPECT_TRUE(on_zero.connect({.address = peer_ip, .port = 7}, now_).has_value());
}

TEST_F(TcpPlacement, CursorWrapsSkipsListenersAndOccupiedTuplesThenNoPort) {
    aloe::fabric::Port& single = fabric_.add_port({
        .mac = {0x02, 0, 0, 0, 0, 0x03},
          .pool_size = 64
    });
    aloe::loop::ShardCounters counters;
    aloe::loop::ShardQueue<aloe::fabric::Port> queue{single, 0, 16, counters};
    Ipv4 ip{
        queue, {.address = {10, 0, 0, 3}, .prefix = 24}
    };
    ip.learn(peer_ip, peer_mac, now_);
    aloe::loop::TimerWheel wheel{1ms, TimePoint{}};
    Tcp tcp{
        ip, wheel, {.ephemeral_first = 40000, .ephemeral_last = 40002}
    };
    ASSERT_TRUE(tcp.listen(40001).has_value());
    const auto first = tcp.connect({.address = peer_ip, .port = 7}, now_);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ((*first)->local().port, 40000U);
    const auto second = tcp.connect({.address = peer_ip, .port = 7}, now_);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ((*second)->local().port, 40002U) << "40001 is a listener";
    const auto third = tcp.connect({.address = peer_ip, .port = 7}, now_);
    ASSERT_FALSE(third.has_value());
    EXPECT_EQ(third.error(), aloe::tcp::ConnectError::NoPort) << "the cursor wrapped to its start";
    const auto elsewhere = tcp.connect({.address = peer_ip, .port = 8}, now_);
    ASSERT_TRUE(elsewhere.has_value()) << "another peer port: the tuples are free";
    EXPECT_EQ((*elsewhere)->local().port, 40000U);
    (*first)->release();
    const auto reused = tcp.connect({.address = peer_ip, .port = 7}, now_);
    ASSERT_TRUE(reused.has_value());
    EXPECT_EQ((*reused)->local().port, 40000U) << "freed and taken again";
    Tcp tiny{
        ip, wheel, {.connections = 1, .ephemeral_first = 50000, .ephemeral_last = 50001}
    };
    ASSERT_TRUE(tiny.connect({.address = peer_ip, .port = 7}, now_).has_value());
    const auto full = tiny.connect({.address = peer_ip, .port = 7}, now_);
    ASSERT_FALSE(full.has_value());
    EXPECT_EQ(full.error(), aloe::tcp::ConnectError::TableFull);
    for (auto* c : {*second, *elsewhere, *reused}) {
        c->release();
    }
    std::ignore = queue.flush();
}
