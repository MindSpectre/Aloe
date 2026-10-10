#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/stream>
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
    using Pool   = aloe::tcp::detail::TcpNodePool<Packet>;
    using View   = aloe::tcp::ReadView<Packet>;

    static_assert(aloe::stream::IsReadView<View>);
    static_assert(std::ranges::forward_range<View>);

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
    };

}  // namespace

TEST_F(TcpBuffers, RetainsPacketsAndIteratesSpans) {
    Pool pool{4};
    EXPECT_EQ(pool.capacity(), 4U);
    EXPECT_EQ(pool.available(), 4U);
    const auto a = pool.acquire(packet(54, 3), 54, 3);
    const auto b = pool.acquire(packet(54, 5), 54, 5);
    const auto c = pool.acquire(packet(60, 2), 60, 2);
    ASSERT_TRUE(a && b && c);
    EXPECT_EQ(pool.available(), 1U);
    pool.node(*a).next = *b;
    pool.node(*b).next = *c;
    const View view{pool, *a, 10};
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
    EXPECT_EQ(starts[0], pool.node(*a).packet.data().data() + 54) << "a span into the packet, not a copy";
    EXPECT_EQ(starts[1], pool.node(*b).packet.data().data() + 54);
    EXPECT_EQ(starts[2], pool.node(*c).packet.data().data() + 60);
    // The pattern helper seeds byte i with offset + i, so the byte at index 54 of a seed-54 packet is 108.
    EXPECT_EQ(std::to_integer<int>(view.front()[0]), 108) << "the first payload byte, not the first packet byte";
    EXPECT_EQ(std::ranges::distance(view.begin(), view.end()), 3)
        << "chunks; distance(view) would use size(), which counts bytes";
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
    const auto a = pool.acquire(packet(54, 6), 54, 6);
    ASSERT_TRUE(a);
    pool.node(*a).offset += 4;  // four bytes consumed
    pool.node(*a).length -= 4;
    const View view{pool, *a, 2};
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

TEST_F(TcpBuffers, ExhaustionKeepsThePacketAndReleaseRestoresTheCount) {
    Pool pool{1};
    const auto a = pool.acquire(packet(54, 1), 54, 1);
    ASSERT_TRUE(a);
    EXPECT_EQ(pool.available(), 0U);
    Packet kept    = packet(54, 1);
    const auto* at = kept.data().data();
    const auto b   = pool.acquire(std::move(kept), 54, 1);
    EXPECT_FALSE(b);
    // A refused acquire leaves the packet with the caller, so the uses below are right.
    // NOLINTBEGIN(bugprone-use-after-move)
    EXPECT_FALSE(kept.empty()) << "a failed acquire leaves the packet with the caller";
    EXPECT_EQ(kept.data().data(), at);
    // NOLINTEND(bugprone-use-after-move)
    pool.release(*a);
    EXPECT_EQ(pool.available(), 1U);
    EXPECT_TRUE(pool.node(*a).packet.empty()) << "release returns the packet to its pool";
    EXPECT_EQ(port_.counters(0).received, 0U);
    const auto c = pool.acquire(std::move(kept), 54, 1);
    EXPECT_TRUE(c);
    EXPECT_EQ(pool.available(), 0U);
}

TEST_F(TcpBuffers, PoolRejectsZeroAndUnrepresentableCounts) {
    EXPECT_THROW((std::ignore = Pool{0}), std::invalid_argument);
    EXPECT_THROW((std::ignore = Pool{static_cast<std::size_t>(aloe::tcp::detail::no_node)}), std::invalid_argument);
}
