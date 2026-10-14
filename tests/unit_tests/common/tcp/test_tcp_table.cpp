#include <aloe/device>
#include <aloe/tcp>
#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using aloe::tcp::FlowKey;
    using aloe::tcp::FlowTable;
    using aloe::tcp::detail::mix_flow_hash;
    using aloe::tcp::detail::software_flow_hash;

    constexpr aloe::wire::Ipv4Address peer{10, 0, 0, 1};
    constexpr aloe::wire::Ipv4Address other{10, 0, 0, 2};

    [[nodiscard]] FlowKey key(const std::uint16_t remote_port, const std::uint16_t local_port = 7) {
        return {.remote = peer, .remote_port = remote_port, .local_port = local_port};
    }

}  // namespace

TEST(TcpTable, CapacityIsTheNextPowerOfTwoAtLeastTwiceTheConnections) {
    EXPECT_EQ(FlowTable{4}.capacity(), 8U);
    EXPECT_EQ(FlowTable{5}.capacity(), 16U);
    EXPECT_EQ(FlowTable{1024}.capacity(), 2048U);
    EXPECT_EQ(FlowTable{1}.capacity(), 2U);
    EXPECT_THROW((std::ignore = FlowTable{0}), std::invalid_argument);
    EXPECT_THROW((std::ignore = FlowTable{std::size_t{1} << 31U}), std::invalid_argument);
}

TEST(TcpTable, FixedVectorsForTheMixerAndTheSoftwareHash) {
    EXPECT_EQ(mix_flow_hash(0U), 0U);
    EXPECT_EQ(mix_flow_hash(1U), 0x514e28b7U);
    EXPECT_EQ(mix_flow_hash(0xdeadbeefU), 0x0de5c6a9U);
    EXPECT_EQ(mix_flow_hash(0x80000000U), 0x6d3c65a0U);
    EXPECT_EQ(software_flow_hash(key(40000)), 0x12854375U);
    EXPECT_EQ(software_flow_hash(key(40001)), 0x4f6bf39aU);
    EXPECT_EQ(software_flow_hash({.remote = other, .remote_port = 40000, .local_port = 7}), 0x43a64bdcU);
    EXPECT_EQ(software_flow_hash({}), 0x9be17165U);
    EXPECT_NE(software_flow_hash(key(40000, 7)), software_flow_hash(key(7, 40000))) << "ports are not symmetric";
}

// Base hashes 0x1, 0x3 and 0x12 have home bucket 7 of 8 after mixing; 0x6 has home 0. The chain from 7
// wraps to 0 and 1, and the entry homed at 0 is pushed to 2. Deleting the head shifts the three behind it.
TEST(TcpTable, BackwardShiftAcrossWrap) {
    FlowTable table{4};
    ASSERT_EQ(table.capacity(), 8U);
    ASSERT_TRUE(table.insert(key(1), 10, 0x1));
    ASSERT_TRUE(table.insert(key(2), 11, 0x3));
    ASSERT_TRUE(table.insert(key(3), 12, 0x12));
    ASSERT_TRUE(table.insert(key(4), 13, 0x6));
    EXPECT_EQ(table.size(), 4U);
    EXPECT_EQ(table.find(key(1), 0x1), 10U);
    EXPECT_EQ(table.find(key(4), 0x6), 13U);

    EXPECT_TRUE(table.erase(key(1), 0x1));  // the head of the chain
    EXPECT_FALSE(table.find(key(1), 0x1));
    EXPECT_EQ(table.find(key(2), 0x3), 11U);
    EXPECT_EQ(table.find(key(3), 0x12), 12U);
    EXPECT_EQ(table.find(key(4), 0x6), 13U);

    EXPECT_TRUE(table.erase(key(3), 0x12));  // the middle
    EXPECT_EQ(table.find(key(2), 0x3), 11U);
    EXPECT_EQ(table.find(key(4), 0x6), 13U);
    EXPECT_FALSE(table.find(key(3), 0x12));

    ASSERT_TRUE(table.insert(key(5), 14, 0x6)) << "insert after deletion reuses the shifted holes";
    EXPECT_EQ(table.find(key(5), 0x6), 14U);
    EXPECT_TRUE(table.erase(key(5), 0x6));  // the tail
    EXPECT_TRUE(table.erase(key(4), 0x6));
    EXPECT_TRUE(table.erase(key(2), 0x3));
    EXPECT_EQ(table.size(), 0U);
    EXPECT_FALSE(table.erase(key(2), 0x3));
}

TEST(TcpTable, DuplicateAndFullTable) {
    FlowTable table{1};
    ASSERT_EQ(table.capacity(), 2U);
    EXPECT_TRUE(table.insert(key(1), 0, 0x1));
    EXPECT_FALSE(table.insert(key(1), 5, 0x1)) << "a duplicate key is refused";
    EXPECT_EQ(table.find(key(1), 0x1), 0U);
    EXPECT_TRUE(table.insert(key(2), 1, 0x1)) << "a second key with the same hash probes to the next slot";
    EXPECT_FALSE(table.insert(key(3), 2, 0x1)) << "full: two entries in two slots";
    EXPECT_EQ(table.size(), 2U);
    EXPECT_FALSE(table.find(key(3), 0x1));
    EXPECT_TRUE(table.erase(key(1), 0x1));
    EXPECT_EQ(table.find(key(2), 0x1), 1U);
}

TEST(TcpTable, FieldEqualityNotHashEquality) {
    FlowTable table{4};
    ASSERT_TRUE(table.insert(key(40000, 7), 1, 0x1));
    EXPECT_FALSE(table.find(key(40000, 8), 0x1)) << "same hash, different local port";
    EXPECT_FALSE(table.find({.remote = other, .remote_port = 40000, .local_port = 7}, 0x1));
    EXPECT_FALSE(table.find(key(40000, 7), 0x2)) << "the same key under another base hash is a different bucket";
}

TEST(TcpTable, HardwareSoftwareHashAgreement) {
    // Received orientation: the peer is the source, we are the destination; `connect` hashes the same tuple.
    const aloe::device::RssDescription rss = aloe::device::round_robin_rss(4);
    const aloe::device::FlowTuple received{.source           = peer,
                                           .destination      = other,
                                           .source_port      = 40000,
                                           .destination_port = 33000,
                                           .protocol         = aloe::wire::Ipv4Protocol::Tcp};
    const std::uint32_t hardware = aloe::device::flow_hash(rss, received);
    const FlowKey logical{.remote = peer, .remote_port = 40000, .local_port = 33000};
    FlowTable table{16};
    ASSERT_TRUE(table.insert(logical, 3, hardware));
    EXPECT_EQ(table.find(logical, aloe::device::flow_hash(rss, received)), 3U)
        << "a packet without rss_hash recomputes it";
    EXPECT_TRUE(table.erase(logical, hardware));
    ASSERT_TRUE(table.insert(logical, 4, software_flow_hash(logical)));
    EXPECT_EQ(table.find(logical, software_flow_hash(logical)), 4U);
    EXPECT_FALSE(table.find(logical, hardware)) << "one base hash function per stack, chosen at construction";
}

TEST(TcpTable, ShardConditionedHashes) {
    // With 16 queues and a round-robin table every hash reaching one shard shares its low four bits.
    FlowTable table{1024};
    ASSERT_EQ(table.capacity(), 2048U);
    std::set<std::uint32_t> homes;
    std::set<std::uint32_t> low_nibbles;
    for (std::uint32_t flow = 0; flow < 64; ++flow) {
        const std::uint32_t base = (flow << 4U) | 0x5U;
        homes.insert(mix_flow_hash(base) & 2047U);
        low_nibbles.insert(mix_flow_hash(base) & 0xfU);
        ASSERT_TRUE(table.insert(key(static_cast<std::uint16_t>(40000 + flow)), flow, base));
    }
    EXPECT_EQ(homes.size(), 64U) << "mixing spreads the conditioned hashes over distinct buckets";
    EXPECT_GT(low_nibbles.size(), 8U) << "the shared low nibble of the base hashes does not survive the mixer";
    for (std::uint32_t flow = 0; flow < 64; ++flow) {
        const std::uint32_t base = (flow << 4U) | 0x5U;
        EXPECT_EQ(table.find(key(static_cast<std::uint16_t>(40000 + flow)), base), flow);
    }
    for (std::uint32_t flow = 0; flow < 64; flow += 2) {
        EXPECT_TRUE(table.erase(key(static_cast<std::uint16_t>(40000 + flow)), (flow << 4U) | 0x5U));
    }
    for (std::uint32_t flow = 1; flow < 64; flow += 2) {
        EXPECT_EQ(table.find(key(static_cast<std::uint16_t>(40000 + flow)), (flow << 4U) | 0x5U), flow);
    }
    // Four queues: the low two bits are fixed; the same property holds.
    std::set<std::uint32_t> homes4;
    for (std::uint32_t flow = 0; flow < 64; ++flow) {
        homes4.insert(mix_flow_hash((flow << 2U) | 0x1U) & 2047U);
    }
    EXPECT_GE(homes4.size(), 60U);
}
