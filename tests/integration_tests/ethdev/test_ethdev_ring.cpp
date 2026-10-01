#include <aloe/ethdev>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <device_conformance.hpp>
#include <eal_environment.hpp>
#include <frames.hpp>
#include <gtest/gtest.h>

namespace aloe::testing {

    namespace {

        const auto* const environment = ::testing::AddGlobalTestEnvironment(new EalEnvironment{"net_ring0"});

        /// A fresh single-queue ring port: what goes into queue 0 comes back out of queue 0.
        struct RingFixture {
            std::string name = probe_vdev("net_ring");
            ethdev::Port port{
                ethdev::PortConfig{.name = name, .queues = 1, .pool_size = 512}
            };

            ethdev::Port& device() {
                return port;
            }
        };

        constexpr device::MacAddress peer{0x02, 0, 0, 0, 0xfe, 0xed};

    }  // namespace

    INSTANTIATE_TYPED_TEST_SUITE_P(Ring, DeviceConformance, RingFixture);

}  // namespace aloe::testing

// Closing a port releases it for good, so no other test may use net_ring0.
TEST(EthdevRing, TheVdevFromTheEalArgumentsIsAPort) {
    const aloe::ethdev::Port port{
        {.name = "net_ring0", .queues = 1, .pool_size = 256}
    };
    EXPECT_EQ(port.driver_name(), "net_ring");
    EXPECT_EQ(port.queue_count(), 1);
    EXPECT_TRUE(port.link_up());
}

TEST(EthdevRing, ReportsNoRssNoOffloadsAndTheDefaultMtu) {
    const aloe::ethdev::Port port{
        {.name = aloe::testing::probe_vdev("net_ring"), .queues = 4, .pool_size = 256}
    };
    EXPECT_EQ(port.queue_count(), 4);
    EXPECT_FALSE(port.capabilities().rss);
    EXPECT_FALSE(port.steering().enabled);
    EXPECT_FALSE(port.capabilities().rx_ipv4_checksum);
    EXPECT_FALSE(port.capabilities().tx_l4_checksum);
    EXPECT_GE(port.capabilities().max_rx_queues, 4);
    EXPECT_EQ(port.mtu(), 1500) << "the driver cannot set an MTU, so configure's value stands";
}

TEST(EthdevRing, EachQueueLoopsBackToItself) {
    aloe::ethdev::Port port{
        {.name = aloe::testing::probe_vdev("net_ring"), .queues = 4, .pool_size = 256}
    };
    for (std::uint16_t queue = 0; queue < 4; ++queue) {
        const auto frame = aloe::testing::ethernet_frame(port.mac(),
                                                         aloe::testing::peer,
                                                         aloe::testing::ethertype_experimental,
                                                         aloe::testing::pattern(40, static_cast<std::uint8_t>(queue)));
        auto packet      = port.allocate(queue);
        ASSERT_TRUE(packet.has_value());
        ASSERT_TRUE(aloe::testing::fill(*packet, frame));
        std::array<aloe::ethdev::Packet, 1> burst{std::move(*packet)};
        ASSERT_EQ(port.transmit(queue, burst), 1);

        for (std::uint16_t other = 0; other < 4; ++other) {
            std::array<aloe::ethdev::Packet, 2> received;
            const std::size_t count = port.receive(other, received);
            if (other == queue) {
                ASSERT_EQ(count, 1) << "queue " << other;
                EXPECT_EQ(aloe::testing::bytes_of(received[0]), frame);
            } else {
                EXPECT_EQ(count, 0) << "queue " << other;
            }
        }
    }
}

TEST(EthdevRing, RefusesTheTransmitThatWouldOverfillTheRing) {
    aloe::ethdev::Port port{
        {.name = aloe::testing::probe_vdev("net_ring"), .queues = 1, .pool_size = 2048}
    };
    const auto frame = aloe::testing::ethernet_frame(
        port.mac(), aloe::testing::peer, aloe::testing::ethertype_experimental, aloe::testing::pattern(20));
    std::size_t accepted = 0;
    std::size_t offered  = 0;
    while (accepted == offered && accepted < 2000) {
        std::array<aloe::ethdev::Packet, 32> burst;
        for (auto& packet : burst) {
            auto allocated = port.allocate(0);
            ASSERT_TRUE(allocated.has_value());
            ASSERT_TRUE(aloe::testing::fill(*allocated, frame));
            packet = std::move(*allocated);
        }
        offered  += burst.size();
        accepted += port.transmit(0, burst);
    }
    EXPECT_LT(accepted, offered) << "the ring never filled";
    EXPECT_LE(accepted, 1024) << "a ring of 1024 entries";
    EXPECT_GT(accepted, 1000);

    std::array<aloe::ethdev::Packet, 1> one{port.allocate(0).value()};
    ASSERT_TRUE(aloe::testing::fill(one[0], frame));
    EXPECT_EQ(port.transmit(0, one), 0) << "a full ring accepts nothing";
    EXPECT_FALSE(one[0].empty());

    std::size_t drained = 0;
    std::array<aloe::ethdev::Packet, 64> received;
    for (std::size_t count = port.receive(0, received); count > 0; count = port.receive(0, received)) {
        drained += count;
        for (auto& packet : received) {
            packet = aloe::ethdev::Packet{};
        }
    }
    EXPECT_EQ(drained, accepted);
    EXPECT_EQ(port.counters(0).transmitted, accepted);
    EXPECT_EQ(port.counters(0).received, accepted);
}

TEST(EthdevRing, BurstsLargerThanMaxBurstMoveInParts) {
    aloe::ethdev::Port port{
        {.name = aloe::testing::probe_vdev("net_ring"), .queues = 1, .pool_size = 512}
    };
    const auto frame = aloe::testing::ethernet_frame(
        port.mac(), aloe::testing::peer, aloe::testing::ethertype_experimental, aloe::testing::pattern(20));
    constexpr std::size_t count = aloe::ethdev::Port::max_burst + 36;
    std::vector<aloe::ethdev::Packet> burst(count);
    for (auto& packet : burst) {
        auto allocated = port.allocate(0);
        ASSERT_TRUE(allocated.has_value());
        ASSERT_TRUE(aloe::testing::fill(*allocated, frame));
        packet = std::move(*allocated);
    }
    EXPECT_EQ(port.transmit(0, burst), aloe::ethdev::Port::max_burst);
    EXPECT_EQ(port.transmit(0, std::span{burst}.subspan(aloe::ethdev::Port::max_burst)), 36);

    std::vector<aloe::ethdev::Packet> received(count);
    EXPECT_EQ(port.receive(0, received), aloe::ethdev::Port::max_burst);
    EXPECT_EQ(port.receive(0, std::span{received}.subspan(aloe::ethdev::Port::max_burst)), 36);
    EXPECT_EQ(port.receive(0, std::span{received}.first(1)), 0);
}

TEST(EthdevRing, TheTransmitterRefusesRuntsAndOversizedFramesInOrder) {
    aloe::ethdev::Port port{
        {.name = aloe::testing::probe_vdev("net_ring"), .queues = 1, .pool_size = 256}
    };
    const auto first = aloe::testing::ethernet_frame(
        port.mac(), aloe::testing::peer, aloe::testing::ethertype_experimental, aloe::testing::pattern(20, 1));
    const auto last = aloe::testing::ethernet_frame(
        port.mac(), aloe::testing::peer, aloe::testing::ethertype_experimental, aloe::testing::pattern(20, 2));
    const auto huge = aloe::testing::ethernet_frame(port.mac(),
                                                    aloe::testing::peer,
                                                    aloe::testing::ethertype_experimental,
                                                    aloe::testing::pattern(static_cast<std::size_t>(port.mtu()) + 1));
    const std::vector<std::byte> runt(aloe::device::ethernet_header_size - 1);
    std::array<aloe::ethdev::Packet, 4> burst;
    for (std::size_t index = 0; const auto& frame : {first, runt, huge, last}) {
        auto packet = port.allocate(0);
        ASSERT_TRUE(packet.has_value());
        ASSERT_TRUE(aloe::testing::fill(*packet, frame));
        burst[index++] = std::move(*packet);
    }
    EXPECT_EQ(port.transmit(0, burst), 4) << "refused frames count as accepted";
    EXPECT_EQ(port.counters(0).oversized, 2) << "counted on the transmitting queue";
    EXPECT_EQ(port.counters(0).transmitted, 2);

    std::array<aloe::ethdev::Packet, 4> received;
    ASSERT_EQ(port.receive(0, received), 2);
    EXPECT_EQ(aloe::testing::bytes_of(received[0]), first);
    EXPECT_EQ(aloe::testing::bytes_of(received[1]), last);
}

TEST(EthdevRing, ARefusedFrameBehindAPartialSendStaysWithTheCaller) {
    aloe::ethdev::Port port{
        {.name = aloe::testing::probe_vdev("net_ring"), .queues = 1, .pool_size = 2048}
    };
    const auto frame = aloe::testing::ethernet_frame(
        port.mac(), aloe::testing::peer, aloe::testing::ethertype_experimental, aloe::testing::pattern(20));
    const auto packet_of = [&](std::span<const std::byte> bytes) {
        auto packet = port.allocate(0);
        EXPECT_TRUE(packet.has_value());
        EXPECT_TRUE(aloe::testing::fill(*packet, bytes));
        return std::move(*packet);
    };

    // The ring holds 1023 frames; leave room for exactly one.
    std::size_t queued = 0;
    while (queued < 1022) {
        std::array<aloe::ethdev::Packet, 1> one{packet_of(frame)};
        ASSERT_EQ(port.transmit(0, one), 1);
        ++queued;
    }

    const std::vector<std::byte> runt(aloe::device::ethernet_header_size - 1);
    std::array<aloe::ethdev::Packet, 4> burst{packet_of(frame), packet_of(frame), packet_of(runt), packet_of(frame)};
    EXPECT_EQ(port.transmit(0, burst), 1) << "only the first frame fit";
    EXPECT_TRUE(burst[0].empty());
    EXPECT_EQ(aloe::testing::bytes_of(burst[1]), frame);
    EXPECT_EQ(aloe::testing::bytes_of(burst[2]), runt) << "the runt behind the unsent frame is untouched";
    EXPECT_EQ(aloe::testing::bytes_of(burst[3]), frame);
    EXPECT_EQ(port.counters(0).oversized, 0) << "and not counted";
    EXPECT_EQ(port.counters(0).transmitted, 1023);
}
