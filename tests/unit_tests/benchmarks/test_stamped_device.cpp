#include <aloe/device>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/wire>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

#include <gtest/gtest.h>
#include <stamped_device.hpp>

// The wrapper every level's device goes through: it stamps the one transmit the application armed and nothing
// else, so a pure ACK leaving in an early flush never closes a round. Over a fabric port: no DPDK.
namespace {

    using Stamped = aloe::benchmarks::StampedDevice<aloe::fabric::Port>;
    using Packet  = aloe::fabric::Packet;

    constexpr aloe::wire::MacAddress mac{0x02, 0, 0, 0, 0, 0x01};
    constexpr std::uint64_t untouched = std::numeric_limits<std::uint64_t>::max();

    static_assert(aloe::device::IsDevice<Stamped>, "the wrapper stands in for the port everywhere");

    void send(Stamped& device, const std::span<const std::byte> frame) {
        auto packet = device.allocate(0);
        ASSERT_TRUE(packet.has_value());
        ASSERT_TRUE(aloe::frames::fill(*packet, frame));
        std::array<Packet, 1> burst{std::move(*packet)};
        ASSERT_EQ(device.transmit(0, burst), 1U);
    }

    class StampedDeviceTest : public ::testing::Test {
    protected:
        aloe::fabric::Fabric fabric_;
        aloe::fabric::Port& port_ = fabric_.add_port({.mac = mac, .queues = 1, .pool_size = 16});
        std::array<std::uint64_t, 4> stops_{untouched, untouched, untouched, untouched};
        Stamped device_{port_, stops_};
        std::vector<std::byte> frame_ =
            aloe::frames::ethernet_frame(mac, mac, aloe::frames::ethertype_experimental, aloe::frames::pattern(46));
    };

}  // namespace

TEST_F(StampedDeviceTest, StampsOnlyTheArmedTransmit) {
    send(device_, frame_);
    EXPECT_EQ(device_.stamps_taken(), 0U) << "unarmed: a pure ACK or a control segment";
    device_.arm(2);
    EXPECT_TRUE(device_.armed());
    send(device_, frame_);
    EXPECT_EQ(device_.stamps_taken(), 1U);
    EXPECT_FALSE(device_.armed());
    EXPECT_NE(stops_[2], untouched);
    EXPECT_EQ(stops_[0], untouched);
    EXPECT_EQ(stops_[1], untouched);
    EXPECT_EQ(stops_[3], untouched);
    send(device_, frame_);
    EXPECT_EQ(device_.stamps_taken(), 1U) << "the arm is spent";
}

TEST_F(StampedDeviceTest, ATransmitTheDeviceTakesNothingOfKeepsTheArm) {
    device_.arm(0);
    std::array<Packet, 0> none{};
    EXPECT_EQ(device_.transmit(0, none), 0U);
    EXPECT_TRUE(device_.armed());
    EXPECT_EQ(device_.stamps_taken(), 0U);
}

TEST_F(StampedDeviceTest, ForwardsTheDevice) {
    EXPECT_EQ(device_.mac(), port_.mac());
    EXPECT_EQ(device_.queue_count(), 1U);
    EXPECT_EQ(device_.mtu(), port_.mtu());
    EXPECT_EQ(&device_.device(), &port_);
}
