#include <aloe/frames>
#include <aloe/wire>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using aloe::frames::Checksums;
    using aloe::frames::TcpPeer;
    using aloe::frames::TcpSpec;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;
    using aloe::wire::TcpSequence;

    constexpr aloe::wire::MacAddress stack_mac{0x02, 0, 0, 0, 0, 0x01};
    constexpr aloe::wire::MacAddress peer_mac{0x02, 0, 0, 0, 0, 0x02};
    constexpr aloe::wire::Ipv4Address stack_ip{10, 0, 0, 2};
    constexpr aloe::wire::Ipv4Address peer_ip{10, 0, 0, 1};

    [[nodiscard]] TcpSpec peer_to_stack() {
        return {.destination_mac  = stack_mac,
                .source_mac       = peer_mac,
                .source           = peer_ip,
                .destination      = stack_ip,
                .source_port      = 40000,
                .destination_port = 7,
                .mss              = 1460};
    }

    [[nodiscard]] std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
        std::vector<std::byte> out;
        for (const unsigned value : values) {
            out.push_back(std::byte{static_cast<std::uint8_t>(value)});
        }
        return out;
    }

    [[nodiscard]] std::vector<std::byte> text(const char* s) {
        std::vector<std::byte> out;
        for (; *s != '\0'; ++s) {
            out.push_back(static_cast<std::byte>(*s));
        }
        return out;
    }

    /// The stack's SYN-ACK as the peer would see it: sequence 5000, acknowledging the peer's SYN.
    [[nodiscard]] aloe::frames::ParsedFrame stack_syn_ack(const TcpSequence acknowledgement) {
        TcpSpec spec          = peer_to_stack();
        spec.destination_mac  = peer_mac;
        spec.source_mac       = stack_mac;
        spec.source           = stack_ip;
        spec.destination      = peer_ip;
        spec.source_port      = 7;
        spec.destination_port = 40000;
        spec.sequence         = TcpSequence{5000U};
        spec.acknowledgement  = acknowledgement;
        spec.flags            = TcpFlags{TcpFlag::Syn, TcpFlag::Ack};
        spec.window           = 46720;
        return aloe::frames::parse_frame(aloe::frames::tcp_frame(spec, {})).value();
    }

}  // namespace

TEST(TcpFrames, HeaderOptionsPayloadAndChecksum) {
    TcpSpec spec     = peer_to_stack();
    spec.sequence    = TcpSequence{1000U};
    spec.flags       = TcpFlags{TcpFlag::Syn};
    const auto frame = aloe::frames::tcp_frame(spec, {});
    EXPECT_EQ(frame.size(), 14U + 20U + 24U);
    const auto parsed = aloe::frames::parse_frame(frame).value();
    ASSERT_TRUE(parsed.ipv4.has_value());
    EXPECT_EQ(parsed.ipv4->protocol, aloe::wire::Ipv4Protocol::Tcp);
    EXPECT_EQ(parsed.ipv4->total_length, 44U);
    EXPECT_EQ(aloe::wire::internet_checksum(parsed.ipv4_header), 0U);
    ASSERT_TRUE(parsed.tcp.has_value());
    EXPECT_EQ(parsed.tcp->data_offset, 24U) << "the MSS option";
    EXPECT_EQ(parsed.tcp->flags, TcpFlags{TcpFlag::Syn});
    EXPECT_EQ(parsed.tcp->checksum, 0xe3f4U) << "the vector computed for this exact segment";
    EXPECT_EQ(parsed.l4, bytes({0x9c, 0x40, 0x00, 0x07, 0x00, 0x00, 0x03, 0xe8, 0x00, 0x00, 0x00, 0x00,
                                0x60, 0x02, 0xff, 0xff, 0xe3, 0xf4, 0x00, 0x00, 0x02, 0x04, 0x05, 0xb4}));
    EXPECT_EQ(aloe::frames::l4_checksum_residue(*parsed.ipv4, parsed.l4), 0U);
    EXPECT_TRUE(aloe::frames::tcp_payload(parsed).empty());
    const auto options = aloe::wire::TcpOptions::parse(std::span<const std::byte>{parsed.l4}.subspan(20, 4));
    ASSERT_TRUE(options.has_value());
    EXPECT_EQ(options->mss, 1460U);
}

TEST(TcpFrames, PayloadAndKnownChecksum) {
    TcpSpec spec         = peer_to_stack();
    spec.mss             = std::nullopt;
    spec.sequence        = TcpSequence{1001U};
    spec.acknowledgement = TcpSequence{5001U};
    spec.flags           = TcpFlags{TcpFlag::Psh, TcpFlag::Ack};
    const auto payload   = text("abc");
    const auto parsed    = aloe::frames::parse_frame(aloe::frames::tcp_frame(spec, payload)).value();
    ASSERT_TRUE(parsed.tcp.has_value());
    EXPECT_EQ(parsed.tcp->data_offset, 20U);
    EXPECT_EQ(parsed.tcp->checksum, 0x23abU);
    EXPECT_EQ(aloe::frames::l4_checksum_residue(*parsed.ipv4, parsed.l4), 0U);
    const auto seen = aloe::frames::tcp_payload(parsed);
    EXPECT_EQ(std::vector<std::byte>(seen.begin(), seen.end()), payload);
}

TEST(TcpFrames, ChecksumModes) {
    TcpSpec spec   = peer_to_stack();
    spec.flags     = TcpFlags{TcpFlag::Ack};
    spec.mss       = std::nullopt;
    spec.checksums = Checksums::Zero;
    auto zero      = aloe::frames::parse_frame(aloe::frames::tcp_frame(spec, {})).value();
    EXPECT_EQ(zero.tcp->checksum, 0U);
    EXPECT_NE(aloe::frames::l4_checksum_residue(*zero.ipv4, zero.l4), 0U);
    EXPECT_EQ(zero.ipv4->checksum, 0U);
    spec.checksums = Checksums::Seeded;
    auto seeded    = aloe::frames::parse_frame(aloe::frames::tcp_frame(spec, {})).value();
    EXPECT_EQ(seeded.tcp->checksum,
              aloe::wire::ipv4_pseudo_header_sum(peer_ip, stack_ip, aloe::wire::Ipv4Protocol::Tcp, 20));
    spec.checksums = Checksums::Wrong;
    auto wrong     = aloe::frames::parse_frame(aloe::frames::tcp_frame(spec, {})).value();
    EXPECT_NE(aloe::frames::l4_checksum_residue(*wrong.ipv4, wrong.l4), 0U);
    EXPECT_EQ(aloe::wire::internet_checksum(wrong.ipv4_header), 0U) << "Wrong corrupts only the TCP checksum here";
}

TEST(TcpFrames, UnknownOptionBytesCanBeInsertedByHand) {
    TcpSpec spec        = peer_to_stack();
    spec.flags          = TcpFlags{TcpFlag::Syn};
    auto frame          = aloe::frames::tcp_frame(spec, {});
    // Replace the MSS option with an unknown kind of the same length: the parser skips it, the brick keeps the default.
    frame[14 + 20 + 20] = std::byte{30};
    const auto parsed   = aloe::frames::parse_frame(frame).value();
    ASSERT_TRUE(parsed.tcp.has_value());
    const auto options = aloe::wire::TcpOptions::parse(std::span<const std::byte>{parsed.l4}.subspan(20, 4));
    ASSERT_TRUE(options.has_value());
    EXPECT_FALSE(options->mss.has_value());
}

TEST(Parser, ExcludesEthernetPadding) {
    TcpSpec spec = peer_to_stack();
    spec.flags   = TcpFlags{TcpFlag::Ack};
    spec.mss     = std::nullopt;
    auto frame   = aloe::frames::tcp_frame(spec, {});
    frame.resize(60, std::byte{0xee});  // a card pads to the minimum frame
    const auto parsed = aloe::frames::parse_frame(frame).value();
    ASSERT_TRUE(parsed.tcp.has_value());
    EXPECT_EQ(parsed.l4.size(), 20U);
    EXPECT_TRUE(aloe::frames::tcp_payload(parsed).empty());
    EXPECT_EQ(aloe::frames::l4_checksum_residue(*parsed.ipv4, parsed.l4), 0U);
}

TEST(TcpPeer, TracksSynDataFinSequenceSpace) {
    TcpPeer peer{peer_to_stack(), TcpSequence{1000U}};
    EXPECT_EQ(peer.iss(), TcpSequence{1000U});
    const auto syn = aloe::frames::parse_frame(peer.syn()).value();
    EXPECT_EQ(syn.tcp->sequence, TcpSequence{1000U});
    EXPECT_TRUE(syn.tcp->flags.has(TcpFlag::Syn));
    EXPECT_FALSE(syn.tcp->flags.has(TcpFlag::Ack));
    EXPECT_EQ(syn.tcp->data_offset, 24U);
    EXPECT_EQ(peer.snd_nxt(), TcpSequence{1001U});

    peer.see(stack_syn_ack(TcpSequence{1001U}));
    EXPECT_EQ(peer.rcv_nxt(), TcpSequence{5001U});
    EXPECT_EQ(peer.snd_una(), TcpSequence{1001U});
    EXPECT_EQ(peer.stack_window(), 46720U);
    EXPECT_EQ(peer.stack_mss(), 1460U);

    const auto ack = aloe::frames::parse_frame(peer.ack()).value();
    EXPECT_EQ(ack.tcp->sequence, TcpSequence{1001U});
    EXPECT_EQ(ack.tcp->acknowledgement, TcpSequence{5001U});
    EXPECT_EQ(ack.tcp->flags, TcpFlags{TcpFlag::Ack});
    EXPECT_EQ(ack.tcp->data_offset, 20U) << "no MSS after the SYN";
    EXPECT_EQ(peer.snd_nxt(), TcpSequence{1001U}) << "a pure ACK takes no sequence space";

    const auto data = aloe::frames::parse_frame(peer.data(text("abc"))).value();
    EXPECT_EQ(data.tcp->sequence, TcpSequence{1001U});
    EXPECT_TRUE(data.tcp->flags.has(TcpFlag::Psh));
    EXPECT_EQ(peer.snd_nxt(), TcpSequence{1004U});

    const auto fin = aloe::frames::parse_frame(peer.fin()).value();
    EXPECT_EQ(fin.tcp->sequence, TcpSequence{1004U});
    EXPECT_EQ(fin.tcp->flags, (TcpFlags{TcpFlag::Fin, TcpFlag::Ack}));
    EXPECT_EQ(peer.snd_nxt(), TcpSequence{1005U});

    const auto rst = aloe::frames::parse_frame(peer.rst()).value();
    EXPECT_TRUE(rst.tcp->flags.has(TcpFlag::Rst));
    EXPECT_EQ(rst.tcp->sequence, TcpSequence{1005U});
}

TEST(TcpPeer, SynAckAdoptsThePortsOfTheSynItAnswers) {
    TcpPeer peer{peer_to_stack(), TcpSequence{1000U}};
    // The stack opened from port 33000 to the peer's 7: the peer answers from 7 to 33000.
    TcpSpec stack_spec          = peer_to_stack();
    stack_spec.destination_mac  = peer_mac;
    stack_spec.source_mac       = stack_mac;
    stack_spec.source           = stack_ip;
    stack_spec.destination      = peer_ip;
    stack_spec.source_port      = 33000;
    stack_spec.destination_port = 7;
    stack_spec.sequence         = TcpSequence{9000U};
    stack_spec.flags            = TcpFlags{TcpFlag::Syn};
    const auto seen             = aloe::frames::parse_frame(aloe::frames::tcp_frame(stack_spec, {})).value();
    const auto reply            = aloe::frames::parse_frame(peer.syn_ack(seen)).value();
    EXPECT_EQ(reply.tcp->source_port, 7U);
    EXPECT_EQ(reply.tcp->destination_port, 33000U);
    EXPECT_EQ(reply.tcp->acknowledgement, TcpSequence{9001U});
    EXPECT_EQ(reply.tcp->flags, (TcpFlags{TcpFlag::Syn, TcpFlag::Ack}));
    EXPECT_EQ(peer.snd_nxt(), TcpSequence{1001U});
    EXPECT_EQ(peer.spec().source_port, 7U);
}

TEST(TcpPeer, SeeNeverRetreats) {
    TcpPeer peer{peer_to_stack(), TcpSequence{1000U}};
    peer.see(stack_syn_ack(TcpSequence{1001U}));
    peer.see(stack_syn_ack(TcpSequence{1001U}));  // a retransmitted SYN-ACK
    EXPECT_EQ(peer.rcv_nxt(), TcpSequence{5001U});
}
