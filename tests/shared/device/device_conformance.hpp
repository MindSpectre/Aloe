#pragma once

#include <algorithm>
#include <aloe/device>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <frames.hpp>
#include <gtest/gtest.h>

/**
 * The conformance suite every device backend passes.
 *
 * A fixture type yields one single-queue device on which a frame addressed to
 * the device's own MAC and transmitted on queue 0 comes back on queue 0. The
 * fixture is default constructible and has `device()`, returning a reference
 * to a type that models IsDevice. Instantiate the suite in a test file with
 *
 *     INSTANTIATE_TYPED_TEST_SUITE_P(Name, DeviceConformance, Fixture);
 *
 * inside namespace aloe::testing, or with the suite name qualified.
 */
namespace aloe::testing {

    template <typename Fixture>
    class DeviceConformance : public ::testing::Test {
    protected:
        using Device = std::remove_cvref_t<decltype(std::declval<Fixture&>().device())>;
        using Packet = typename Device::Packet;
        static_assert(IsDevice<Device>);

        static constexpr MacAddress peer{0x02, 0, 0, 0, 0xfe, 0xed};
        static constexpr std::size_t burst_size = 32;

        Fixture fixture_;

        Device& device() {
            return fixture_.device();
        }

        /// A frame to the device's own MAC with `payload` bytes of pattern.
        std::vector<std::byte> frame(std::size_t payload, std::uint8_t seed = 0) {
            return ethernet_frame(device().mac(), peer, ethertype_experimental, pattern(payload, seed));
        }

        /// A packet from queue 0 holding `bytes`.
        Packet packet_with(std::span<const std::byte> bytes) {
            auto packet = device().allocate(0);
            EXPECT_TRUE(packet.has_value());
            if (!packet) {
                return Packet{};
            }
            EXPECT_TRUE(fill(*packet, bytes));
            return std::move(*packet);
        }

        /// Polls queue 0 until `count` packets arrived or `polls` polls came back empty.
        std::vector<Packet> receive_up_to(std::size_t count, int polls = 1000) {
            std::vector<Packet> result;
            result.reserve(count);
            std::array<Packet, burst_size> burst;
            while (result.size() < count && polls > 0) {
                const std::size_t wanted = std::min(burst.size(), count - result.size());
                const std::size_t got    = device().receive(0, std::span{burst}.first(wanted));
                if (got == 0) {
                    --polls;
                }
                for (std::size_t index = 0; index < got; ++index) {
                    result.push_back(std::move(burst[index]));
                }
            }
            return result;
        }
    };

    TYPED_TEST_SUITE_P(DeviceConformance);

    TYPED_TEST_P(DeviceConformance, CapabilitiesAreSane) {
        auto& device = this->device();
        EXPECT_EQ(device.queue_count(), 1);
        EXPECT_GE(device.capabilities().max_rx_queues, 1);
        EXPECT_GE(device.capabilities().max_tx_queues, 1);
        EXPECT_GE(device.mtu(), 68) << "the IPv4 minimum";
        EXPECT_GE(device.mtu(), device.capabilities().min_mtu);
        EXPECT_LE(device.mtu(), device.capabilities().max_mtu);
        EXPECT_LE(device.queue_count(), device.capabilities().max_rx_queues);
        EXPECT_LE(device.queue_count(), device.capabilities().max_tx_queues);
        EXPECT_TRUE(device.link_up());
        EXPECT_FALSE(device.steering().enabled) << "one queue has nothing to steer between";
        EXPECT_FALSE(device.mac().is_multicast());
    }

    TYPED_TEST_P(DeviceConformance, AllocateYieldsAnEmptyPacketWithFullHeadroom) {
        auto packet = this->device().allocate(0);
        ASSERT_TRUE(packet.has_value());
        EXPECT_FALSE(packet->empty());
        EXPECT_EQ(packet->size(), 0);
        EXPECT_TRUE(packet->data().empty());
        EXPECT_EQ(packet->headroom(), packet_headroom);
        EXPECT_GE(packet->tailroom(), static_cast<std::size_t>(this->device().mtu()) + ethernet_header_size);
        EXPECT_EQ(packet->rx(), RxMetadata{});
        EXPECT_EQ(packet->tx(), TxMetadata{});
    }

    TYPED_TEST_P(DeviceConformance, PacketsGrowAndShrinkWithinTheirRoom) {
        auto packet = this->device().allocate(0);
        ASSERT_TRUE(packet.has_value());
        const std::size_t tailroom = packet->tailroom();

        const auto payload = packet->append(100);
        ASSERT_TRUE(payload.has_value());
        EXPECT_EQ(payload->size(), 100);
        const auto header = packet->prepend(14);
        ASSERT_TRUE(header.has_value());
        EXPECT_EQ(header->size(), 14);
        EXPECT_EQ(packet->size(), 114);
        EXPECT_EQ(packet->headroom(), packet_headroom - 14);
        EXPECT_EQ(packet->tailroom(), tailroom - 100);
        EXPECT_EQ(header->data(), packet->data().data()) << "the header is the front of the data";

        EXPECT_FALSE(packet->prepend(packet_headroom).has_value()) << "past the headroom";
        EXPECT_FALSE(packet->append(tailroom).has_value()) << "past the tailroom";
        EXPECT_EQ(packet->size(), 114) << "a refused growth changes nothing";

        (*header)[0] = std::byte{0xaa};
        EXPECT_EQ(packet->data()[0], std::byte{0xaa}) << "the new bytes are the packet's own";
        packet->trim_front(14);
        EXPECT_EQ(packet->size(), 100);
        EXPECT_EQ(packet->headroom(), packet_headroom);
        EXPECT_EQ(packet->data().data(), payload->data());
        packet->trim_back(100);
        EXPECT_EQ(packet->size(), 0);
        EXPECT_EQ(packet->tailroom(), tailroom);
    }

    TYPED_TEST_P(DeviceConformance, ATransmittedFrameComesBackByteForByte) {
        const auto frame = this->frame(100, 7);
        std::array<typename TestFixture::Packet, 1> burst{this->packet_with(frame)};
        ASSERT_EQ(this->device().transmit(0, burst), 1);
        EXPECT_TRUE(burst[0].empty()) << "an accepted packet is moved from";

        auto received = this->receive_up_to(1);
        ASSERT_EQ(received.size(), 1);
        EXPECT_EQ(bytes_of(received[0]), frame);
        EXPECT_FALSE(received[0].rx().rss_hash.has_value());
        EXPECT_EQ(received[0].rx().l3, ChecksumVerdict::Unknown);
        EXPECT_EQ(received[0].rx().l4, ChecksumVerdict::Unknown);
        EXPECT_EQ(received[0].headroom(), packet_headroom) << "a received frame keeps its headroom";
        EXPECT_EQ(received[0].tx(), TxMetadata{}) << "a received frame asks for no transmit offloads";
    }

    TYPED_TEST_P(DeviceConformance, AMaximumSizeFrameComesBack) {
        const auto frame = this->frame(this->device().mtu(), 3);
        EXPECT_EQ(frame.size(), static_cast<std::size_t>(this->device().mtu()) + ethernet_header_size);
        std::array<typename TestFixture::Packet, 1> burst{this->packet_with(frame)};
        ASSERT_EQ(this->device().transmit(0, burst), 1);
        auto received = this->receive_up_to(1);
        ASSERT_EQ(received.size(), 1);
        EXPECT_EQ(bytes_of(received[0]), frame);
    }

    TYPED_TEST_P(DeviceConformance, AnOversizedFrameIsDroppedAndCounted) {
        const auto frame = this->frame(static_cast<std::size_t>(this->device().mtu()) + 1);
        std::array<typename TestFixture::Packet, 1> burst{this->packet_with(frame)};
        ASSERT_EQ(this->device().transmit(0, burst), 1);
        EXPECT_TRUE(this->receive_up_to(1, 20).empty());
        EXPECT_EQ(this->device().counters(0).oversized, 1);
    }

    TYPED_TEST_P(DeviceConformance, ARuntIsRefusedAndCountedByTheTransmitter) {
        const QueueCounters before = this->device().counters(0);
        const std::vector<std::byte> runt(ethernet_header_size - 1);
        std::array<typename TestFixture::Packet, 1> burst{this->packet_with(runt)};
        ASSERT_EQ(this->device().transmit(0, burst), 1) << "a refused frame counts as accepted";
        EXPECT_TRUE(burst[0].empty());
        EXPECT_TRUE(this->receive_up_to(1, 20).empty());
        const QueueCounters after = this->device().counters(0);
        EXPECT_EQ(after.oversized, before.oversized + 1);
        EXPECT_EQ(after.transmitted, before.transmitted);
    }

    TYPED_TEST_P(DeviceConformance, ABurstComesBackInOrder) {
        constexpr std::size_t count = 8;
        std::vector<std::vector<std::byte>> frames;
        std::array<typename TestFixture::Packet, count> burst;
        for (std::size_t index = 0; index < count; ++index) {
            frames.push_back(this->frame(50 + index, static_cast<std::uint8_t>(index)));
            burst[index] = this->packet_with(frames.back());
        }
        ASSERT_EQ(this->device().transmit(0, burst), count);
        auto received = this->receive_up_to(count);
        ASSERT_EQ(received.size(), count);
        for (std::size_t index = 0; index < count; ++index) {
            EXPECT_EQ(bytes_of(received[index]), frames[index]) << "frame " << index;
        }
    }

    TYPED_TEST_P(DeviceConformance, TransmitAcceptsAtMostWhatIsOfferedAndLeavesTheRestUntouched) {
        // Whether a backend ever accepts part of a burst is tested per backend, under back-pressure;
        // here every packet it did not accept must be exactly as it was offered.
        const std::vector<std::vector<std::byte>> frames{this->frame(20, 1), this->frame(20, 2), this->frame(20, 3)};
        std::array<typename TestFixture::Packet, 3> burst{
            this->packet_with(frames[0]), this->packet_with(frames[1]), this->packet_with(frames[2])};
        const std::size_t accepted = this->device().transmit(0, burst);
        EXPECT_LE(accepted, burst.size());
        for (std::size_t index = 0; index < burst.size(); ++index) {
            EXPECT_EQ(burst[index].empty(), index < accepted) << "slot " << index;
            if (index >= accepted) {
                EXPECT_EQ(bytes_of(burst[index]), frames[index]) << "slot " << index;
                EXPECT_EQ(burst[index].tx(), TxMetadata{}) << "slot " << index;
            }
        }
        auto received = this->receive_up_to(accepted);
        ASSERT_EQ(received.size(), accepted);
        for (std::size_t index = 0; index < accepted; ++index) {
            EXPECT_EQ(bytes_of(received[index]), frames[index]) << "frame " << index;
        }
    }

    TYPED_TEST_P(DeviceConformance, EmptySlotsInABurstAreSkipped) {
        const QueueCounters before = this->device().counters(0);
        const std::vector<std::vector<std::byte>> frames{this->frame(20, 1), this->frame(20, 2)};
        std::array<typename TestFixture::Packet, 3> burst{
            this->packet_with(frames[0]), typename TestFixture::Packet{}, this->packet_with(frames[1])};
        EXPECT_EQ(this->device().transmit(0, burst), 3);
        auto received = this->receive_up_to(2);
        ASSERT_EQ(received.size(), 2);
        EXPECT_EQ(bytes_of(received[0]), frames[0]);
        EXPECT_EQ(bytes_of(received[1]), frames[1]);
        EXPECT_TRUE(this->receive_up_to(1, 20).empty()) << "nothing was sent for the empty slot";
        EXPECT_EQ(this->device().counters(0).transmitted, before.transmitted + 2);
    }

    TYPED_TEST_P(DeviceConformance, CountersCount) {
        const QueueCounters before = this->device().counters(0);
        std::array<typename TestFixture::Packet, 2> burst{this->packet_with(this->frame(20, 1)),
                                                          this->packet_with(this->frame(20, 2))};
        ASSERT_EQ(this->device().transmit(0, burst), 2);
        ASSERT_EQ(this->receive_up_to(2).size(), 2);
        const QueueCounters after = this->device().counters(0);
        EXPECT_EQ(after.transmitted, before.transmitted + 2);
        EXPECT_EQ(after.received, before.received + 2);
        EXPECT_EQ(after.dropped, before.dropped);
        EXPECT_EQ(after.oversized, before.oversized);
    }

    TYPED_TEST_P(DeviceConformance, ThePoolRunsOutAndRecovers) {
        std::vector<typename TestFixture::Packet> held;
        constexpr std::size_t cap = 1'000'000;
        while (held.size() < cap) {
            auto packet = this->device().allocate(0);
            if (!packet) {
                break;
            }
            held.push_back(std::move(*packet));
        }
        EXPECT_GE(held.size(), 2);
        EXPECT_LT(held.size(), cap) << "the pool never ran out";
        EXPECT_FALSE(this->device().allocate(0).has_value());
        held.clear();
        EXPECT_TRUE(this->device().allocate(0).has_value());
    }

    TYPED_TEST_P(DeviceConformance, ReceiveOnAnIdleQueueReturnsNothing) {
        std::array<typename TestFixture::Packet, 4> burst;
        EXPECT_EQ(this->device().receive(0, burst), 0);
        for (const auto& packet : burst) {
            EXPECT_TRUE(packet.empty());
        }
    }

    TYPED_TEST_P(DeviceConformance, MovedFromPacketsAreEmpty) {
        auto packet = this->device().allocate(0);
        ASSERT_TRUE(packet.has_value());
        ASSERT_TRUE(packet->append(10).has_value());
        typename TestFixture::Packet moved{std::move(*packet)};
        EXPECT_TRUE(packet->empty());  // NOLINT(bugprone-use-after-move): the moved-from state is the point
        EXPECT_EQ(packet->size(), 0);
        EXPECT_EQ(moved.size(), 10);
        *packet = std::move(moved);
        EXPECT_TRUE(moved.empty());  // NOLINT(bugprone-use-after-move)
        EXPECT_EQ(packet->size(), 10);
    }

    REGISTER_TYPED_TEST_SUITE_P(DeviceConformance,
                                CapabilitiesAreSane,
                                AllocateYieldsAnEmptyPacketWithFullHeadroom,
                                PacketsGrowAndShrinkWithinTheirRoom,
                                ATransmittedFrameComesBackByteForByte,
                                AMaximumSizeFrameComesBack,
                                AnOversizedFrameIsDroppedAndCounted,
                                ARuntIsRefusedAndCountedByTheTransmitter,
                                ABurstComesBackInOrder,
                                TransmitAcceptsAtMostWhatIsOfferedAndLeavesTheRestUntouched,
                                EmptySlotsInABurstAreSkipped,
                                CountersCount,
                                ThePoolRunsOutAndRecovers,
                                ReceiveOnAnIdleQueueReturnsNothing,
                                MovedFromPacketsAreEmpty);

}  // namespace aloe::testing
