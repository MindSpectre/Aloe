#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/loop>
#include <aloe/wire>
#include <array>
#include <cstddef>
#include <optional>
#include <utility>

#include <gtest/gtest.h>

namespace {

    constexpr aloe::wire::MacAddress mac{0x02, 0, 0, 0, 0, 0x01};

    struct QueueFixture : ::testing::Test {
        aloe::fabric::Fabric fabric;
        aloe::fabric::Port& port = fabric.add_port({.mac = mac, .pool_size = 4});
        aloe::loop::ShardQueueCounters counters;
        aloe::loop::ShardQueue<aloe::fabric::Port> queue{port, 0, 4, counters};

        /// A packet of the queue holding a frame to the port's own MAC.
        std::optional<aloe::fabric::Packet> frame_packet() {
            auto packet = queue.allocate();
            if (packet) {
                EXPECT_TRUE(
                    aloe::frames::fill(*packet,
                                       aloe::frames::ethernet_frame(
                                           mac, mac, aloe::frames::ethertype_experimental, aloe::frames::pattern(20))));
            }
            return packet;
        }
    };

}  // namespace

TEST_F(QueueFixture, ReclaimReachesTheDevicesQueue) {
    auto lender = frame_packet();
    ASSERT_TRUE(lender.has_value());
    ASSERT_TRUE(queue.transmit(lender->lend()));
    EXPECT_TRUE(lender->lent()) << "in the ring";
    EXPECT_EQ(queue.flush(), 1);
    EXPECT_TRUE(lender->lent()) << "in the device's in-flight list";
    EXPECT_EQ(queue.reclaim(), 1);
    EXPECT_FALSE(lender->lent());
}

TEST_F(QueueFixture, DiscardReturnsALoan) {
    auto lender = frame_packet();
    ASSERT_TRUE(lender.has_value());
    ASSERT_TRUE(queue.transmit(lender->lend()));
    queue.discard();
    EXPECT_FALSE(lender->lent()) << "a dropped handle returns the loan at once";
    EXPECT_EQ(counters.transmit_refused, 1);
}

TEST_F(QueueFixture, ReceiveCountsTheFramesItReturns) {
    constexpr aloe::wire::MacAddress peer_mac{0x02, 0, 0, 0, 0, 0x02};
    aloe::fabric::Port& peer = fabric.add_port({.mac = peer_mac, .pool_size = 4});
    auto packet              = peer.allocate(0);
    ASSERT_TRUE(packet.has_value());
    ASSERT_TRUE(aloe::frames::fill(
        *packet,
        aloe::frames::ethernet_frame(mac, peer_mac, aloe::frames::ethertype_experimental, aloe::frames::pattern(20))));
    std::array<aloe::fabric::Packet, 1> sent{std::move(*packet)};
    ASSERT_EQ(peer.transmit(0, sent), 1);

    std::array<aloe::fabric::Packet, 4> burst;
    EXPECT_EQ(queue.receive(burst), 1);
    EXPECT_EQ(counters.frames_received, 1);
    std::array<aloe::fabric::Packet, 4> empty;
    EXPECT_EQ(queue.receive(empty), 0);
    EXPECT_EQ(counters.frames_received, 1) << "an empty receive counts nothing";
}
