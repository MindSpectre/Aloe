#include <aloe/fabric>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

    constexpr std::size_t capacity = 256;

}  // namespace

TEST(FabricPacket, AllocatesEmptyPacketsWithFullHeadroom) {
    aloe::fabric::Pool pool{2, capacity};
    auto packet = pool.allocate();
    ASSERT_TRUE(packet.has_value());
    EXPECT_FALSE(packet->empty());
    EXPECT_EQ(packet->size(), 0);
    EXPECT_EQ(packet->headroom(), aloe::packet_headroom);
    EXPECT_EQ(packet->tailroom(), capacity);
    EXPECT_EQ(packet->capacity(), aloe::packet_headroom + capacity);
    EXPECT_TRUE(packet->data().empty());
    EXPECT_EQ(pool.available(), 1);
}

TEST(FabricPacket, GrowsIntoRoomAndRefusesPastIt) {
    aloe::fabric::Pool pool{1, capacity};
    auto packet = pool.allocate();
    ASSERT_TRUE(packet.has_value());

    const auto payload = packet->append(100);
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(payload->size(), 100);
    const auto header = packet->prepend(14);
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->size(), 14);
    EXPECT_EQ(packet->size(), 114);
    EXPECT_EQ(packet->headroom(), aloe::packet_headroom - 14);
    EXPECT_EQ(packet->tailroom(), capacity - 100);
    EXPECT_EQ(header->data(), packet->data().data()) << "the header is the front of the data";

    EXPECT_FALSE(packet->prepend(aloe::packet_headroom).has_value());
    EXPECT_FALSE(packet->append(capacity).has_value());
    EXPECT_EQ(packet->size(), 114) << "a refused growth changes nothing";

    packet->trim_front(14);
    packet->trim_back(100);
    EXPECT_EQ(packet->size(), 0);
    EXPECT_EQ(packet->headroom(), aloe::packet_headroom);
}

TEST(FabricPacket, KeepsBytesWhereTheyWereWritten) {
    aloe::fabric::Pool pool{1, capacity};
    auto packet = pool.allocate();
    ASSERT_TRUE(packet.has_value());
    auto room = packet->append(4);
    ASSERT_TRUE(room.has_value());
    (*room)[0] = std::byte{1};
    (*room)[3] = std::byte{4};
    auto front = packet->prepend(1);
    ASSERT_TRUE(front.has_value());
    (*front)[0] = std::byte{9};
    EXPECT_EQ(packet->data()[0], std::byte{9});
    EXPECT_EQ(packet->data()[1], std::byte{1});
    EXPECT_EQ(packet->data()[4], std::byte{4});
}

TEST(FabricPacket, ReturnsItsBlockWhenDestroyedOrAssignedOver) {
    aloe::fabric::Pool pool{1, capacity};
    {
        auto packet = pool.allocate();
        ASSERT_TRUE(packet.has_value());
        EXPECT_EQ(pool.available(), 0);
        EXPECT_FALSE(pool.allocate().has_value()) << "the pool is exhausted";
    }
    EXPECT_EQ(pool.available(), 1);

    auto packet = pool.allocate();
    ASSERT_TRUE(packet.has_value());
    *packet = aloe::fabric::Packet{};
    EXPECT_TRUE(packet->empty());
    EXPECT_EQ(pool.available(), 1);
}

TEST(FabricPacket, MovingLeavesTheSourceEmpty) {
    aloe::fabric::Pool pool{1, capacity};
    auto packet = pool.allocate();
    ASSERT_TRUE(packet.has_value());
    ASSERT_TRUE(packet->append(3).has_value());
    packet->set_tx(aloe::TxMetadata{.l2_length = 14});

    aloe::fabric::Packet moved{std::move(*packet)};
    EXPECT_TRUE(packet->empty());  // NOLINT(bugprone-use-after-move): the moved-from state is the point
    EXPECT_EQ(packet->size(), 0);
    EXPECT_FALSE(moved.empty());
    EXPECT_EQ(moved.size(), 3);
    EXPECT_EQ(moved.tx().l2_length, 14);
    EXPECT_EQ(pool.available(), 0);
}

TEST(FabricPacket, EmptyPacketsHaveNoRoom) {
    aloe::fabric::Packet packet;
    EXPECT_TRUE(packet.empty());
    EXPECT_EQ(packet.size(), 0);
    EXPECT_EQ(packet.headroom(), 0);
    EXPECT_EQ(packet.tailroom(), 0);
    EXPECT_FALSE(packet.prepend(1).has_value());
    EXPECT_FALSE(packet.append(1).has_value());
    EXPECT_EQ(packet.rx(), aloe::RxMetadata{});
}

TEST(FabricPacket, EveryBlockIsHandedOutOnce) {
    aloe::fabric::Pool pool{8, capacity};
    std::vector<aloe::fabric::Packet> packets;
    std::vector<const std::byte*> addresses;
    while (auto packet = pool.allocate()) {
        ASSERT_TRUE(packet->append(1).has_value());
        addresses.push_back(packet->data().data());
        packets.push_back(std::move(*packet));
    }
    EXPECT_EQ(packets.size(), 8);
    for (std::size_t first = 0; first < addresses.size(); ++first) {
        for (std::size_t second = first + 1; second < addresses.size(); ++second) {
            EXPECT_NE(addresses[first], addresses[second]);
        }
    }
    packets.clear();
    EXPECT_EQ(pool.available(), 8);
}
