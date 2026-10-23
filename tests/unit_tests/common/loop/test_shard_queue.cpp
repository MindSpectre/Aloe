#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/loop>
#include <aloe/wire>
#include <cstddef>
#include <optional>
#include <utility>

#include <gtest/gtest.h>

namespace {

    constexpr aloe::wire::MacAddress mac{0x02, 0, 0, 0, 0, 0x01};

    struct QueueFixture : ::testing::Test {
        aloe::fabric::Fabric fabric;
        aloe::fabric::Port& port = fabric.add_port({.mac = mac, .pool_size = 4});
        aloe::loop::ShardCounters counters;
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
