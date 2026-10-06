#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/wire>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

    constexpr aloe::wire::MacAddress alpha{0x02, 0, 0, 0, 0, 0x0a};
    constexpr aloe::wire::MacAddress beta{0x02, 0, 0, 0, 0, 0x0b};
    constexpr aloe::wire::MacAddress gamma{0x02, 0, 0, 0, 0, 0x0c};
    constexpr aloe::wire::MacAddress nobody{0x02, 0, 0, 0, 0, 0xff};

    aloe::fabric::PortConfig port(const aloe::wire::MacAddress& mac) {
        return {.mac = mac, .queues = 1, .pool_size = 16, .queue_depth = 4};
    }

    /// Transmits `frame` from `source` on queue 0.
    void send(aloe::fabric::Port& source, std::span<const std::byte> frame) {
        auto packet = source.allocate(0);
        ASSERT_TRUE(packet.has_value());
        ASSERT_TRUE(aloe::frames::fill(*packet, frame));
        std::array<aloe::fabric::Packet, 1> burst{std::move(*packet)};
        ASSERT_EQ(source.transmit(0, burst), 1);
        EXPECT_TRUE(burst[0].empty()) << "an accepted packet is moved from";
    }

    /// Everything pending on `queue` of `port`, as frames.
    std::vector<std::vector<std::byte>> drain(aloe::fabric::Port& port, std::uint16_t queue = 0) {
        std::vector<std::vector<std::byte>> frames;
        std::array<aloe::fabric::Packet, 8> burst;
        for (std::size_t count = port.receive(queue, burst); count > 0; count = port.receive(queue, burst)) {
            for (std::size_t index = 0; index < count; ++index) {
                frames.push_back(aloe::frames::bytes_of(burst[index]));
                burst[index] = aloe::fabric::Packet{};
            }
        }
        return frames;
    }

}  // namespace

TEST(FabricDelivery, UnicastReachesThePortWithThatMacOnly) {
    aloe::fabric::Fabric fabric;
    auto& a = fabric.add_port(port(alpha));
    auto& b = fabric.add_port(port(beta));
    auto& c = fabric.add_port(port(gamma));

    const auto frame =
        aloe::frames::ethernet_frame(beta, alpha, aloe::frames::ethertype_experimental, aloe::frames::pattern(30));
    send(a, frame);

    EXPECT_EQ(drain(b), std::vector<std::vector<std::byte>>{frame});
    EXPECT_TRUE(drain(a).empty());
    EXPECT_TRUE(drain(c).empty());
    EXPECT_EQ(a.counters(0).transmitted, 1);
    EXPECT_EQ(b.counters(0).received, 1);
    EXPECT_EQ(c.counters(0).received, 0);
}

TEST(FabricDelivery, BroadcastAndMulticastReachEveryOtherPort) {
    aloe::fabric::Fabric fabric;
    auto& a = fabric.add_port(port(alpha));
    auto& b = fabric.add_port(port(beta));
    auto& c = fabric.add_port(port(gamma));

    const auto broadcast = aloe::frames::ethernet_frame(
        aloe::wire::MacAddress::broadcast(), alpha, aloe::frames::ethertype_experimental, aloe::frames::pattern(30));
    constexpr aloe::wire::MacAddress group{0x01, 0x00, 0x5e, 0, 0, 1};
    const auto multicast =
        aloe::frames::ethernet_frame(group, alpha, aloe::frames::ethertype_experimental, aloe::frames::pattern(30, 1));
    send(a, broadcast);
    send(a, multicast);

    EXPECT_TRUE(drain(a).empty()) << "the sender does not hear its own broadcast";
    EXPECT_EQ(drain(b), (std::vector<std::vector<std::byte>>{broadcast, multicast}));
    EXPECT_EQ(drain(c), (std::vector<std::vector<std::byte>>{broadcast, multicast}));
}

TEST(FabricDelivery, AFrameToOwnMacComesBack) {
    aloe::fabric::Fabric fabric;
    auto& a = fabric.add_port(port(alpha));
    const auto frame =
        aloe::frames::ethernet_frame(alpha, alpha, aloe::frames::ethertype_experimental, aloe::frames::pattern(30));
    send(a, frame);
    EXPECT_EQ(drain(a), std::vector<std::vector<std::byte>>{frame});
}

TEST(FabricDelivery, AFrameToNobodyVanishesWithoutACount) {
    aloe::fabric::Fabric fabric;
    auto& a = fabric.add_port(port(alpha));
    auto& b = fabric.add_port(port(beta));
    send(a,
         aloe::frames::ethernet_frame(nobody, alpha, aloe::frames::ethertype_experimental, aloe::frames::pattern(30)));
    EXPECT_TRUE(drain(a).empty());
    EXPECT_TRUE(drain(b).empty());
    EXPECT_EQ(a.counters(0).transmitted, 1);
    EXPECT_EQ(a.counters(0).oversized, 0);
    EXPECT_EQ(b.counters(0).dropped, 0);
}

TEST(FabricDelivery, AFullReceiveQueueDropsAndCounts) {
    aloe::fabric::Fabric fabric;
    auto& a = fabric.add_port(port(alpha));
    auto& b = fabric.add_port(port(beta));  // queue_depth 4
    for (std::uint8_t index = 0; index < 6; ++index) {
        send(a,
             aloe::frames::ethernet_frame(
                 beta, alpha, aloe::frames::ethertype_experimental, aloe::frames::pattern(30, index)));
    }
    EXPECT_EQ(b.counters(0).dropped, 2);
    EXPECT_EQ(drain(b).size(), 4);
    EXPECT_EQ(b.counters(0).received, 4);
}

TEST(FabricDelivery, ReceiveStopsWhenThePoolIsOutAndResumesAfterFrees) {
    aloe::fabric::Fabric fabric;
    auto& a            = fabric.add_port(port(alpha));
    auto b_config      = port(beta);
    b_config.pool_size = 2;
    auto& b            = fabric.add_port(b_config);
    for (std::uint8_t index = 0; index < 3; ++index) {
        send(a,
             aloe::frames::ethernet_frame(
                 beta, alpha, aloe::frames::ethertype_experimental, aloe::frames::pattern(30, index)));
    }
    std::array<aloe::fabric::Packet, 4> burst;
    EXPECT_EQ(b.receive(0, burst), 2) << "two packets in the pool, three frames pending";
    EXPECT_EQ(b.receive(0, std::span{burst}.last(2)), 0) << "the pool is out";
    burst[0] = aloe::fabric::Packet{};
    EXPECT_EQ(b.receive(0, std::span{burst}.last(2)), 1);
    EXPECT_EQ(b.counters(0).received, 3);
    EXPECT_EQ(b.counters(0).dropped, 0);
}

TEST(FabricDelivery, OversizedFramesAreCountedByTheReceiver) {
    aloe::fabric::Fabric fabric;
    auto a_config = port(alpha);
    a_config.mtu  = 1500;
    auto b_config = port(beta);
    b_config.mtu  = 100;
    auto& a       = fabric.add_port(a_config);
    auto& b       = fabric.add_port(b_config);
    send(a,
         aloe::frames::ethernet_frame(beta, alpha, aloe::frames::ethertype_experimental, aloe::frames::pattern(101)));
    send(a,
         aloe::frames::ethernet_frame(beta, alpha, aloe::frames::ethertype_experimental, aloe::frames::pattern(100)));
    EXPECT_EQ(b.counters(0).oversized, 1);
    EXPECT_EQ(drain(b).size(), 1);
    EXPECT_EQ(a.counters(0).transmitted, 2);
}

TEST(FabricDelivery, RuntAndOversizedFramesAreRefusedByTheTransmitter) {
    aloe::fabric::Fabric fabric;
    auto a_config = port(alpha);
    a_config.mtu  = 100;
    auto& a       = fabric.add_port(a_config);
    auto& b       = fabric.add_port(port(beta));
    send(a, std::vector<std::byte>(13));  // shorter than an Ethernet header
    send(a,
         aloe::frames::ethernet_frame(beta, alpha, aloe::frames::ethertype_experimental, aloe::frames::pattern(101)));
    EXPECT_EQ(a.counters(0).oversized, 2);
    EXPECT_EQ(a.counters(0).transmitted, 0);
    EXPECT_TRUE(drain(b).empty());
}

TEST(FabricDelivery, TransmitSkipsEmptyPackets) {
    aloe::fabric::Fabric fabric;
    auto& a = fabric.add_port(port(alpha));
    std::array<aloe::fabric::Packet, 2> burst;
    EXPECT_EQ(a.transmit(0, burst), 2);
    EXPECT_EQ(a.counters(0).transmitted, 0);
}

TEST(FabricDelivery, ReportsItsConfiguration) {
    aloe::fabric::Fabric fabric;
    auto config          = port(alpha);
    config.data_capacity = 2048;
    auto& a              = fabric.add_port(config);
    EXPECT_EQ(a.queue_count(), 1);
    EXPECT_EQ(a.mac(), alpha);
    EXPECT_EQ(a.mtu(), 1500);
    EXPECT_TRUE(a.link_up());
    EXPECT_FALSE(a.capabilities().rss);
    EXPECT_FALSE(a.capabilities().rx_l4_checksum);
    EXPECT_EQ(a.capabilities().max_rx_queues, 1);
    EXPECT_EQ(a.capabilities().max_mtu, 2048 - 14);
    EXPECT_FALSE(a.steering().enabled);
    EXPECT_EQ(fabric.port_count(), 1);
    static_assert(aloe::device::IsDevice<aloe::fabric::Port>);
}

TEST(FabricDelivery, RejectsImpossiblePorts) {
    aloe::fabric::Fabric fabric;
    std::ignore = fabric.add_port(port(alpha));
    EXPECT_THROW(std::ignore = fabric.add_port(port(alpha)), std::invalid_argument) << "duplicate MAC";
    auto no_queues   = port(beta);
    no_queues.queues = 0;
    EXPECT_THROW(std::ignore = fabric.add_port(no_queues), std::invalid_argument);
    auto tiny          = port(beta);
    tiny.data_capacity = 1513;
    EXPECT_THROW(std::ignore = fabric.add_port(tiny), std::invalid_argument) << "cannot hold mtu + 14";
    EXPECT_EQ(fabric.port_count(), 1);
}
