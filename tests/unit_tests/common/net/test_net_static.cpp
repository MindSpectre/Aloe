#include <algorithm>
#include <aloe/core>
#include <aloe/device>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/net>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <net_fixture.hpp>

namespace {

    using aloe::device::L4Checksum;
    using aloe::net::ArpResolution;
    using aloe::net::SendError;
    using aloe::testing::far_ip;
    using aloe::testing::gateway_ip;
    using aloe::testing::gateway_mac;
    using aloe::testing::harness_ip;
    using aloe::testing::harness_mac;
    using aloe::testing::stack_ip;
    using aloe::testing::stack_mac;
    using aloe::wire::Ipv4Protocol;
    using Taught = aloe::net::ArpCache::Taught;

    constexpr aloe::wire::Ipv4Address stranger{10, 0, 0, 77};  ///< On the subnet, in no static entry.
    constexpr aloe::wire::MacAddress stranger_mac{0x02, 0, 0, 0, 0, 0x77};
    constexpr aloe::wire::Ipv4Address subnet_broadcast{10, 0, 0, 255};
    const aloe::wire::MacAddress broadcast = aloe::wire::MacAddress::broadcast();

    /// The tests' configuration: a /24 whose gateway and harness are known from the static table, learning as given.
    [[nodiscard]] aloe::net::Ipv4Config static_config(const bool learning = false) {
        return {
            .address      = stack_ip,
            .prefix       = 24,
            .gateway      = gateway_ip,
            .static_arp   = {{.address = gateway_ip, .mac = gateway_mac}, {.address = harness_ip, .mac = harness_mac}},
            .arp_learning = learning,
        };
    }

    // The harness claims the gateway's address at its own MAC, so the answer to a request lands where it can be read.
    [[nodiscard]] std::vector<std::byte> contradicting_reply() {
        return aloe::frames::arp_frame(aloe::frames::arp_reply(harness_mac, gateway_ip, stack_mac, stack_ip),
                                       stack_mac);
    }

    [[nodiscard]] std::vector<std::byte> contradicting_gratuitous_arp() {
        return aloe::frames::arp_frame(aloe::frames::arp_request(harness_mac, gateway_ip, gateway_ip), broadcast);
    }

    [[nodiscard]] std::vector<std::byte> contradicting_request_for_us() {
        return aloe::frames::arp_frame(aloe::frames::arp_request(harness_mac, gateway_ip, stack_ip), broadcast);
    }

    /**
     * The net fixture over a brick with `static_config()`, and a third port that owns the gateway's MAC, so a frame the
     * brick routes through the gateway can be read. Every test ends with the fixture brick's generation where it
     * started: the static table never moves it, and with learning off nothing else does.
     */
    class NetStatic : public aloe::testing::NetFixture {
    protected:
        NetStatic()
            : NetFixture{static_config()} {
        }

        void TearDown() override {
            EXPECT_EQ(ip_.generation(), generation_) << "the generation moved";
        }

        /// A UDP segment from port 40000 to port 80 with the checksum field zero and four bytes, on a fresh packet.
        [[nodiscard]] Packet udp_segment() {
            const auto payload = aloe::frames::pattern(4);
            auto packet        = ip_.allocate();
            EXPECT_TRUE(packet.has_value());
            const auto room = packet->append(8 + payload.size());
            EXPECT_TRUE(room.has_value());
            aloe::wire::store_be16(room->subspan(0, 2), 40000);
            aloe::wire::store_be16(room->subspan(2, 2), 80);
            aloe::wire::store_be16(room->subspan(4, 2), static_cast<std::uint16_t>(8 + payload.size()));
            aloe::wire::store_be16(room->subspan(6, 2), 0);
            std::ranges::copy(payload, room->begin() + 8);
            return std::move(*packet);
        }

        [[nodiscard]] static aloe::net::SendRequest udp_to(const aloe::wire::Ipv4Address destination) {
            return {.destination = destination, .protocol = Ipv4Protocol::Udp, .checksum = L4Checksum::Udp};
        }

        /// Sends `frame` from the harness and runs `brick`'s `process` over what the stack port received.
        void inject_into(Ipv4& brick, const std::span<const std::byte> frame) {
            auto packet = harness_.allocate(0);
            ASSERT_TRUE(packet.has_value());
            ASSERT_TRUE(aloe::frames::fill(*packet, frame));
            std::array<Packet, 1> out{std::move(*packet)};
            ASSERT_EQ(harness_.transmit(0, out), 1U);
            const std::size_t count = port_.receive(0, burst_);
            brick.process(std::span<Packet>{burst_}.first(count), now_);
        }

        /// Flushes the brick's queue and returns every frame `port` received, parsed, oldest first.
        [[nodiscard]] std::vector<aloe::frames::ParsedFrame> received_by(aloe::fabric::Port& port) {
            std::ignore = queue_.flush();
            std::vector<aloe::frames::ParsedFrame> frames;
            std::array<Packet, 16> out;
            for (std::size_t count = port.receive(0, out); count > 0; count = port.receive(0, out)) {
                for (std::size_t index = 0; index < count; ++index) {
                    if (auto parsed = aloe::frames::parse_frame(aloe::frames::bytes_of(out[index]))) {
                        frames.push_back(std::move(*parsed));
                    }
                    out[index] = Packet{};
                }
            }
            return frames;
        }

        aloe::fabric::Port& gateway_ = fabric_.add_port({.mac = gateway_mac, .pool_size = 64, .offloads = GetParam()});
        const std::uint64_t generation_ = ip_.generation();
    };

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             NetStatic,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

}  // namespace

TEST_P(NetStatic, AStaticNeighbourResolvesItsFirstSendWithNoRequest) {
    ASSERT_TRUE(ip_.send(udp_segment(), udp_to(harness_ip), now_).has_value());
    const auto frames = received_by(harness_);
    ASSERT_EQ(frames.size(), 1U) << "the datagram, and no request before it";
    EXPECT_EQ(frames[0].ethernet.destination, harness_mac);
    ASSERT_TRUE(frames[0].ipv4.has_value());
    EXPECT_EQ(frames[0].ipv4->destination, harness_ip);
    EXPECT_EQ(ip_.counters().arp_requests_sent, 0U);
    EXPECT_EQ(ip_.counters().send_unresolved, 0U);
    EXPECT_EQ(ip_.counters().datagrams_sent, 1U);
}

TEST_P(NetStatic, AStaticNeighbourNeverAges) {
    for (const aloe::core::TimePoint at : {now_, now_ + std::chrono::years{100}}) {
        ASSERT_TRUE(ip_.send(udp_segment(), udp_to(harness_ip), at).has_value());
        const auto frames = received_by(harness_);
        ASSERT_EQ(frames.size(), 1U) << "resolved, with no request, far past arp_expire";
        EXPECT_EQ(frames[0].ethernet.destination, harness_mac);
    }
    EXPECT_EQ(ip_.counters().arp_requests_sent, 0U);
    EXPECT_EQ(ip_.counters().send_unresolved, 0U);
}

TEST_P(NetStatic, ATemplateOfAStaticHopNeverGoesStale) {
    const std::uint64_t before = ip_.generation();
    const auto prebuilt        = ip_.prebuild(far_ip, Ipv4Protocol::Tcp, now_);
    ASSERT_TRUE(prebuilt.has_value());
    EXPECT_EQ(prebuilt->valid_until, aloe::core::TimePoint::max()) << "the end of time, as for a broadcast destination";
    EXPECT_EQ(prebuilt->generation, before);
    EXPECT_EQ(ip_.generation(), before);
    const auto ethernet = aloe::wire::EthernetHeader::parse(prebuilt->headers);
    ASSERT_TRUE(ethernet.has_value());
    EXPECT_EQ(ethernet->destination, gateway_mac);
    EXPECT_EQ(ip_.counters().arp_requests_sent, 0U);
}

TEST_P(NetStatic, AFrameThatContradictsAStaticEntryIsCountedAndChangesNothing) {
    inject(contradicting_reply());
    EXPECT_TRUE(ip_.resolved().empty());
    inject(contradicting_gratuitous_arp());
    EXPECT_TRUE(ip_.resolved().empty());
    inject(contradicting_request_for_us());
    EXPECT_TRUE(ip_.resolved().empty());
    EXPECT_EQ(ip_.counters().dropped_arp_static_conflict, 3U);
    EXPECT_EQ(ip_.counters().dropped_arp_unsolicited, 0U) << "the reply moves one drop counter";
    EXPECT_EQ(ip_.counters().resolutions, 0U);

    const auto answers = received_by(harness_);
    ASSERT_EQ(answers.size(), 1U) << "the request for our address is answered, to the frame's sender MAC";
    ASSERT_TRUE(answers[0].arp.has_value());
    EXPECT_EQ(answers[0].ethernet.destination, harness_mac);
    EXPECT_EQ(*answers[0].arp, aloe::frames::arp_reply(stack_mac, stack_ip, harness_mac, gateway_ip));

    std::ignore = received_by(gateway_);  // the gratuitous ARP's broadcast
    ASSERT_TRUE(ip_.send(udp_segment(), udp_to(far_ip), now_).has_value());
    const auto routed = received_by(gateway_);
    ASSERT_EQ(routed.size(), 1U) << "sends still go to the gateway's configured MAC";
    EXPECT_EQ(routed[0].ethernet.destination, gateway_mac);
    EXPECT_TRUE(received_by(harness_).empty());
}

TEST_P(NetStatic, AFrameThatAgreesWithAStaticEntryTeachesNothing) {
    const aloe::net::Ipv4Counters before = ip_.counters();
    inject(aloe::frames::arp_frame(aloe::frames::arp_reply(gateway_mac, gateway_ip, stack_mac, stack_ip), stack_mac));
    EXPECT_TRUE(ip_.resolved().empty());
    aloe::net::Ipv4Counters expected = before;
    ++expected.frames;
    ++expected.arp_replies_received;
    EXPECT_EQ(ip_.counters(), expected) << "no counter but the frame's and the reply's";
    EXPECT_TRUE(burst_empty());
}

TEST_P(NetStatic, NothingChangesTheStaticTable) {
    Ipv4 learning{queue_, static_config(true)};  // over the same queue: the fixture's brick stays idle meanwhile
    for (Ipv4* brick : {&ip_, &learning}) {
        SCOPED_TRACE(brick == &ip_ ? "learning off" : "learning on");
        const std::uint64_t generation = brick->generation();
        EXPECT_EQ(brick->learn(gateway_ip, stranger_mac, now_), Taught::Conflict) << "a static address, another MAC";
        inject_into(*brick, contradicting_reply());
        inject_into(*brick, contradicting_gratuitous_arp());
        inject_into(*brick, contradicting_request_for_us());
        EXPECT_TRUE(brick->resolved().empty());
        std::ignore = received_by(harness_);  // the answer to the request
        std::ignore = received_by(gateway_);  // the gratuitous ARP's broadcast
        for (const aloe::core::TimePoint at : {now_, now_ + std::chrono::years{100}}) {
            EXPECT_EQ(brick->resolve(gateway_ip, at), gateway_mac);
            ASSERT_TRUE(brick->send(udp_segment(), udp_to(far_ip), at).has_value());
            const auto routed = received_by(gateway_);
            ASSERT_EQ(routed.size(), 1U);
            EXPECT_EQ(routed[0].ethernet.destination, gateway_mac);
        }
        EXPECT_EQ(brick->generation(), generation);
        EXPECT_EQ(brick->counters().dropped_arp_static_conflict, 3U);
        EXPECT_EQ(brick->counters().arp_requests_sent, 0U);
        EXPECT_EQ(brick->counters().resolutions, 0U);
    }
}

TEST_P(NetStatic, WithLearningOffNoRequestIsEverSent) {
    const auto sent = ip_.send(udp_segment(), udp_to(stranger), now_);
    ASSERT_FALSE(sent.has_value());
    EXPECT_EQ(sent.error(), SendError::NoRoute);
    const auto prebuilt = ip_.prebuild(stranger, Ipv4Protocol::Tcp, now_);
    ASSERT_FALSE(prebuilt.has_value());
    EXPECT_EQ(prebuilt.error(), SendError::NoRoute);
    EXPECT_EQ(ip_.resolve(stranger, now_), std::nullopt);
    EXPECT_TRUE(received_by(harness_).empty());
    EXPECT_TRUE(received_by(gateway_).empty());
    EXPECT_EQ(ip_.counters().arp_requests_sent, 0U);
    EXPECT_EQ(ip_.counters().send_no_route, 2U);
    EXPECT_EQ(ip_.counters().send_unresolved, 0U);
}

TEST_P(NetStatic, WithLearningOffAReplyTeachesNothing) {
    inject(aloe::frames::arp_frame(aloe::frames::arp_reply(stranger_mac, stranger, stack_mac, stack_ip), stack_mac));
    EXPECT_TRUE(ip_.resolved().empty());
    EXPECT_EQ(ip_.counters().resolutions, 0U);
    const auto sent = ip_.send(udp_segment(), udp_to(stranger), now_);
    ASSERT_FALSE(sent.has_value());
    EXPECT_EQ(sent.error(), SendError::NoRoute);
}

TEST_P(NetStatic, WithLearningOffARequestForOurAddressIsAnswered) {
    // A stranger's address at the harness's MAC, so the answer lands where it can be read.
    inject(aloe::frames::arp_frame(aloe::frames::arp_request(harness_mac, stranger, stack_ip), broadcast));
    const auto answers = received_by(harness_);
    ASSERT_EQ(answers.size(), 1U);
    EXPECT_EQ(answers[0].ethernet.destination, harness_mac);
    ASSERT_TRUE(answers[0].arp.has_value());
    EXPECT_EQ(*answers[0].arp, aloe::frames::arp_reply(stack_mac, stack_ip, harness_mac, stranger));
    EXPECT_TRUE(ip_.resolved().empty());
    EXPECT_EQ(ip_.counters().arp_replies_sent, 1U);
    EXPECT_EQ(ip_.counters().resolutions, 0U);
    EXPECT_EQ(ip_.next_hop(stranger), std::nullopt) << "the request taught nothing";
}

TEST_P(NetStatic, WithLearningOffLearnDoesNothing) {
    EXPECT_EQ(ip_.learn(stranger, stranger_mac, now_), Taught::Off) << "nothing stored";
    const auto sent = ip_.send(udp_segment(), udp_to(stranger), now_);
    ASSERT_FALSE(sent.has_value());
    EXPECT_EQ(sent.error(), SendError::NoRoute);
    EXPECT_EQ(ip_.resolve(stranger, now_), std::nullopt);
}

TEST_P(NetStatic, ABrickThatResolvesNoNeighbourIsBuiltAndAnswersAnEcho) {
    // Learning off and an empty static table: no unicast next hop can have a MAC, but an echo responder or a
    // receive-only program needs none, so the brick alone is built; a TCP stack over it is refused.
    const aloe::net::Ipv4Config no_neighbours{.address = stack_ip, .prefix = 24};
    Ipv4 bare{queue_, no_neighbours};  // over the same queue: the fixture's brick stays idle meanwhile
    EXPECT_FALSE(bare.resolves_neighbours());
    EXPECT_TRUE(ip_.resolves_neighbours()) << "the static table holds two neighbours";
    const aloe::net::Ipv4Config learning_on{.address = stack_ip, .prefix = 24, .arp_learning = true};
    const Ipv4 learning{queue_, learning_on};
    EXPECT_TRUE(learning.resolves_neighbours()) << "with learning on any neighbour may be resolved";

    const aloe::frames::EchoSpec echo{
        .destination_mac = stack_mac, .source_mac = harness_mac, .source = harness_ip, .destination = stack_ip};
    inject_into(bare, aloe::frames::icmp_echo_frame(echo, aloe::frames::pattern(8)));
    const auto replies = received_by(harness_);
    ASSERT_EQ(replies.size(), 1U) << "answered at the request's source MAC, with no ARP";
    EXPECT_EQ(replies[0].ethernet.destination, harness_mac);
    ASSERT_TRUE(replies[0].icmp.has_value());
    EXPECT_EQ(replies[0].icmp->type, aloe::wire::IcmpType::EchoReply);
    EXPECT_EQ(bare.counters().echo_replies_sent, 1U);
    EXPECT_EQ(bare.counters().arp_requests_sent, 0U);
}

TEST_P(NetStatic, TheRouteQuestionKnowsTheStaticTable) {
    const aloe::net::Ipv4Counters before = ip_.counters();
    EXPECT_EQ(ip_.next_hop(harness_ip), harness_ip);
    EXPECT_EQ(ip_.next_hop(far_ip), gateway_ip);
    EXPECT_EQ(ip_.next_hop(stranger), std::nullopt) << "a neighbour outside the table, with learning off";
    EXPECT_EQ(ip_.next_hop(subnet_broadcast), subnet_broadcast);
    EXPECT_EQ(queue_.pending(), 0U) << "a question with no side effect";
    EXPECT_EQ(ip_.counters(), before);
}

TEST_P(NetStatic, ABadStaticTableIsRefused) {
    // The message of the refusal, or nothing when the brick is built.
    const auto refusal = [this](const aloe::net::Ipv4Config& config) {
        try {
            const Ipv4 brick{queue_, config};
        } catch (const std::invalid_argument& error) {
            return std::string{error.what()};
        }
        return std::string{};
    };
    const auto table = [](const std::vector<ArpResolution>& entries) {
        return aloe::net::Ipv4Config{.address = stack_ip, .prefix = 24, .static_arp = entries};
    };
    const std::string host = refusal(table({
        {.address = stack_ip, .mac = stranger_mac}
    }));
    EXPECT_FALSE(host.empty()) << "our own address";
    EXPECT_EQ(refusal(table({
                  {.address = far_ip, .mac = stranger_mac}
    })),
              host)
        << "an address off the subnet";
    EXPECT_EQ(refusal(table({
                  {.address = subnet_broadcast, .mac = stranger_mac}
    })),
              host)
        << "the subnet's broadcast";

    const std::string mac = refusal(table({
        {.address = stranger, .mac = aloe::wire::MacAddress{}}
    }));
    EXPECT_FALSE(mac.empty()) << "a zero MAC";
    EXPECT_EQ(refusal(table({
                  {.address = stranger, .mac = {0x01, 0x00, 0x5e, 0x00, 0x00, 0x01}}
    })),
              mac)
        << "a group MAC";

    const std::string twice = refusal(table({
        {.address = stranger, .mac = stranger_mac},
        {.address = stranger, .mac = harness_mac }
    }));
    EXPECT_FALSE(twice.empty()) << "an address listed twice";

    aloe::net::Ipv4Config gateway = table({
        {.address = harness_ip, .mac = harness_mac}
    });
    gateway.gateway               = gateway_ip;
    const std::string unknown     = refusal(gateway);
    EXPECT_FALSE(unknown.empty()) << "learning off, and a gateway the table does not hold";
    gateway.arp_learning = true;
    EXPECT_EQ(refusal(gateway), "") << "with learning on the gateway is resolved on the wire";

    EXPECT_NE(host, mac);
    EXPECT_NE(host, twice);
    EXPECT_NE(host, unknown);
    EXPECT_NE(mac, twice);
    EXPECT_NE(mac, unknown);
    EXPECT_NE(twice, unknown);
}
