#include <aloe/device>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace {

    // Microsoft's RSS verification vectors for IPv4, as DPDK's app/test/test_thash.c lists them.
    struct Vector {
        aloe::device::Ipv4Address destination;
        aloe::device::Ipv4Address source;
        std::uint16_t destination_port = 0;
        std::uint16_t source_port      = 0;
        std::uint32_t hash_2_tuple     = 0;
        std::uint32_t hash_4_tuple     = 0;
    };

    constexpr std::array<Vector, 5> vectors = {
        {
         {{161, 142, 100, 80}, {66, 9, 149, 187}, 1766, 2794, 0x323e8fc2, 0x51ccc178},
         {{65, 69, 140, 83}, {199, 92, 111, 2}, 4739, 14230, 0xd718262a, 0xc626b0ea},
         {{12, 22, 207, 184}, {24, 19, 198, 95}, 38024, 12898, 0xd2d0a5de, 0x5c2b394a},
         {{209, 142, 163, 6}, {38, 27, 205, 30}, 2217, 48228, 0x82989176, 0xafc7327f},
         {{202, 188, 127, 2}, {153, 39, 163, 191}, 1303, 44251, 0x5d1809c5, 0x10e828a2},
         }
    };

    aloe::device::RssDescription microsoft_key_rss() {
        auto rss       = aloe::device::round_robin_rss(4, 128, 40);
        rss.types.ipv4 = true;
        return rss;
    }

    aloe::device::FlowTuple tuple(const Vector& vector, const aloe::device::Ipv4Protocol protocol) {
        return {.source           = vector.source,
                .destination      = vector.destination,
                .source_port      = vector.source_port,
                .destination_port = vector.destination_port,
                .protocol         = protocol};
    }

}  // namespace

TEST(Rss, ToeplitzMatchesTheVerificationVectorsWithPorts) {
    const auto rss = microsoft_key_rss();
    for (const Vector& vector : vectors) {
        EXPECT_EQ(aloe::device::flow_hash(rss, tuple(vector, aloe::device::Ipv4Protocol::Tcp)), vector.hash_4_tuple);
        EXPECT_EQ(aloe::device::flow_hash(rss, tuple(vector, aloe::device::Ipv4Protocol::Udp)), vector.hash_4_tuple);
    }
}

TEST(Rss, ToeplitzMatchesTheVerificationVectorsWithoutPorts) {
    const auto rss = microsoft_key_rss();
    for (const Vector& vector : vectors) {
        EXPECT_EQ(aloe::device::flow_hash(rss, tuple(vector, aloe::device::Ipv4Protocol::Icmp)), vector.hash_2_tuple);
    }
}

TEST(Rss, TheFullKeyHashesIpv4TheSameAsTheFortyByteKey) {
    const auto full  = aloe::device::round_robin_rss(4);
    const auto forty = microsoft_key_rss();
    for (const Vector& vector : vectors) {
        EXPECT_EQ(aloe::device::flow_hash(full, tuple(vector, aloe::device::Ipv4Protocol::Tcp)), vector.hash_4_tuple);
        EXPECT_EQ(aloe::device::flow_hash(forty, tuple(vector, aloe::device::Ipv4Protocol::Tcp)), vector.hash_4_tuple);
    }
}

TEST(Rss, FallsBackToTheTwoTupleWhenThePortTypeIsOff) {
    auto rss           = microsoft_key_rss();
    rss.types.ipv4_udp = false;
    EXPECT_EQ(aloe::device::flow_hash(rss, tuple(vectors[0], aloe::device::Ipv4Protocol::Udp)),
              vectors[0].hash_2_tuple);
    EXPECT_EQ(aloe::device::flow_hash(rss, tuple(vectors[0], aloe::device::Ipv4Protocol::Tcp)),
              vectors[0].hash_4_tuple);
}

TEST(Rss, HashesZeroWhenNoTypeApplies) {
    auto rss  = microsoft_key_rss();
    rss.types = {};
    EXPECT_EQ(aloe::device::flow_hash(rss, tuple(vectors[0], aloe::device::Ipv4Protocol::Tcp)), 0U);
    aloe::device::RssDescription disabled;
    EXPECT_EQ(aloe::device::flow_hash(disabled, tuple(vectors[0], aloe::device::Ipv4Protocol::Tcp)), 0U);
}

TEST(Rss, QueueForIndexesTheTableWithTheHash) {
    const auto rss = microsoft_key_rss();
    for (const Vector& vector : vectors) {
        const std::uint16_t expected = rss.table[vector.hash_4_tuple & (rss.table.size() - 1)];
        EXPECT_EQ(aloe::device::queue_for(rss, tuple(vector, aloe::device::Ipv4Protocol::Tcp)), expected);
        EXPECT_EQ(expected, vector.hash_4_tuple % 4) << "round robin table";
    }
}

TEST(Rss, QueueForIsZeroWhenSteeringIsOff) {
    aloe::device::RssDescription disabled;
    EXPECT_EQ(aloe::device::queue_for(disabled, tuple(vectors[0], aloe::device::Ipv4Protocol::Tcp)), 0);
    auto no_table = microsoft_key_rss();
    no_table.table.clear();
    EXPECT_EQ(aloe::device::queue_for(no_table, tuple(vectors[0], aloe::device::Ipv4Protocol::Tcp)), 0);
}

TEST(Rss, RoundRobinDescriptionSpreadsQueuesOverTheTable) {
    const auto rss = aloe::device::round_robin_rss(3, 8);
    EXPECT_TRUE(rss.enabled);
    EXPECT_EQ(rss.key_length, 52);
    EXPECT_EQ(rss.key, aloe::device::aloe_rss_key);
    EXPECT_EQ(rss.types, (aloe::device::RssHashTypes{.ipv4 = true, .ipv4_tcp = true, .ipv4_udp = true}));
    EXPECT_EQ(rss.table, (std::vector<std::uint16_t>{0, 1, 2, 0, 1, 2, 0, 1}));
}

TEST(Rss, RoundRobinDescriptionRejectsImpossibleArguments) {
    EXPECT_THROW(std::ignore = aloe::device::round_robin_rss(0), std::invalid_argument);
    EXPECT_THROW(std::ignore = aloe::device::round_robin_rss(4, 128, 3), std::invalid_argument);
    EXPECT_THROW(std::ignore = aloe::device::round_robin_rss(4, 128, 53), std::invalid_argument);
}

TEST(Rss, AnOverlongKeyLengthIsClampedToTheKey) {
    auto rss       = aloe::device::round_robin_rss(4);
    const auto key = aloe::device::flow_hash(rss,
                                             {
                                                 .source = {10, 0, 0, 1},
                                                   .destination = {10, 0, 0, 2}
    });
    rss.key_length = 255;
    EXPECT_EQ(aloe::device::flow_hash(rss,
                                      {
                                          .source = {10, 0, 0, 1},
                                            .destination = {10, 0, 0, 2}
    }),
              key);
}
