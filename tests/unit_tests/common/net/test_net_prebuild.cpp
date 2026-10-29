#include <algorithm>
#include <aloe/core>
#include <aloe/device>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <net_fixture.hpp>

namespace {
    using namespace std::chrono_literals;
    using aloe::device::L4Checksum;
    using aloe::net::Prebuilt;
    using aloe::net::SendError;
    using aloe::wire::Ipv4Protocol;
    class NetPrebuild : public aloe::testing::NetFixture {
    protected:
        /// A frame from `prebuilt` with a TCP header of ACK only (window 0xffff) and `payload`, checksum field zero.
        [[nodiscard]] Packet frame_from(const Prebuilt& prebuilt, const std::span<const std::byte> payload) {
            auto packet = ip_.allocate();
            EXPECT_TRUE(packet.has_value());
            const auto room = packet->append(Prebuilt::size + 20 + payload.size());
            EXPECT_TRUE(room.has_value());
            std::ranges::copy(prebuilt.headers, room->begin());
            const auto tcp = room->subspan(Prebuilt::size, 20);
            std::ranges::fill(tcp, std::byte{0});
            aloe::wire::store_be16(tcp.subspan(0, 2), 40000);
            aloe::wire::store_be16(tcp.subspan(2, 2), 80);
            tcp[12] = std::byte{0x50};
            tcp[13] = std::byte{0x10};
            aloe::wire::store_be16(tcp.subspan(14, 2), 0xffff);
            std::ranges::copy(payload, room->begin() + Prebuilt::size + 20);
            return std::move(*packet);
        }
        [[nodiscard]] static aloe::net::SendRequest tcp_to(const aloe::wire::Ipv4Address destination) {
            return {.destination = destination, .protocol = Ipv4Protocol::Tcp, .checksum = L4Checksum::Tcp};
        }
        /// `harness_received()` parsed: the fixture returns raw bytes.
        [[nodiscard]] std::vector<aloe::frames::ParsedFrame> collect() {
            std::vector<aloe::frames::ParsedFrame> frames;
            for (const auto& bytes : harness_received()) {
                if (auto parsed = aloe::frames::parse_frame(bytes)) {
                    frames.push_back(std::move(*parsed));
                }
            }
            return frames;
        }
    };
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             NetPrebuild,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
}  // namespace

TEST_P(NetPrebuild, PrebuildWritesBothHeadersWithThePerDatagramFieldsZero) {
    ip_.learn(aloe::testing::harness_ip, aloe::testing::harness_mac, now_);
    const auto prebuilt = ip_.prebuild(aloe::testing::harness_ip, Ipv4Protocol::Tcp, now_);
    ASSERT_TRUE(prebuilt.has_value());
    const auto ethernet = aloe::wire::EthernetHeader::parse(prebuilt->headers);
    ASSERT_TRUE(ethernet.has_value());
    EXPECT_EQ(ethernet->destination, aloe::testing::harness_mac);
    EXPECT_EQ(ethernet->source, aloe::testing::stack_mac);
    // `Ipv4Header::parse` refuses a total length of zero, so parse a copy with the length of an empty datagram.
    auto headers = prebuilt->headers;
    EXPECT_EQ(aloe::wire::load_be16(std::span<const std::byte>{headers}.subspan(16, 2)), 0U);
    aloe::wire::store_be16(std::span<std::byte>{headers}.subspan(16, 2), 20);
    const auto ipv4 = aloe::wire::Ipv4Header::parse(std::span<const std::byte>{headers}.subspan(14));
    ASSERT_TRUE(ipv4.has_value());
    EXPECT_EQ(ipv4->source, aloe::testing::stack_ip);
    EXPECT_EQ(ipv4->destination, aloe::testing::harness_ip);
    EXPECT_EQ(ipv4->protocol, Ipv4Protocol::Tcp);
    EXPECT_EQ(ipv4->ttl, 64U);
    EXPECT_EQ(ipv4->identification, 0U);
    EXPECT_EQ(ipv4->checksum, 0U);
    EXPECT_EQ(prebuilt->valid_until, now_ + 60s) << "confirmed now, reachable for arp_reachable";
    EXPECT_EQ(prebuilt->generation, ip_.generation());
}

TEST_P(NetPrebuild, PrebuildOffSubnetUsesTheGatewayAndStaleEntriesRebuildPerRequestInterval) {
    ip_.learn(aloe::testing::gateway_ip, aloe::testing::gateway_mac, now_);
    const auto fresh = ip_.prebuild(aloe::testing::far_ip, Ipv4Protocol::Tcp, now_);
    ASSERT_TRUE(fresh.has_value());
    EXPECT_EQ(aloe::wire::EthernetHeader::parse(fresh->headers)->destination, aloe::testing::gateway_mac);
    now_             += 61s;  // stale: a refresh request leaves, the MAC still serves
    const auto stale  = ip_.prebuild(aloe::testing::far_ip, Ipv4Protocol::Tcp, now_);
    ASSERT_TRUE(stale.has_value());
    EXPECT_EQ(stale->valid_until, now_ + 1s) << "arp_request_interval";
    EXPECT_EQ(ip_.counters().arp_requests_sent, 1U);
}

TEST_P(NetPrebuild, PrebuildFailsUnresolvedWithOneRequestAndNoRoute) {
    const auto unresolved = ip_.prebuild(aloe::testing::harness_ip, Ipv4Protocol::Tcp, now_);
    ASSERT_FALSE(unresolved.has_value());
    EXPECT_EQ(unresolved.error(), SendError::Unresolved);
    EXPECT_EQ(ip_.counters().send_unresolved, 1U);
    EXPECT_EQ(ip_.counters().arp_requests_sent, 1U);
    Ipv4 no_gateway{
        queue_, {.address = aloe::testing::stack_ip, .prefix = 24}
    };
    const auto no_route = no_gateway.prebuild(aloe::testing::far_ip, Ipv4Protocol::Tcp, now_);
    ASSERT_FALSE(no_route.has_value());
    EXPECT_EQ(no_route.error(), SendError::NoRoute);
}

TEST_P(NetPrebuild, TheGenerationMovesWithTheCache) {
    const auto before = ip_.generation();
    ip_.learn(aloe::testing::harness_ip, aloe::testing::harness_mac, now_);
    EXPECT_EQ(ip_.generation(), before + 1);
    ip_.learn(aloe::testing::harness_ip, aloe::testing::harness_mac, now_ + 1s);
    EXPECT_EQ(ip_.generation(), before + 1);
}

TEST_P(NetPrebuild, SendFrameLendsTransmitsAndCountsAndTheLoanComesBack) {
    ip_.learn(aloe::testing::harness_ip, aloe::testing::harness_mac, now_);
    const auto prebuilt = ip_.prebuild(aloe::testing::harness_ip, Ipv4Protocol::Tcp, now_);
    ASSERT_TRUE(prebuilt.has_value());
    const auto payload = aloe::frames::pattern(100);
    Packet packet      = frame_from(*prebuilt, payload);
    ASSERT_TRUE(ip_.send_frame(packet, tcp_to(aloe::testing::harness_ip), now_).has_value());
    EXPECT_TRUE(packet.lent());
    EXPECT_EQ(ip_.counters().datagrams_sent, 1U);
    const auto frames = collect();  // flushes the queue: the fabric delivers
    ASSERT_EQ(frames.size(), 1U);
    ASSERT_TRUE(frames[0].ipv4.has_value() && frames[0].tcp.has_value());
    EXPECT_EQ(frames[0].ipv4->total_length, 20U + 20U + 100U);
    EXPECT_EQ(aloe::wire::internet_checksum(frames[0].ipv4_header), 0U);
    EXPECT_EQ(aloe::frames::l4_checksum_residue(*frames[0].ipv4, frames[0].l4), 0U);
    EXPECT_EQ(aloe::frames::tcp_payload(frames[0]).size(), 100U);
    Packet second = frame_from(*prebuilt, payload);
    ASSERT_TRUE(ip_.send_frame(second, tcp_to(aloe::testing::harness_ip), now_).has_value());
    const auto next = collect();
    ASSERT_EQ(next.size(), 1U);
    EXPECT_EQ(next[0].ipv4->identification, frames[0].ipv4->identification + 1U);
    now_ += 1ms;
    fabric_.advance(now_);  // OnStep: the loans finish
    process_pending();      // the queue's next receive releases them
    EXPECT_FALSE(packet.lent());
    EXPECT_FALSE(second.lent());
}

TEST_P(NetPrebuild, ARefusedSendFrameReturnsTheLoanAndLeavesTheFrameAsBuilt) {
    aloe::testing::StuckPort stuck{port_};
    aloe::loop::ShardCounters counters;
    aloe::loop::ShardQueue<aloe::testing::StuckPort> queue{stuck, 0, 1, counters};
    aloe::net::Ipv4<aloe::testing::StuckPort> ip{queue, aloe::testing::stack_config()};
    ip.learn(aloe::testing::harness_ip, aloe::testing::harness_mac, now_);
    const auto prebuilt = ip.prebuild(aloe::testing::harness_ip, Ipv4Protocol::Tcp, now_);
    ASSERT_TRUE(prebuilt.has_value());
    auto filler = ip.allocate();  // fills the one-slot ring so the next transmit meets a full ring and a stuck device
    ASSERT_TRUE(filler.has_value());
    ASSERT_TRUE(filler->append(14).has_value());
    ASSERT_TRUE(queue.transmit(std::move(*filler)));
    Packet packet                       = frame_from(*prebuilt, aloe::frames::pattern(10));
    const std::vector<std::byte> before = aloe::frames::bytes_of(packet);
    const auto refused                  = ip.send_frame(packet, tcp_to(aloe::testing::harness_ip), now_);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), SendError::Refused);
    EXPECT_FALSE(packet.lent());
    EXPECT_EQ(aloe::frames::bytes_of(packet), before) << "identification and checksums restored to zero";
    EXPECT_EQ(packet.tx(), aloe::device::TxMetadata{});
    EXPECT_EQ(ip.counters().send_refused, 1U);
}

TEST_P(NetPrebuild, AnOversizedFrameIsRefusedUntouched) {
    ip_.learn(aloe::testing::harness_ip, aloe::testing::harness_mac, now_);
    const auto prebuilt = ip_.prebuild(aloe::testing::harness_ip, Ipv4Protocol::Tcp, now_);
    Packet packet       = frame_from(*prebuilt, aloe::frames::pattern(port_.mtu() - 20 - 20 + 1));
    const auto refused  = ip_.send_frame(packet, tcp_to(aloe::testing::harness_ip), now_);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), SendError::Oversized);
    EXPECT_FALSE(packet.lent());
    EXPECT_EQ(ip_.counters().send_oversized, 1U);
}
