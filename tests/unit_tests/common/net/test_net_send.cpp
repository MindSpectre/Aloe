#include <algorithm>
#include <aloe/core>
#include <aloe/net>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <frames.hpp>
#include <gtest/gtest.h>
#include <net_fixture.hpp>

namespace {

    using namespace std::chrono_literals;
    using aloe::device::Ipv4Protocol;
    using aloe::device::L4Checksum;
    using aloe::net::SendError;
    using aloe::testing::far_ip;
    using aloe::testing::gateway_ip;
    using aloe::testing::harness_ip;
    using aloe::testing::harness_mac;
    using aloe::testing::stack_ip;
    using aloe::testing::stack_mac;

    /// A device that takes nothing on transmit. The fabric port under it never refuses, so this does.
    class StuckPort {
    public:
        using Packet = aloe::fabric::Packet;

        explicit StuckPort(aloe::fabric::Port& inner) noexcept
            : inner_{&inner} {
        }

        [[nodiscard]] std::uint16_t queue_count() const noexcept {
            return inner_->queue_count();
        }

        [[nodiscard]] aloe::device::MacAddress mac() const noexcept {
            return inner_->mac();
        }

        [[nodiscard]] std::uint16_t mtu() const noexcept {
            return inner_->mtu();
        }

        [[nodiscard]] bool link_up() const noexcept {
            return inner_->link_up();
        }

        [[nodiscard]] const aloe::device::Capabilities& capabilities() const noexcept {
            return inner_->capabilities();
        }

        [[nodiscard]] const aloe::device::RssDescription& steering() const noexcept {
            return inner_->steering();
        }

        [[nodiscard]] std::optional<Packet> allocate(const std::uint16_t queue) noexcept {
            aloe::core::force_non_const(this);
            return inner_->allocate(queue);
        }

        [[nodiscard]] std::size_t receive(const std::uint16_t queue, std::span<Packet> out) noexcept {
            aloe::core::force_non_const(this);
            return inner_->receive(queue, out);
        }

        [[nodiscard]] static std::size_t transmit(std::uint16_t /*queue*/, std::span<Packet> /*in*/) noexcept {
            return 0;
        }

        [[nodiscard]] aloe::device::QueueCounters counters(const std::uint16_t queue) const noexcept {
            return inner_->counters(queue);
        }

    private:
        aloe::fabric::Port* inner_;
    };

    static_assert(aloe::device::IsDevice<StuckPort>);

    class NetSend : public aloe::testing::NetFixture {
    protected:
        /// A UDP segment from port 40000 to port 80 with the checksum field zero, then `payload`, on a fresh packet.
        [[nodiscard]] Packet udp_segment(const std::span<const std::byte> payload) {
            auto packet = ip_.allocate();
            EXPECT_TRUE(packet.has_value());
            const auto room = packet->append(8 + payload.size());
            EXPECT_TRUE(room.has_value());
            aloe::device::store_be16(room->subspan(0, 2), 40000);
            aloe::device::store_be16(room->subspan(2, 2), 80);
            aloe::device::store_be16(room->subspan(4, 2), static_cast<std::uint16_t>(8 + payload.size()));
            aloe::device::store_be16(room->subspan(6, 2), 0);
            std::ranges::copy(payload, room->begin() + 8);
            return std::move(*packet);
        }

        /// A TCP segment, ACK only, with the checksum field zero, then `payload`.
        [[nodiscard]] Packet tcp_segment(const std::span<const std::byte> payload) {
            auto packet = ip_.allocate();
            EXPECT_TRUE(packet.has_value());
            const auto room = packet->append(20 + payload.size());
            EXPECT_TRUE(room.has_value());
            std::ranges::fill(room->first(20), std::byte{0});
            aloe::device::store_be16(room->subspan(0, 2), 40000);
            aloe::device::store_be16(room->subspan(2, 2), 80);
            (*room)[12] = std::byte{0x50};  // data offset: five words
            (*room)[13] = std::byte{0x10};  // ACK
            aloe::device::store_be16(room->subspan(14, 2), 0xffff);
            std::ranges::copy(payload, room->begin() + 20);
            return std::move(*packet);
        }

        [[nodiscard]] static aloe::net::SendRequest udp_to(const aloe::device::Ipv4Address destination) {
            return {.destination = destination, .protocol = Ipv4Protocol::Udp, .checksum = L4Checksum::Udp};
        }
    };

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             NetSend,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

}  // namespace

TEST_P(NetSend, AnOnSubnetSendCarriesBothHeadersAndBothChecksums) {
    ip_.learn(harness_ip, harness_mac, now_);
    const auto payload = aloe::testing::pattern(24);
    ASSERT_TRUE(ip_.send(udp_segment(payload), udp_to(harness_ip), now_).has_value());

    const auto frames = harness_received();
    ASSERT_EQ(frames.size(), 1);
    const auto sent = aloe::testing::parse_frame(frames[0]);
    EXPECT_EQ(sent.ethernet.destination, harness_mac);
    EXPECT_EQ(sent.ethernet.source, stack_mac);
    EXPECT_EQ(sent.ethernet.ethertype, aloe::testing::ethertype_ipv4);
    ASSERT_TRUE(sent.ipv4.has_value());
    EXPECT_EQ(sent.ipv4->source, stack_ip);
    EXPECT_EQ(sent.ipv4->destination, harness_ip);
    EXPECT_EQ(sent.ipv4->protocol, Ipv4Protocol::Udp);
    EXPECT_EQ(sent.ipv4->header_length, 20);
    EXPECT_EQ(sent.ipv4->total_length, 20 + 8 + 24);
    EXPECT_EQ(sent.ipv4->ttl, 64);
    EXPECT_TRUE(sent.ipv4->dont_fragment);
    EXPECT_EQ(sent.ipv4->identification, 0);
    EXPECT_EQ(aloe::device::internet_checksum(sent.ipv4_header), 0);
    EXPECT_EQ(aloe::testing::l4_checksum_residue(*sent.ipv4, sent.l4), 0)
        << "UDP checksum right, by " << (offloads() ? "the device from the pseudo-header sum" : "software");
    EXPECT_EQ(aloe::device::load_be16(std::span<const std::byte>{sent.l4}.first(2)), 40000);
    EXPECT_EQ(std::vector<std::byte>(sent.l4.begin() + 8, sent.l4.end()), payload);
    EXPECT_EQ(ip_.counters().datagrams_sent, 1);
    EXPECT_EQ(ip_.counters().arp_requests_sent, 0);
}

TEST_P(NetSend, ATcpSegmentGetsItsChecksumAtOffsetSixteen) {
    ip_.learn(harness_ip, harness_mac, now_);
    ASSERT_TRUE(ip_.send(tcp_segment(aloe::testing::pattern(10)),
                         {.destination = harness_ip, .protocol = Ipv4Protocol::Tcp, .checksum = L4Checksum::Tcp},
                         now_)
                    .has_value());
    const auto frames = harness_received();
    ASSERT_EQ(frames.size(), 1);
    const auto sent = aloe::testing::parse_frame(frames[0]);
    ASSERT_TRUE(sent.ipv4.has_value());
    EXPECT_EQ(sent.ipv4->protocol, Ipv4Protocol::Tcp);
    EXPECT_EQ(sent.l4[12], std::byte{0x50});
    EXPECT_EQ(aloe::testing::l4_checksum_residue(*sent.ipv4, sent.l4), 0);
    EXPECT_NE(aloe::device::load_be16(std::span<const std::byte>{sent.l4}.subspan(16, 2)), 0)
        << "something was written there";
}

TEST_P(NetSend, WithoutAFillRequestTheL4FieldIsLeftAlone) {
    ip_.learn(harness_ip, harness_mac, now_);
    Packet packet = udp_segment(aloe::testing::pattern(4));
    aloe::device::store_be16(packet.data().subspan(6, 2), 0xabcd);
    ASSERT_TRUE(ip_.send(std::move(packet),
                         {.destination = harness_ip, .protocol = Ipv4Protocol::Udp, .checksum = L4Checksum::None},
                         now_)
                    .has_value());
    const auto frames = harness_received();
    ASSERT_EQ(frames.size(), 1);
    const auto sent = aloe::testing::parse_frame(frames[0]);
    EXPECT_EQ(aloe::device::load_be16(std::span<const std::byte>{sent.l4}.subspan(6, 2)), 0xabcd);
    EXPECT_EQ(aloe::device::internet_checksum(sent.ipv4_header), 0) << "the IPv4 checksum is always filled";
}

TEST_P(NetSend, AnOffSubnetSendGoesThroughTheGatewayOnceItResolves) {
    const auto first = ip_.send(udp_segment(aloe::testing::pattern(8)), udp_to(far_ip), now_);
    ASSERT_FALSE(first.has_value());
    EXPECT_EQ(first.error(), SendError::Unresolved);
    EXPECT_EQ(ip_.counters().send_unresolved, 1);
    auto frames = harness_received();
    ASSERT_EQ(frames.size(), 1) << "the ARP request, not the datagram";
    const auto request = aloe::testing::parse_frame(frames[0]);
    ASSERT_TRUE(request.arp.has_value());
    EXPECT_EQ(request.arp->target_ip, gateway_ip) << "the next hop is the gateway";
    EXPECT_EQ(request.ethernet.destination, aloe::device::MacAddress::broadcast());

    // The harness plays the gateway: it answers for gateway_ip with its own MAC, so the fabric delivers to it.
    inject(aloe::testing::arp_frame(aloe::testing::arp_reply(harness_mac, gateway_ip, stack_mac, stack_ip), stack_mac));
    ASSERT_TRUE(ip_.send(udp_segment(aloe::testing::pattern(8)), udp_to(far_ip), now_).has_value());
    frames = harness_received();
    ASSERT_EQ(frames.size(), 1);
    const auto sent = aloe::testing::parse_frame(frames[0]);
    EXPECT_EQ(sent.ethernet.destination, harness_mac) << "the frame goes to the gateway";
    ASSERT_TRUE(sent.ipv4.has_value());
    EXPECT_EQ(sent.ipv4->destination, far_ip) << "the datagram names the destination";
}

TEST_P(NetSend, AnUnresolvedSendHandsThePacketBackAndRateLimitsRequests) {
    Packet packet     = udp_segment(aloe::testing::pattern(8));
    const auto before = aloe::testing::bytes_of(packet);
    EXPECT_EQ(ip_.send(std::move(packet), udp_to(harness_ip), now_).error(), SendError::Unresolved);
    // NOLINTNEXTLINE(bugprone-use-after-move): send moves only on success
    EXPECT_EQ(aloe::testing::bytes_of(packet), before) << "the packet is the caller's again, untouched";
    EXPECT_EQ(harness_received().size(), 1);

    now_ += 500ms;
    EXPECT_EQ(ip_.send(udp_segment(aloe::testing::pattern(8)), udp_to(harness_ip), now_).error(),
              SendError::Unresolved);
    EXPECT_TRUE(harness_received().empty()) << "one request per interval";
    now_ += 500ms;
    EXPECT_EQ(ip_.send(udp_segment(aloe::testing::pattern(8)), udp_to(harness_ip), now_).error(),
              SendError::Unresolved);
    EXPECT_EQ(harness_received().size(), 1);
    EXPECT_EQ(ip_.counters().send_unresolved, 3);
    EXPECT_EQ(ip_.counters().arp_requests_sent, 2);
}

TEST_P(NetSend, NoGatewayMeansNoRoute) {
    Ipv4 lone{
        queue_, {.address = stack_ip, .prefix = 24}
    };
    const auto result = lone.send(udp_segment(aloe::testing::pattern(8)), udp_to(far_ip), now_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), SendError::NoRoute);
    EXPECT_TRUE(harness_received().empty());
    EXPECT_EQ(lone.counters().send_no_route, 1);
}

TEST_P(NetSend, BroadcastsAndMulticastGoOutWithoutArp) {
    ASSERT_TRUE(ip_.send(udp_segment(aloe::testing::pattern(4)), udp_to({255, 255, 255, 255}), now_).has_value());
    ASSERT_TRUE(ip_.send(udp_segment(aloe::testing::pattern(4)), udp_to({10, 0, 0, 255}), now_).has_value());
    ASSERT_TRUE(ip_.send(udp_segment(aloe::testing::pattern(4)), udp_to({239, 1, 2, 3}), now_).has_value());
    const auto frames = harness_received();
    ASSERT_EQ(frames.size(), 3) << "the fabric delivers broadcast and multicast to every other port";
    EXPECT_EQ(aloe::testing::parse_frame(frames[0]).ethernet.destination, aloe::device::MacAddress::broadcast());
    EXPECT_EQ(aloe::testing::parse_frame(frames[1]).ethernet.destination, aloe::device::MacAddress::broadcast());
    EXPECT_EQ(aloe::testing::parse_frame(frames[2]).ethernet.destination,
              aloe::device::MacAddress(0x01, 0x00, 0x5e, 0x01, 0x02, 0x03));
    EXPECT_EQ(ip_.counters().arp_requests_sent, 0);
    EXPECT_EQ(ip_.counters().datagrams_sent, 3);
}

TEST_P(NetSend, AnOversizedSegmentIsRefusedUnchangedAndTheLargestOneGoes) {
    ip_.learn(harness_ip, harness_mac, now_);
    ASSERT_EQ(ip_.max_l4_size(), 1480);
    Packet too_big = udp_segment(aloe::testing::pattern(1481 - 8));
    EXPECT_EQ(ip_.send(std::move(too_big), udp_to(harness_ip), now_).error(), SendError::Oversized);
    // NOLINTNEXTLINE(bugprone-use-after-move): send moves only on success
    EXPECT_EQ(too_big.size(), 1481U);
    EXPECT_TRUE(harness_received().empty());
    EXPECT_EQ(ip_.counters().send_oversized, 1);

    ASSERT_TRUE(ip_.send(udp_segment(aloe::testing::pattern(1480 - 8)), udp_to(harness_ip), now_).has_value());
    const auto frames = harness_received();
    ASSERT_EQ(frames.size(), 1);
    EXPECT_EQ(frames[0].size(), 1514U) << "exactly the MTU plus the Ethernet header";
}

TEST_P(NetSend, ARefusedSendReturnsThePacketExactlyAsBuilt) {
    StuckPort stuck{port_};
    aloe::loop::ShardCounters counters;
    aloe::loop::ShardQueue<StuckPort> queue{stuck, 0, 1, counters};  // a ring of one
    aloe::net::Ipv4<StuckPort> ip{queue, aloe::testing::stack_config()};
    ip.learn(harness_ip, harness_mac, now_);

    ASSERT_TRUE(ip.send(udp_segment(aloe::testing::pattern(4)), udp_to(harness_ip), now_).has_value())
        << "into the ring";
    Packet second       = udp_segment(aloe::testing::pattern(4));
    const auto before   = aloe::testing::bytes_of(second);
    const auto headroom = second.headroom();
    const auto result   = ip.send(std::move(second), udp_to(harness_ip), now_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), SendError::Refused);
    // NOLINTBEGIN(bugprone-use-after-move): send moves only on success
    EXPECT_EQ(aloe::testing::bytes_of(second), before) << "headers trimmed, the checksum field zero again";
    EXPECT_EQ(second.headroom(), headroom);
    EXPECT_EQ(second.tx(), aloe::device::TxMetadata{});
    // NOLINTEND(bugprone-use-after-move)
    EXPECT_EQ(ip.counters().send_refused, 1);
    EXPECT_EQ(ip.counters().datagrams_sent, 1);
    queue.discard();  // the ring still holds the first packet; the pool outlives it only if we drop it here
}
