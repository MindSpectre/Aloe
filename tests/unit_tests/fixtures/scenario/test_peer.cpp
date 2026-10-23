#include <aloe/frames>
#include <aloe/scenario>
#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using aloe::scenario::ObservedSegment;
    using aloe::scenario::Peer;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;
    using aloe::wire::TcpSequence;

    constexpr aloe::wire::MacAddress stack_mac{0x02, 0, 0, 0, 0, 0x01};
    constexpr aloe::wire::MacAddress peer_mac{0x02, 0, 0, 0, 0, 0x02};
    constexpr aloe::wire::Ipv4Address stack_ip{10, 0, 0, 2};
    constexpr aloe::wire::Ipv4Address peer_ip{10, 0, 0, 1};
    constexpr TcpSequence peer_isn{1'000'000};
    constexpr TcpSequence stack_isn{5000};

    /// A frame as the stack sends it: from the stack's port to the peer's.
    aloe::frames::ParsedFrame from_stack(const TcpFlags flags,
                                         const TcpSequence sequence,
                                         const TcpSequence acknowledgement,
                                         const std::uint16_t stack_port,
                                         const std::vector<std::byte>& payload = {},
                                         const bool with_mss                   = false) {
        aloe::frames::TcpSpec spec{.destination_mac  = peer_mac,
                                   .source_mac       = stack_mac,
                                   .source           = stack_ip,
                                   .destination      = peer_ip,
                                   .source_port      = stack_port,
                                   .destination_port = 40000,
                                   .sequence         = sequence,
                                   .acknowledgement  = acknowledgement,
                                   .flags            = flags,
                                   .window           = 46720};
        if (with_mss) {
            spec.mss = 1460;
        }
        return aloe::frames::parse_frame(aloe::frames::tcp_frame(spec, payload)).value();
    }

}  // namespace

TEST(Peer, ASynAckFixesTheStackIsnAndTheNumbersBecomeRelative) {
    Peer peer{peer_isn, 40000, 8080};
    EXPECT_FALSE(peer.stack_isn().has_value());
    EXPECT_EQ(peer.sequence(1), TcpSequence{1'000'001});
    EXPECT_EQ(peer.acknowledgement(7), TcpSequence{7}) << "absolute until the stack's ISN is seen";

    const auto seen = peer.observe(from_stack({TcpFlag::Syn, TcpFlag::Ack}, stack_isn, peer_isn + 1, 8080, {}, true));
    ASSERT_TRUE(seen.has_value());
    EXPECT_EQ(seen->flags, (TcpFlags{TcpFlag::Syn, TcpFlag::Ack}));
    EXPECT_EQ(seen->first, 0);
    EXPECT_EQ(seen->last, 0);
    EXPECT_EQ(seen->length, 0);
    EXPECT_EQ(seen->ack, 1);
    EXPECT_EQ(seen->window, 46720);
    EXPECT_EQ(seen->options, (std::vector<std::byte>{std::byte{2}, std::byte{4}, std::byte{0x05}, std::byte{0xb4}}));
    EXPECT_EQ(peer.stack_isn(), stack_isn);
    EXPECT_EQ(peer.stack_port(), 8080);
    EXPECT_EQ(peer.acknowledgement(1), TcpSequence{5001});

    const auto data = peer.observe(
        from_stack({TcpFlag::Psh, TcpFlag::Ack}, stack_isn + 1, peer_isn + 1, 8080, std::vector<std::byte>(1000)));
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(data->first, 1);
    EXPECT_EQ(data->last, 1001);
    EXPECT_EQ(data->length, 1000);
    EXPECT_TRUE(data->options.empty());
}

TEST(Peer, TheStacksSynAdoptsItsEphemeralPort) {
    Peer peer{peer_isn, 40000, 8080};
    const auto seen = peer.observe(from_stack({TcpFlag::Syn}, stack_isn, TcpSequence{}, 33333, {}, true));
    ASSERT_TRUE(seen.has_value());
    EXPECT_EQ(peer.stack_port(), 33333);
    EXPECT_EQ(peer.stack_isn(), stack_isn);
    const auto later = peer.observe(from_stack({TcpFlag::Syn}, TcpSequence{9}, TcpSequence{}, 44444, {}, true));
    ASSERT_TRUE(later.has_value());
    EXPECT_EQ(peer.stack_isn(), stack_isn) << "the first SYN fixes it; a retransmit is the same SYN";
    EXPECT_EQ(peer.stack_port(), 33333);
}

TEST(Peer, AResetBeforeAnyIsnIsAbsolute) {
    Peer peer{peer_isn, 40000, 8080};
    const auto seen = peer.observe(from_stack({TcpFlag::Rst, TcpFlag::Ack}, TcpSequence{0}, peer_isn + 1, 8080));
    ASSERT_TRUE(seen.has_value());
    EXPECT_EQ(seen->first, 0);
    EXPECT_EQ(seen->ack, 1);
    EXPECT_FALSE(peer.stack_isn().has_value());
}

TEST(Peer, ANonTcpFrameIsNotObserved) {
    Peer peer{peer_isn, 40000, 8080};
    const auto arp = aloe::frames::parse_frame(aloe::frames::arp_frame(
        aloe::frames::arp_request(stack_mac, stack_ip, peer_ip), aloe::wire::MacAddress::broadcast()));
    ASSERT_TRUE(arp.has_value());
    EXPECT_FALSE(peer.observe(*arp).has_value());
}
