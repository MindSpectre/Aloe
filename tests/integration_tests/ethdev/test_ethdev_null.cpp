#include <aloe/ethdev>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <tuple>

#include <eal_environment.hpp>
#include <gtest/gtest.h>

static_assert(aloe::device::IsDevice<aloe::ethdev::Port>);

TEST(EthdevNull, ReportsCapabilitiesAndRoundRobinSteering) {
    const aloe::ethdev::Port port{
        {.name = aloe::testing::probe_vdev("net_null"), .queues = 4, .pool_size = 256}
    };
    EXPECT_EQ(port.driver_name(), "net_null");
    EXPECT_EQ(port.queue_count(), 4);
    EXPECT_EQ(port.mtu(), 1500);
    EXPECT_TRUE(port.link_up());

    const aloe::device::Capabilities& capabilities = port.capabilities();
    EXPECT_TRUE(capabilities.rss);
    EXPECT_EQ(capabilities.rss_key_size, 40);
    EXPECT_EQ(capabilities.rss_table_size, 128);
    EXPECT_EQ(capabilities.rss_types, (aloe::device::RssHashTypes{.ipv4 = true, .ipv4_tcp = true, .ipv4_udp = true}));
    EXPECT_FALSE(capabilities.rx_ipv4_checksum);
    EXPECT_FALSE(capabilities.tx_ipv4_checksum);

    const aloe::device::RssDescription& steering = port.steering();
    EXPECT_TRUE(steering.enabled);
    EXPECT_EQ(steering.key_length, 40);
    for (std::size_t index = 0; index < 40; ++index) {
        EXPECT_EQ(steering.key[index], aloe::device::aloe_rss_key[index]) << "key byte " << index;
    }
    EXPECT_EQ(steering.types, capabilities.rss_types);
    ASSERT_EQ(steering.table.size(), 128);
    for (std::size_t index = 0; index < steering.table.size(); ++index) {
        EXPECT_EQ(steering.table[index], index % 4) << "table entry " << index;
    }
}

TEST(EthdevNull, ASingleQueuePortDoesNotSteer) {
    const aloe::ethdev::Port port{
        {.name = aloe::testing::probe_vdev("net_null"), .queues = 1, .pool_size = 256}
    };
    EXPECT_TRUE(port.capabilities().rss) << "the driver could";
    EXPECT_FALSE(port.steering().enabled) << "but one queue has nothing to steer between";
}

TEST(EthdevNull, ReceivesGeneratedFramesAndTransmitsIntoTheVoid) {
    aloe::ethdev::Port port{
        {.name = aloe::testing::probe_vdev("net_null"), .queues = 1, .pool_size = 256}
    };
    std::array<aloe::ethdev::Packet, 8> burst;
    const std::size_t received = port.receive(0, burst);
    EXPECT_EQ(received, 8) << "the null driver generates a frame per slot";
    for (std::size_t index = 0; index < received; ++index) {
        EXPECT_EQ(burst[index].size(), 64) << "the driver's default frame size";
        EXPECT_EQ(burst[index].headroom(), aloe::device::packet_headroom);
    }
    EXPECT_EQ(port.transmit(0, std::span{burst}.first(received)), received);
    for (std::size_t index = 0; index < received; ++index) {
        EXPECT_TRUE(burst[index].empty());
    }
    EXPECT_EQ(port.counters(0).received, received);
    EXPECT_EQ(port.counters(0).transmitted, received);
}

TEST(EthdevNull, RefusesWhatTheDriverCannotDo) {
    EXPECT_THROW((std::ignore = aloe::ethdev::Port{{.name = "net_nothing0"}}), aloe::ethdev::EthdevError);
    EXPECT_THROW((std::ignore =
                      aloe::ethdev::Port{
                          {.name = aloe::testing::probe_vdev("net_null"), .queues = 0}
    }),
                 aloe::ethdev::EthdevError);
    EXPECT_THROW((std::ignore =
                      aloe::ethdev::Port{
                          {.name = aloe::testing::probe_vdev("net_null"), .queues = 65535}
    }),
                 aloe::ethdev::EthdevError)
        << "more queues than the driver has";
}

TEST(EthdevNull, APortIsTakenOnce) {
    const std::string name = aloe::testing::probe_vdev("net_null");
    const aloe::ethdev::Port first{
        {.name = name, .queues = 1, .pool_size = 256}
    };
    EXPECT_THROW((std::ignore =
                      aloe::ethdev::Port{
                          {.name = name, .queues = 1, .pool_size = 256}
    }),
                 aloe::ethdev::EthdevError)
        << "a port is taken once";
    EXPECT_THROW((std::ignore =
                      aloe::ethdev::Port{
                          {.name = name, .queues = 65535}
    }),
                 aloe::ethdev::EthdevError)
        << "even a request that fails before configuring leaves the port alone";
    EXPECT_TRUE(first.link_up()) << "the first port is unharmed";
}
