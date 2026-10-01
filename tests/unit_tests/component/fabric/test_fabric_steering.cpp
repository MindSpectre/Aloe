#include <aloe/fabric>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <frames.hpp>
#include <gtest/gtest.h>

namespace {

    constexpr aloe::MacAddress client{0x02, 0, 0, 0, 0, 0x01};
    constexpr aloe::MacAddress server{0x02, 0, 0, 0, 0, 0x02};
    constexpr aloe::Ipv4Address client_ip{10, 0, 0, 1};
    constexpr aloe::Ipv4Address server_ip{10, 0, 0, 2};

    class FabricSteering : public testing::Test {
    protected:
        aloe::fabric::Fabric fabric_;
        aloe::fabric::Port& client_port_ = fabric_.add_port({.mac = client, .queues = 1, .pool_size = 64});
        aloe::fabric::Port& server_port_ = fabric_.add_port({.mac = server, .queues = 4, .pool_size = 64});

        void send(std::span<const std::byte> frame) {
            auto packet = client_port_.allocate(0);
            ASSERT_TRUE(packet.has_value());
            ASSERT_TRUE(aloe::testing::fill(*packet, frame));
            std::array<aloe::fabric::Packet, 1> burst{std::move(*packet)};
            ASSERT_EQ(client_port_.transmit(0, burst), 1);
        }

        /// The one packet pending on `queue`, which must be the only queue with anything pending.
        aloe::fabric::Packet receive_only_on(std::uint16_t queue) {
            aloe::fabric::Packet result;
            for (std::uint16_t index = 0; index < server_port_.queue_count(); ++index) {
                std::array<aloe::fabric::Packet, 2> burst;
                const std::size_t count = server_port_.receive(index, burst);
                if (index == queue) {
                    EXPECT_EQ(count, 1) << "queue " << index;
                    result = std::move(burst[0]);
                } else {
                    EXPECT_EQ(count, 0) << "queue " << index;
                }
            }
            return result;
        }

        static aloe::testing::Ipv4Spec spec(const aloe::Ipv4Protocol protocol, const std::uint16_t source_port) {
            return {.destination_mac  = server,
                    .source_mac       = client,
                    .source           = client_ip,
                    .destination      = server_ip,
                    .source_port      = source_port,
                    .destination_port = 80,
                    .protocol         = protocol};
        }
    };

}  // namespace

TEST_F(FabricSteering, AMultiQueuePortSteersByRss) {
    EXPECT_TRUE(server_port_.capabilities().rss);
    EXPECT_TRUE(server_port_.steering().enabled);
    EXPECT_EQ(server_port_.steering().key_length, 52);
    EXPECT_EQ(server_port_.steering().table.size(), 128);
    EXPECT_EQ(server_port_.capabilities().rss_table_size, 128);
    EXPECT_EQ(server_port_.capabilities().rss_types,
              (aloe::RssHashTypes{.ipv4 = true, .ipv4_tcp = true, .ipv4_udp = true}));
    EXPECT_FALSE(client_port_.capabilities().rss);
}

TEST_F(FabricSteering, TcpAndUdpLandOnTheQueueTheirFourTupleSelects) {
    std::array<bool, 4> seen{};
    for (std::uint16_t port = 40000; port < 40064; ++port) {
        for (const aloe::Ipv4Protocol protocol : {aloe::Ipv4Protocol::Tcp, aloe::Ipv4Protocol::Udp}) {
            const auto frame_spec        = spec(protocol, port);
            const std::uint16_t expected = aloe::queue_for(server_port_.steering(), aloe::testing::flow_of(frame_spec));
            send(aloe::testing::ipv4_frame(frame_spec, aloe::testing::pattern(8)));
            aloe::fabric::Packet packet = receive_only_on(expected);
            ASSERT_FALSE(packet.empty());
            EXPECT_EQ(packet.rx().rss_hash,
                      aloe::flow_hash(server_port_.steering(), aloe::testing::flow_of(frame_spec)));
            seen[expected] = true;
        }
    }
    EXPECT_EQ(seen, (std::array{true, true, true, true})) << "64 ports spread over every queue";
}

TEST_F(FabricSteering, OtherIpv4LandsOnTheQueueTheAddressesSelect) {
    const auto frame_spec        = spec(aloe::Ipv4Protocol::Icmp, 0);
    const std::uint16_t expected = aloe::queue_for(server_port_.steering(), aloe::testing::flow_of(frame_spec));
    send(aloe::testing::ipv4_frame(frame_spec, aloe::testing::pattern(8)));
    EXPECT_FALSE(receive_only_on(expected).empty());
}

TEST_F(FabricSteering, AFragmentIsSteeredByAddressesOnly) {
    auto fragment           = spec(aloe::Ipv4Protocol::Tcp, 40000);
    fragment.flags_fragment = 0x2000;  // more fragments, offset zero
    auto whole              = spec(aloe::Ipv4Protocol::Icmp, 0);
    EXPECT_FALSE(aloe::testing::flow_of(fragment).protocol.has_value());
    const std::uint16_t expected = aloe::queue_for(server_port_.steering(), aloe::testing::flow_of(whole));
    send(aloe::testing::ipv4_frame(fragment, aloe::testing::pattern(8)));
    EXPECT_FALSE(receive_only_on(expected).empty());
}

TEST_F(FabricSteering, NonIpv4GoesToQueueZeroWithoutAHash) {
    send(aloe::testing::ethernet_frame(
        server, client, aloe::testing::ethertype_experimental, aloe::testing::pattern(40)));
    aloe::fabric::Packet packet = receive_only_on(0);
    ASSERT_FALSE(packet.empty());
    EXPECT_FALSE(packet.rx().rss_hash.has_value());
}

TEST_F(FabricSteering, ASingleQueuePortHashesNothing) {
    auto reply            = spec(aloe::Ipv4Protocol::Tcp, 40000);
    reply.destination_mac = client;
    reply.source_mac      = server;
    auto packet           = server_port_.allocate(0);
    ASSERT_TRUE(packet.has_value());
    ASSERT_TRUE(aloe::testing::fill(*packet, aloe::testing::ipv4_frame(reply, aloe::testing::pattern(8))));
    std::array<aloe::fabric::Packet, 1> burst{std::move(*packet)};
    ASSERT_EQ(server_port_.transmit(0, burst), 1);

    std::array<aloe::fabric::Packet, 1> received;
    ASSERT_EQ(client_port_.receive(0, received), 1);
    EXPECT_FALSE(received[0].rx().rss_hash.has_value());
}

TEST_F(FabricSteering, ATruncatedFrameIsStillSteeredAndDoesNotCrash) {
    const auto frame_spec = spec(aloe::Ipv4Protocol::Tcp, 40000);
    auto frame            = aloe::testing::ipv4_frame(frame_spec, aloe::testing::pattern(64));
    frame.resize(frame.size() - 40);  // the total length now claims more than the frame carries
    const std::uint16_t expected = aloe::queue_for(server_port_.steering(), aloe::testing::flow_of(frame_spec));
    send(frame);
    EXPECT_FALSE(receive_only_on(expected).empty());
}

TEST_F(FabricSteering, AnIpv4HeaderWithOptionsIsParsedByItsLength) {
    const auto frame_spec        = spec(aloe::Ipv4Protocol::Udp, 40001);
    const std::uint16_t expected = aloe::queue_for(server_port_.steering(), aloe::testing::flow_of(frame_spec));
    send(aloe::testing::with_ipv4_options(aloe::testing::ipv4_frame(frame_spec, aloe::testing::pattern(8)), 2));
    aloe::fabric::Packet packet = receive_only_on(expected);
    ASSERT_FALSE(packet.empty());
    EXPECT_EQ(packet.rx().rss_hash, aloe::flow_hash(server_port_.steering(), aloe::testing::flow_of(frame_spec)));
}
