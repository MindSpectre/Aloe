#include <algorithm>
#include <aloe/ethdev>
#include <aloe/frames>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/stream>
#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <eal_environment.hpp>
#include <gtest/gtest.h>

// The brick over DPDK's ring driver, which loops a queue's transmit back into its own receive. A scripted
// peer at another address drives it: what the stack transmits is read back from the ring and never fed to
// IP again; the peer's frames are injected and processed. Real mbufs, headroom, offsets, software checksums.
namespace {

    using namespace std::chrono_literals;
    using Packet    = aloe::ethdev::Packet;
    using Ipv4      = aloe::net::Ipv4<aloe::ethdev::Port>;
    using Tcp       = aloe::tcp::Stack<Ipv4>;
    using TimePoint = aloe::core::TimePoint;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;
    using aloe::wire::TcpSequence;

    constexpr aloe::wire::Ipv4Address stack_ip{10, 0, 0, 2};
    constexpr aloe::wire::Ipv4Address peer_ip{10, 0, 0, 1};
    constexpr aloe::wire::MacAddress peer_mac{0x02, 0, 0, 0, 0xfe, 0xed};

    const auto* const environment = ::testing::AddGlobalTestEnvironment(new aloe::testing::EalEnvironment{"net_ring0"});

    class TcpRing : public ::testing::Test {
    protected:
        aloe::ethdev::Port port_{
            aloe::ethdev::PortConfig{.name = aloe::testing::probe_vdev("net_ring"), .queues = 1, .pool_size = 256}
        };
        aloe::loop::ShardCounters counters_;
        aloe::loop::ShardQueue<aloe::ethdev::Port> queue_{port_, 0, 16, counters_};
        Ipv4 ip_{
            queue_, {.address = stack_ip, .prefix = 24}
        };
        aloe::loop::TimerWheel wheel_{1ms, TimePoint{}};
        Tcp tcp_{ip_, wheel_, {}};
        TimePoint now_{};
        std::vector<Packet> burst_ = std::vector<Packet>(64);
        aloe::frames::TcpPeer peer_{
            {.destination_mac  = port_.mac(),
             .source_mac       = peer_mac,
             .source           = peer_ip,
             .destination      = stack_ip,
             .source_port      = 40000,
             .destination_port = 7,
             .mss              = 1460},
            TcpSequence{1000U}
        };

        TcpRing() {
            ip_.learn(peer_ip, peer_mac, now_);
        }

        /// Flushes, then reads back what the stack put on the ring: its own transmissions, taken apart.
        [[nodiscard]] std::vector<aloe::frames::ParsedFrame> collect() {
            tcp_.flush(now_);
            std::ignore = queue_.flush();
            std::vector<aloe::frames::ParsedFrame> frames;
            std::array<Packet, 16> out;
            for (std::size_t count = port_.receive(0, out); count > 0; count = port_.receive(0, out)) {
                for (std::size_t index = 0; index < count; ++index) {
                    frames.push_back(aloe::frames::parse_frame(aloe::frames::bytes_of(out[index])).value());
                    out[index] = Packet{};
                }
            }
            return frames;
        }

        /// Puts the peer's frame on the ring and processes it: the ring must be empty of our own frames first.
        void receive(const std::span<const std::byte> frame) {
            auto packet = port_.allocate(0);
            ASSERT_TRUE(packet.has_value());
            ASSERT_TRUE(aloe::frames::fill(*packet, frame));
            std::array<Packet, 1> out{std::move(*packet)};
            ASSERT_EQ(port_.transmit(0, out), 1U);
            const std::size_t received = port_.receive(0, burst_);
            ASSERT_EQ(received, 1U) << "only the injected frame: our own were drained by collect()";
            ip_.process(std::span<Packet>{burst_}.first(received), now_);
            tcp_.process(ip_.received(aloe::wire::Ipv4Protocol::Tcp), now_);
            ASSERT_TRUE(burst_[0].empty()) << "taken or freed";
        }

        [[nodiscard]] static std::vector<std::byte> text(const char* s) {
            std::vector<std::byte> out;
            for (; *s != '\0'; ++s) {
                out.push_back(static_cast<std::byte>(*s));
            }
            return out;
        }
    };

}  // namespace

TEST_F(TcpRing, ScriptedPeerConnectsSendsAndCloses) {
    ASSERT_TRUE(tcp_.listen(7).has_value());
    receive(peer_.syn());
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->flags, (TcpFlags{TcpFlag::Syn, TcpFlag::Ack}));
    EXPECT_EQ(frames[0].tcp->data_offset, 24U);
    EXPECT_EQ(frames[0].ipv4->total_length, 44U) << "20 of IPv4 and 24 of TCP: the 54-byte base plus the option";
    EXPECT_EQ(frames[0].ipv4->header_length, 20U);
    EXPECT_EQ(aloe::frames::l4_checksum_residue(*frames[0].ipv4, frames[0].l4), 0U)
        << "software checksum on a real mbuf";
    EXPECT_EQ(aloe::wire::internet_checksum(frames[0].ipv4_header), 0U);
    peer_.see(frames[0]);
    receive(peer_.ack());
    const auto event = tcp_.poll_event();
    ASSERT_TRUE(event.has_value());
    ASSERT_TRUE(event->events.accepted());
    auto& c = *event->connection;
    EXPECT_EQ(c.mss(), 1460U);

    receive(aloe::frames::with_ipv4_options(peer_.data(text("hello ring")),
                                            2));  // options: a nonzero L4 offset in a real mbuf
    EXPECT_EQ(c.unread().size(), 10U);
    std::vector<std::byte> seen;
    for (const std::span<const std::byte> chunk : c.unread()) {
        seen.insert(seen.end(), chunk.begin(), chunk.end());
    }
    EXPECT_EQ(seen, text("hello ring"));
    c.consume(6);
    EXPECT_EQ(c.unread().size(), 4U) << "partial consumption keeps the mbuf";
    const auto reply = text("ring");
    const auto out   = c.prepare(reply.size());
    ASSERT_TRUE(out.has_value());
    std::ranges::copy(reply, out->begin());
    ASSERT_TRUE(c.commit(reply.size()));
    frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "the reply carried the ACK";
    EXPECT_EQ(frames[0].tcp->flags, (TcpFlags{TcpFlag::Psh, TcpFlag::Ack}));
    EXPECT_EQ(frames[0].tcp->acknowledgement, peer_.snd_nxt());
    EXPECT_EQ(frames[0].ipv4->total_length, 44U);
    EXPECT_EQ(aloe::frames::l4_checksum_residue(*frames[0].ipv4, frames[0].l4), 0U);
    const auto payload = aloe::frames::tcp_payload(frames[0]);
    EXPECT_EQ(std::vector<std::byte>(payload.begin(), payload.end()), reply);
    c.consume(4);

    receive(peer_.ack(frames[0]));
    EXPECT_EQ(c.unacknowledged(), 0U);
    receive(peer_.fin());
    EXPECT_TRUE(c.peer_closed());
    c.close();
    frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->flags, (TcpFlags{TcpFlag::Fin, TcpFlag::Ack}));
    receive(peer_.ack(frames[0]));
    EXPECT_EQ(c.state(), aloe::tcp::State::Closed);
    c.release();
    EXPECT_EQ(wheel_.pending(), 0U);
    EXPECT_EQ(tcp_.table_size(), 0U);
    EXPECT_EQ(tcp_.nodes_available(), tcp_.config().receive_pool) << "every mbuf went back";
    EXPECT_EQ(ip_.counters().dropped_martian, 0U);
    EXPECT_EQ(tcp_.counters().dropped_bad_checksum, 0U);
    EXPECT_EQ(tcp_.counters().connections_closed, 1U);
}

TEST_F(TcpRing, ActiveOpenToScriptedPeer) {
    const auto connected = tcp_.connect({.address = peer_ip, .port = 40000}, now_);
    ASSERT_TRUE(connected.has_value());
    auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].tcp->flags, TcpFlags{TcpFlag::Syn});
    EXPECT_EQ(aloe::frames::l4_checksum_residue(*frames[0].ipv4, frames[0].l4), 0U);
    receive(peer_.syn_ack(frames[0]));
    const auto event = tcp_.poll_event();
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->events.connected());
    frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "the deferred handshake ACK";
    EXPECT_EQ(frames[0].tcp->flags, TcpFlags{TcpFlag::Ack});
    EXPECT_EQ(frames[0].ipv4->total_length, 40U);
    peer_.see(frames[0]);
    const auto payload = aloe::frames::pattern(2000);
    EXPECT_EQ((*connected)->send(payload), 2000U);
    frames = collect();
    ASSERT_EQ(frames.size(), 2U) << "1460 and 540";
    EXPECT_EQ(aloe::frames::tcp_payload(frames[0]).size(), 1460U);
    EXPECT_EQ(aloe::frames::tcp_payload(frames[1]).size(), 540U);
    EXPECT_EQ(frames[0].ipv4->total_length, 1500U) << "a full MTU on a real mbuf";
    for (const auto& frame : frames) {
        EXPECT_EQ(aloe::frames::l4_checksum_residue(*frame.ipv4, frame.l4), 0U);
    }
    receive(peer_.ack(frames[1]));
    EXPECT_EQ((*connected)->acknowledged(), (*connected)->committed());
    (*connected)->release();
    std::ignore = collect();
    EXPECT_EQ(wheel_.pending(), 0U);
}
