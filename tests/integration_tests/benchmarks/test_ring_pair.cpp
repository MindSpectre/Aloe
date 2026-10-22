#include <aloe/ethdev>
#include <aloe/frames>
#include <aloe/wire>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <eal_environment.hpp>
#include <gtest/gtest.h>
#include <ring_pair.hpp>

// Two ethdev ports over a pair of SPSC rings: what the peer transmits the stack receives and nothing loops back,
// so the benchmark's peer can live on another core. Real mbufs, no root, no hugepages.
namespace {

    using aloe::benchmarks::RingPair;
    using Packet = aloe::ethdev::Packet;

    const auto* const environment = ::testing::AddGlobalTestEnvironment(new aloe::testing::EalEnvironment{"net_null0"});

    constexpr aloe::wire::MacAddress alpha{0x02, 0, 0, 0, 0, 0x0a};
    constexpr aloe::wire::MacAddress beta{0x02, 0, 0, 0, 0, 0x0b};

    /// Transmits `frame` on `queue` of `port` from a fresh packet of its pool.
    void send(aloe::ethdev::Port& port, const std::span<const std::byte> frame, const std::uint16_t queue = 0) {
        auto packet = port.allocate(queue);
        ASSERT_TRUE(packet.has_value());
        ASSERT_TRUE(aloe::frames::fill(*packet, frame));
        std::array<Packet, 1> burst{std::move(*packet)};
        ASSERT_EQ(port.transmit(queue, burst), 1U);
        EXPECT_TRUE(burst[0].empty()) << "an accepted packet is moved from";
    }

    /// Everything pending on `queue` of `port`, as frames, freed.
    [[nodiscard]] std::vector<std::vector<std::byte>> receive_all(aloe::ethdev::Port& port,
                                                                  const std::uint16_t queue = 0) {
        std::vector<std::vector<std::byte>> frames;
        std::array<Packet, 16> burst{};
        for (std::size_t got = port.receive(queue, burst); got > 0; got = port.receive(queue, burst)) {
            for (Packet& packet : std::span{burst}.first(got)) {
                frames.push_back(aloe::frames::bytes_of(packet));
                packet = Packet{};
            }
        }
        return frames;
    }

}  // namespace

TEST(RingPair, FramesCrossFromPeerToStackAndBackAndNeverLoop) {
    RingPair pair{
        {.name = "ring-pair-cross", .pool_size = 256}
    };
    const auto frame =
        aloe::frames::ethernet_frame(alpha, beta, aloe::frames::ethertype_experimental, aloe::frames::pattern(60));
    send(pair.peer(), frame);
    const auto at_stack = receive_all(pair.stack());
    ASSERT_EQ(at_stack.size(), 1U);
    EXPECT_EQ(at_stack[0], frame);
    EXPECT_TRUE(receive_all(pair.peer()).empty()) << "a vdev would loop it back; the pair must not";
    send(pair.stack(), frame);
    const auto at_peer = receive_all(pair.peer());
    ASSERT_EQ(at_peer.size(), 1U);
    EXPECT_EQ(at_peer[0], frame);
    EXPECT_TRUE(receive_all(pair.stack()).empty());
}

TEST(RingPair, NoChecksumOffloadAndNoSteering) {
    RingPair pair{
        {.name = "ring-pair-caps", .pool_size = 256}
    };
    EXPECT_FALSE(pair.stack().capabilities().tx_l4_checksum) << "the stack checksums in software on the ring";
    EXPECT_FALSE(pair.stack().capabilities().rx_l4_checksum);
    EXPECT_FALSE(pair.stack().capabilities().tx_ipv4_checksum);
    EXPECT_FALSE(pair.stack().steering().enabled);
    EXPECT_EQ(pair.stack().queue_count(), 1U);
}

TEST(RingPair, DrainFreesBothDirections) {
    RingPair pair{
        {.name = "ring-pair-drain", .pool_size = 256}
    };
    const auto frame =
        aloe::frames::ethernet_frame(alpha, beta, aloe::frames::ethertype_experimental, aloe::frames::pattern(20));
    send(pair.peer(), frame);
    send(pair.peer(), frame);
    send(pair.stack(), frame);
    pair.drain();
    EXPECT_TRUE(receive_all(pair.stack()).empty());
    EXPECT_TRUE(receive_all(pair.peer()).empty());
}

TEST(RingPair, QueuesStayApart) {
    RingPair pair{
        {.name = "ring-pair-queues", .queues = 2, .pool_size = 256}
    };
    const auto frame =
        aloe::frames::ethernet_frame(alpha, beta, aloe::frames::ethertype_experimental, aloe::frames::pattern(20));
    send(pair.peer(), frame, 1);
    EXPECT_TRUE(receive_all(pair.stack(), 0).empty());
    EXPECT_EQ(receive_all(pair.stack(), 1).size(), 1U);
}
