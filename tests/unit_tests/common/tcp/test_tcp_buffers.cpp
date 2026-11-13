#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/tcp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using Packet = aloe::fabric::Packet;
    using Pool   = aloe::tcp::detail::HeldSegmentPool<Packet>;
    using Chain  = aloe::tcp::detail::NodeChain;
    using View   = aloe::tcp::ReadView<Packet>;

    static_assert(std::ranges::forward_range<View>);
    static_assert(!std::ranges::sized_range<View>, "size() counts bytes, not chunks");

    class TcpBuffers : public ::testing::Test {
    protected:
        aloe::fabric::Fabric fabric_;
        aloe::fabric::Port& port_ = fabric_.add_port({
            .mac = {0x02, 0, 0, 0, 0, 1},
              .pool_size = 8
        });

        /// A packet holding `offset` bytes of header and `length` payload bytes; byte `i` is `offset + i`.
        [[nodiscard]] Packet packet(const std::uint8_t offset, const std::size_t length) {
            auto out = port_.allocate(0);
            EXPECT_TRUE(out.has_value());
            EXPECT_TRUE(aloe::frames::fill(*out, aloe::frames::pattern(offset + length, offset)));
            return std::move(*out);
        }

        /// Holds a packet as the stack does: a node first, then the packet moved in and the node chained.
        [[nodiscard]] static std::uint32_t
        hold(Pool& pool, Chain& chain, Packet&& held, const std::uint16_t offset, const std::uint16_t length) {
            const std::uint32_t index = pool.acquire().value();  // throws, and fails the test, on a full pool
            auto& node                = pool.node(index);
            node.packet               = std::move(held);
            node.offset               = offset;
            node.length               = length;
            pool.push_back(chain, index);
            return index;
        }
    };

    /// A node with nothing but its link: the pool's own behaviour, apart from what a node holds.
    struct Bare {
        std::uint32_t next = aloe::tcp::detail::no_node;
    };

}  // namespace

TEST_F(TcpBuffers, RetainsPacketsAndIteratesSpans) {
    Pool pool{4};
    Chain chain;
    EXPECT_EQ(pool.capacity(), 4U);
    EXPECT_EQ(pool.available(), 4U);
    const std::uint32_t a = hold(pool, chain, packet(54, 3), 54, 3);
    const std::uint32_t b = hold(pool, chain, packet(54, 5), 54, 5);
    const std::uint32_t c = hold(pool, chain, packet(60, 2), 60, 2);
    EXPECT_EQ(pool.available(), 1U);
    EXPECT_EQ(chain.head, a);
    EXPECT_EQ(chain.tail, c);
    EXPECT_EQ(chain.count, 3U);
    const View view{pool, chain.head, 10};
    EXPECT_EQ(view.size(), 10U);
    EXPECT_FALSE(view.empty());
    EXPECT_EQ(view.front().size(), 3U);
    std::vector<std::size_t> sizes;
    std::vector<const std::byte*> starts;
    for (const std::span<const std::byte> chunk : view) {
        sizes.push_back(chunk.size());
        starts.push_back(chunk.data());
    }
    EXPECT_EQ(sizes, (std::vector<std::size_t>{3, 5, 2}));
    EXPECT_EQ(starts[0], pool.node(a).packet.data().data() + 54) << "a span into the packet, not a copy";
    EXPECT_EQ(starts[1], pool.node(b).packet.data().data() + 54);
    EXPECT_EQ(starts[2], pool.node(c).packet.data().data() + 60);
    // The pattern helper seeds byte i with offset + i, so the byte at index 54 of a seed-54 packet is 108.
    EXPECT_EQ(std::to_integer<int>(view.front()[0]), 108) << "the first payload byte, not the first packet byte";
    EXPECT_EQ(std::ranges::distance(view), 3) << "iteration counts chunks; size() counts bytes";
    auto it = view.begin();
    ++it;
    EXPECT_EQ((*it).size(), 5U);
    EXPECT_EQ((*it++).size(), 5U);
    EXPECT_EQ((*it).size(), 2U);
    ++it;
    EXPECT_EQ(it, view.end());
}

TEST_F(TcpBuffers, PartialFrontOffsetAndEmptyView) {
    Pool pool{2};
    Chain chain;
    const std::uint32_t a  = hold(pool, chain, packet(54, 6), 54, 6);
    pool.node(a).offset   += 4;  // four bytes consumed
    pool.node(a).length   -= 4;
    const View view{pool, a, 2};
    EXPECT_EQ(view.size(), 2U);
    EXPECT_EQ(view.front().size(), 2U);
    // Index 58 of a seed-54 packet holds 54 + 58 = 112.
    EXPECT_EQ(std::to_integer<int>(view.front()[0]), 112);
    const View empty;
    EXPECT_TRUE(empty.empty());
    EXPECT_EQ(empty.size(), 0U);
    EXPECT_EQ(empty.begin(), empty.end());
    const View none{pool, aloe::tcp::detail::no_node, 0};
    EXPECT_TRUE(none.empty());
}

TEST_F(TcpBuffers, ExhaustionRefusesAndReleaseReturnsThePacket) {
    Pool pool{1};
    Chain chain;
    const std::uint32_t a = hold(pool, chain, packet(54, 1), 54, 1);
    EXPECT_EQ(pool.available(), 0U);
    EXPECT_FALSE(pool.acquire()) << "every node is taken";
    pool.release_front(chain);
    EXPECT_TRUE(chain.empty());
    EXPECT_EQ(chain.tail, aloe::tcp::detail::no_node);
    EXPECT_EQ(chain.count, 0U);
    EXPECT_EQ(pool.available(), 1U);
    EXPECT_TRUE(pool.node(a).packet.empty()) << "release returns the packet to its pool";
    EXPECT_TRUE(pool.acquire());
}

TEST(NodePool, ReleaseBackCutsTheLinkToTheOldTail) {
    aloe::tcp::detail::NodePool<Bare> pool{3};
    Chain chain;
    const auto a = pool.acquire();
    const auto b = pool.acquire();
    ASSERT_TRUE(a && b);
    pool.push_back(chain, *a);
    pool.push_back(chain, *b);
    pool.release_back(chain, *a);
    EXPECT_EQ(chain.head, *a);
    EXPECT_EQ(chain.tail, *a);
    EXPECT_EQ(chain.count, 1U);
    EXPECT_EQ(pool.node(*a).next, aloe::tcp::detail::no_node) << "the new tail links nowhere";
    pool.release_back(chain, aloe::tcp::detail::no_node);
    EXPECT_TRUE(chain.empty());
    EXPECT_EQ(chain.tail, aloe::tcp::detail::no_node);
    EXPECT_EQ(pool.available(), 3U);
}

TEST(NodePool, AReservedNodeIsLeftForAcquireReserved) {
    aloe::tcp::detail::NodePool<Bare> pool{2};
    ASSERT_TRUE(pool.reserve());
    EXPECT_EQ(pool.available(), 1U);
    EXPECT_TRUE(pool.acquire());
    EXPECT_FALSE(pool.acquire()) << "the last free node is set aside";
    EXPECT_FALSE(pool.reserve()) << "none is left to set aside";
    EXPECT_TRUE(pool.acquire_reserved());
    EXPECT_EQ(pool.reserved(), 0U);
    EXPECT_FALSE(pool.acquire_reserved()) << "nothing set aside any more";
}

TEST(NodePool, RejectsZeroAndUnrepresentableCountsBeforeAllocating) {
    EXPECT_THROW((std::ignore = Pool{0}), std::invalid_argument);
    EXPECT_THROW((std::ignore = Pool{static_cast<std::size_t>(aloe::tcp::detail::no_node)}), std::invalid_argument);
}

TEST(NodePool, InsertAfterAndUnlinkAfterKeepTheChainInStep) {
    using aloe::tcp::detail::no_node;
    aloe::tcp::detail::NodePool<Bare> pool{4};
    Chain chain;
    const std::uint32_t a = pool.acquire().value();
    const std::uint32_t b = pool.acquire().value();
    const std::uint32_t c = pool.acquire().value();
    const std::uint32_t d = pool.acquire().value();

    pool.insert_after(chain, no_node, b);  // into an empty chain: head and tail
    EXPECT_EQ(chain.head, b);
    EXPECT_EQ(chain.tail, b);
    EXPECT_EQ(chain.count, 1U);
    pool.insert_after(chain, no_node, a);  // at the head: a, b
    EXPECT_EQ(chain.head, a);
    EXPECT_EQ(chain.tail, b);
    EXPECT_EQ(chain.count, 2U);
    EXPECT_EQ(pool.node(a).next, b);
    pool.insert_after(chain, b, d);  // at the tail: a, b, d
    EXPECT_EQ(chain.head, a);
    EXPECT_EQ(chain.tail, d);
    EXPECT_EQ(chain.count, 3U);
    EXPECT_EQ(pool.node(b).next, d);
    EXPECT_EQ(pool.node(d).next, no_node);
    pool.insert_after(chain, b, c);  // in the middle: a, b, c, d
    EXPECT_EQ(chain.head, a);
    EXPECT_EQ(chain.tail, d);
    EXPECT_EQ(chain.count, 4U);
    EXPECT_EQ(pool.node(b).next, c);
    EXPECT_EQ(pool.node(c).next, d);

    EXPECT_EQ(pool.unlink_after(chain, b), c);  // the middle: a, b, d
    EXPECT_EQ(chain.head, a);
    EXPECT_EQ(chain.tail, d);
    EXPECT_EQ(chain.count, 3U);
    EXPECT_EQ(pool.node(b).next, d);
    EXPECT_EQ(pool.node(c).next, no_node) << "an unlinked node links nowhere";
    EXPECT_EQ(pool.unlink_after(chain, b), d);  // the tail: a, b
    EXPECT_EQ(chain.head, a);
    EXPECT_EQ(chain.tail, b);
    EXPECT_EQ(chain.count, 2U);
    EXPECT_EQ(pool.node(b).next, no_node) << "the new tail links nowhere";
    EXPECT_EQ(pool.unlink_after(chain, no_node), a);  // the head: b
    EXPECT_EQ(chain.head, b);
    EXPECT_EQ(chain.tail, b);
    EXPECT_EQ(chain.count, 1U);
    EXPECT_EQ(pool.node(a).next, no_node);
    EXPECT_EQ(pool.unlink_after(chain, no_node), b);  // the only node
    EXPECT_TRUE(chain.empty());
    EXPECT_EQ(chain.tail, no_node);
    EXPECT_EQ(chain.count, 0U);
    EXPECT_EQ(pool.available(), 0U) << "unlinked nodes stay taken";
}

TEST(NodePool, SpliceBackLinksARunAndEmptiesIt) {
    using aloe::tcp::detail::no_node;
    aloe::tcp::detail::NodePool<Bare> pool{4};
    const std::uint32_t a = pool.acquire().value();
    const std::uint32_t b = pool.acquire().value();
    const std::uint32_t c = pool.acquire().value();
    const std::uint32_t d = pool.acquire().value();

    Chain chain;
    Chain run;
    pool.push_back(run, a);
    pool.push_back(run, b);
    pool.splice_back(chain, run);  // onto an empty chain
    EXPECT_EQ(chain.head, a);
    EXPECT_EQ(chain.tail, b);
    EXPECT_EQ(chain.count, 2U);
    EXPECT_TRUE(run.empty());
    EXPECT_EQ(run.tail, no_node);
    EXPECT_EQ(run.count, 0U);

    pool.push_back(run, c);
    pool.push_back(run, d);
    pool.splice_back(chain, run);  // onto a chain with nodes
    EXPECT_EQ(chain.head, a);
    EXPECT_EQ(chain.tail, d);
    EXPECT_EQ(chain.count, 4U);
    EXPECT_EQ(pool.node(b).next, c);
    EXPECT_EQ(pool.node(d).next, no_node);
    EXPECT_TRUE(run.empty());
    EXPECT_EQ(run.count, 0U);

    pool.splice_back(chain, run);  // an empty run changes nothing
    EXPECT_EQ(chain.tail, d);
    EXPECT_EQ(chain.count, 4U);
}
