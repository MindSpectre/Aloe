#include <algorithm>
#include <aloe/ethdev>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/wire>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <eal_environment.hpp>
#include <frames.hpp>
#include <gtest/gtest.h>
#include <net_frames.hpp>

// The brick over DPDK's ring driver, which loops a queue's transmit back into its own receive: ARP, echo and
// send on real mbufs, with no root. The ring has no offloads, so this is the software checksum path on ethdev.
namespace {

    using Packet = aloe::ethdev::Packet;
    using Ipv4   = aloe::net::Ipv4<aloe::ethdev::Port>;

    constexpr aloe::wire::Ipv4Address stack_ip{10, 0, 0, 2};
    constexpr aloe::wire::Ipv4Address peer_ip{10, 0, 0, 1};
    constexpr aloe::wire::MacAddress peer_mac{0x02, 0, 0, 0, 0xfe, 0xed};

    const auto* const environment = ::testing::AddGlobalTestEnvironment(new aloe::testing::EalEnvironment{"net_ring0"});

    class NetRing : public ::testing::Test {
    protected:
        aloe::ethdev::Port port_{
            aloe::ethdev::PortConfig{.name = aloe::testing::probe_vdev("net_ring"), .queues = 1, .pool_size = 256}
        };
        aloe::loop::ShardCounters counters_;
        aloe::loop::ShardQueue<aloe::ethdev::Port> queue_{port_, 0, 16, counters_};
        Ipv4 ip_{
            queue_, {.address = stack_ip, .prefix = 24}
        };
        aloe::core::TimePoint now_{};
        std::vector<Packet> burst_ = std::vector<Packet>(64);

        /// Puts `frame` on the wire as a peer would; the ring hands it back on the next receive.
        void inject(const std::span<const std::byte> frame) {
            auto packet = port_.allocate(0);
            ASSERT_TRUE(packet.has_value());
            ASSERT_TRUE(aloe::testing::fill(*packet, frame));
            std::array<Packet, 1> out{std::move(*packet)};
            ASSERT_EQ(port_.transmit(0, out), 1);
        }

        /// One tick of a hand-written loop: receive, process, flush.
        void tick() {
            const std::size_t received = port_.receive(0, burst_);
            ip_.process(std::span<Packet>{burst_}.first(received), now_);
            std::ignore = queue_.flush();
        }

        /// What the brick put on the wire, read back raw.
        [[nodiscard]] std::vector<std::vector<std::byte>> on_the_wire() {
            std::vector<std::vector<std::byte>> frames;
            std::array<Packet, 16> out;
            const std::size_t count = port_.receive(0, out);
            frames.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                frames.push_back(aloe::testing::bytes_of(out[index]));
            }
            return frames;
        }
    };

}  // namespace

TEST_F(NetRing, AnArpRequestIsAnsweredOnRealMbufs) {
    inject(aloe::testing::arp_frame(aloe::testing::arp_request(peer_mac, peer_ip, stack_ip),
                                    aloe::wire::MacAddress::broadcast()));
    tick();
    const auto frames = on_the_wire();
    ASSERT_EQ(frames.size(), 1);
    const auto reply = aloe::testing::parse_frame(frames[0]);
    EXPECT_EQ(reply.ethernet.destination, peer_mac);
    EXPECT_EQ(reply.ethernet.source, port_.mac());
    ASSERT_TRUE(reply.arp.has_value());
    EXPECT_EQ(*reply.arp, aloe::testing::arp_reply(port_.mac(), stack_ip, peer_mac, peer_ip));
    EXPECT_EQ(ip_.counters().arp_replies_sent, 1);
}

TEST_F(NetRing, AnEchoRequestIsAnsweredOnRealMbufs) {
    ASSERT_FALSE(port_.capabilities().tx_ipv4_checksum) << "the ring has no offloads: software checksums";
    const auto payload = aloe::testing::pattern(56);
    inject(aloe::testing::with_ipv4_options(
        aloe::testing::icmp_echo_frame(
            {.destination_mac = port_.mac(), .source_mac = peer_mac, .source = peer_ip, .destination = stack_ip},
            payload),
        3));  // options make `trim_front` run with a non-zero count on a real mbuf
    tick();
    const auto frames = on_the_wire();
    ASSERT_EQ(frames.size(), 1);
    const auto reply = aloe::testing::parse_frame(frames[0]);
    EXPECT_EQ(reply.ethernet.destination, peer_mac);
    EXPECT_EQ(reply.ethernet.source, port_.mac());
    ASSERT_TRUE(reply.ipv4.has_value());
    EXPECT_EQ(reply.ipv4->header_length, 20);
    EXPECT_EQ(reply.ipv4->destination, peer_ip);
    EXPECT_EQ(reply.ipv4->total_length, 20 + 8 + 56);
    EXPECT_EQ(aloe::wire::internet_checksum(reply.ipv4_header), 0);
    ASSERT_TRUE(reply.icmp.has_value());
    EXPECT_EQ(reply.icmp->type, aloe::wire::IcmpType::EchoReply);
    EXPECT_EQ(aloe::wire::internet_checksum(reply.l4), 0);
    EXPECT_EQ(std::vector<std::byte>(reply.l4.begin() + 8, reply.l4.end()), payload);
    EXPECT_EQ(ip_.counters().echo_replies_sent, 1);
}

TEST_F(NetRing, ASentSegmentComesBackThroughTheRingIntoTheUdpList) {
    ip_.learn(stack_ip, port_.mac(), now_);  // a send to ourselves: the ring brings it back addressed to us
    auto packet = ip_.allocate();
    ASSERT_TRUE(packet.has_value());
    const auto payload = aloe::testing::pattern(16);
    const auto room    = packet->append(8 + payload.size());
    ASSERT_TRUE(room.has_value());
    aloe::wire::store_be16(room->subspan(0, 2), 40000);
    aloe::wire::store_be16(room->subspan(2, 2), 80);
    aloe::wire::store_be16(room->subspan(4, 2), static_cast<std::uint16_t>(8 + payload.size()));
    aloe::wire::store_be16(room->subspan(6, 2), 0);
    std::ranges::copy(payload, room->begin() + 8);

    ASSERT_TRUE(ip_.send(std::move(*packet),
                         {.destination = stack_ip,
                          .protocol    = aloe::wire::Ipv4Protocol::Udp,
                          .checksum    = aloe::device::L4Checksum::Udp},
                         now_)
                    .has_value());
    std::ignore = queue_.flush();
    tick();

    ASSERT_EQ(ip_.received(aloe::wire::Ipv4Protocol::Udp).size(), 1);
    auto& received = ip_.received(aloe::wire::Ipv4Protocol::Udp)[0];
    EXPECT_EQ(received.source, stack_ip);
    EXPECT_EQ(received.destination, stack_ip);
    EXPECT_EQ(received.l4_length, 8 + 16);
    const auto header = aloe::wire::parse_ipv4(received.packet.data().subspan(aloe::wire::ethernet_header_size));
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(aloe::testing::l4_checksum_residue(*header, received.l4()), 0) << "the software UDP checksum";
    EXPECT_EQ(std::vector<std::byte>(received.l4().begin() + 8, received.l4().end()), payload);
    EXPECT_EQ(ip_.counters().datagrams_sent, 1);
    EXPECT_EQ(ip_.counters().delivered_udp, 1);
    EXPECT_EQ(ip_.counters().dropped_bad_checksum, 0) << "the receive path verified the software header checksum";
}
