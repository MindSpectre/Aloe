#include <aloe/ethdev>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <eal_environment.hpp>
#include <gtest/gtest.h>
#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

namespace {

    /// A pool of the test's own, so the packet is tested without a port.
    class EthdevPacket : public testing::Test {
    protected:
        rte_mempool* pool_ = rte_pktmbuf_pool_create(
            "packet-test", 63, 0, 0, RTE_MBUF_DEFAULT_BUF_SIZE, static_cast<int>(rte_socket_id()));

        ~EthdevPacket() override {
            rte_mempool_free(pool_);
        }

        void SetUp() override {
            ASSERT_NE(pool_, nullptr) << rte_strerror(rte_errno);
        }

        aloe::ethdev::Packet allocate() {
            rte_mbuf* mbuf = rte_pktmbuf_alloc(pool_);
            EXPECT_NE(mbuf, nullptr);
            return aloe::ethdev::Packet{mbuf};
        }
    };

}  // namespace

TEST_F(EthdevPacket, WrapsOneMbufAndFreesItToItsPool) {
    static_assert(aloe::IsPacket<aloe::ethdev::Packet>);
    const unsigned before       = rte_mempool_avail_count(pool_);
    aloe::ethdev::Packet packet = allocate();
    ASSERT_FALSE(packet.empty());
    EXPECT_EQ(rte_mempool_avail_count(pool_), before - 1);
    EXPECT_EQ(packet.headroom(), RTE_PKTMBUF_HEADROOM);
    EXPECT_EQ(packet.headroom(), aloe::packet_headroom);
    EXPECT_EQ(packet.tailroom(), RTE_MBUF_DEFAULT_DATAROOM);
    EXPECT_EQ(packet.get()->nb_segs, 1);

    aloe::ethdev::Packet moved{std::move(packet)};
    EXPECT_TRUE(packet.empty());  // NOLINT(bugprone-use-after-move): the moved-from state is the point
    EXPECT_EQ(rte_mempool_avail_count(pool_), before - 1) << "moving frees nothing";
    moved = aloe::ethdev::Packet{};
    EXPECT_EQ(rte_mempool_avail_count(pool_), before) << "assigning over a packet frees its mbuf";

    rte_mbuf* raw = allocate().release();
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(rte_mempool_avail_count(pool_), before - 1) << "a released mbuf is the caller's";
    {
        const aloe::ethdev::Packet adopted{raw};
        EXPECT_EQ(adopted.get(), raw);
    }
    EXPECT_EQ(rte_mempool_avail_count(pool_), before) << "and freed again once adopted and destroyed";
}

TEST_F(EthdevPacket, GrowsAndShrinksThroughTheMbuf) {
    aloe::ethdev::Packet packet = allocate();
    ASSERT_TRUE(packet.append(100).has_value());
    ASSERT_TRUE(packet.prepend(14).has_value());
    EXPECT_EQ(packet.size(), 114);
    EXPECT_EQ(packet.get()->data_len, 114);
    EXPECT_EQ(packet.get()->pkt_len, 114);
    EXPECT_EQ(packet.data().data(), rte_pktmbuf_mtod(packet.get(), std::byte*));
    EXPECT_FALSE(packet.prepend(200).has_value());
    EXPECT_FALSE(packet.append(4000).has_value());
    packet.trim_front(14);
    packet.trim_back(100);
    EXPECT_EQ(packet.size(), 0);
    EXPECT_EQ(packet.headroom(), aloe::packet_headroom);
}

TEST_F(EthdevPacket, ReadsReceiveMetadataFromTheOffloadFlags) {
    aloe::ethdev::Packet packet = allocate();
    EXPECT_EQ(packet.rx(), aloe::RxMetadata{});

    packet.get()->ol_flags    = RTE_MBUF_F_RX_RSS_HASH | RTE_MBUF_F_RX_IP_CKSUM_GOOD | RTE_MBUF_F_RX_L4_CKSUM_BAD;
    packet.get()->hash.rss    = 0x1234'5678;
    const aloe::RxMetadata rx = packet.rx();
    EXPECT_EQ(rx.rss_hash, 0x1234'5678U);
    EXPECT_EQ(rx.l3, aloe::ChecksumVerdict::Good);
    EXPECT_EQ(rx.l4, aloe::ChecksumVerdict::Bad);

    packet.get()->ol_flags = RTE_MBUF_F_RX_IP_CKSUM_NONE | RTE_MBUF_F_RX_L4_CKSUM_NONE;
    EXPECT_EQ(packet.rx().l3, aloe::ChecksumVerdict::Unknown) << "both bits set means not checked";
    EXPECT_EQ(packet.rx().l4, aloe::ChecksumVerdict::Unknown);
    EXPECT_FALSE(packet.rx().rss_hash.has_value());
}

TEST_F(EthdevPacket, WritesTransmitMetadataToTheOffloadFlags) {
    aloe::ethdev::Packet packet = allocate();
    const aloe::TxMetadata tcp{
        .l2_length = 14, .l3_length = 20, .fill_ipv4_checksum = true, .fill_l4_checksum = aloe::L4Checksum::Tcp};
    packet.set_tx(tcp);
    EXPECT_EQ(packet.get()->l2_len, 14);
    EXPECT_EQ(packet.get()->l3_len, 20);
    EXPECT_EQ(packet.get()->ol_flags, RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_TCP_CKSUM);
    EXPECT_EQ(packet.tx(), tcp);

    packet.set_tx(aloe::TxMetadata{.l2_length = 14, .l3_length = 20, .fill_l4_checksum = aloe::L4Checksum::Udp});
    EXPECT_EQ(packet.get()->ol_flags, RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_UDP_CKSUM);
    EXPECT_EQ(packet.tx().fill_l4_checksum, aloe::L4Checksum::Udp);
    EXPECT_FALSE(packet.tx().fill_ipv4_checksum);

    packet.set_tx(aloe::TxMetadata{});
    EXPECT_EQ(packet.get()->ol_flags, 0);
    EXPECT_EQ(packet.tx(), aloe::TxMetadata{});
}

TEST_F(EthdevPacket, EmptyPacketsHaveNoRoom) {
    aloe::ethdev::Packet packet;
    EXPECT_TRUE(packet.empty());
    EXPECT_EQ(packet.size(), 0);
    EXPECT_EQ(packet.headroom(), 0);
    EXPECT_EQ(packet.tailroom(), 0);
    EXPECT_FALSE(packet.prepend(1).has_value());
    EXPECT_FALSE(packet.append(1).has_value());
    EXPECT_EQ(packet.get(), nullptr);
    packet.set_tx(aloe::TxMetadata{.l2_length = 14});
    EXPECT_EQ(packet.tx(), aloe::TxMetadata{}) << "nothing to write to";
}
