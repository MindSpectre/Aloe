#include <algorithm>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using Packet   = aloe::fabric::Packet;
    using Pool     = aloe::tcp::detail::HeldSegmentPool<Packet>;
    using Queue    = aloe::tcp::detail::ReassemblyQueue<Packet>;
    using Placed   = Queue::Placed;
    using Sequence = aloe::wire::TcpSequence;
    using Block    = aloe::wire::TcpSackBlock;

    constexpr std::uint16_t header_size = 54;

    [[nodiscard]] Block block(const std::uint32_t first, const std::uint32_t end) {
        return Block{.first = Sequence{first}, .end = Sequence{end}};
    }

    class TcpReassembly : public ::testing::Test {
    protected:
        aloe::fabric::Fabric fabric_;
        aloe::fabric::Port& port_ = fabric_.add_port({
            .mac = {0x02, 0, 0, 0, 0, 1},
              .pool_size = 64
        });
        Pool pool{32};

        /// A packet with `header_size` bytes of header, then `length` payload bytes of `frames::pattern(length, seed)`.
        [[nodiscard]] Packet segment(const std::size_t length, const std::uint8_t seed) {
            return framed(aloe::frames::pattern(length, seed));
        }

        /// A packet with `header_size` bytes of header, then `payload`.
        [[nodiscard]] Packet framed(const std::span<const std::byte> payload) {
            auto out = port_.allocate(0);
            EXPECT_TRUE(out.has_value());
            std::vector<std::byte> frame(header_size);
            frame.insert(frame.end(), payload.begin(), payload.end());
            EXPECT_TRUE(aloe::frames::fill(*out, frame));
            return std::move(*out);
        }

        /// Inserts `length` bytes of `frames::pattern(length, seed)` starting at sequence `first`.
        Queue::Inserted put(Queue& queue,
                            Pool& on,
                            const std::uint32_t first,
                            const std::uint16_t length,
                            const std::uint8_t seed = 0) {
            Packet packet = segment(length, seed);
            return queue.insert(on, packet, header_size, length, Sequence{first});
        }

        Queue::Inserted
        put(Queue& queue, const std::uint32_t first, const std::uint16_t length, const std::uint8_t seed = 0) {
            return put(queue, pool, first, length, seed);
        }

        /// The bytes of the run's nodes, in order.
        [[nodiscard]] static std::vector<std::byte> read(const Pool& on, const Queue::Run& run) {
            std::vector<std::byte> bytes;
            for (std::uint32_t index = run.chain.head; index != aloe::tcp::detail::no_node;
                 index               = on.node(index).next) {
                const auto& node   = on.node(index);
                const auto payload = node.packet.data().subspan(node.offset, node.length);
                bytes.insert(bytes.end(), payload.begin(), payload.end());
            }
            return bytes;
        }

        [[nodiscard]] std::vector<std::byte> read(const Queue::Run& run) const {
            return read(pool, run);
        }
    };

    /// `count` bytes of `frames::pattern(length, seed)` from `from` on.
    [[nodiscard]] std::vector<std::byte>
    slice(const std::size_t length, const std::uint8_t seed, const std::size_t from, const std::size_t count) {
        const std::vector<std::byte> whole = aloe::frames::pattern(length, seed);
        return {whole.begin() + static_cast<std::ptrdiff_t>(from),
                whole.begin() + static_cast<std::ptrdiff_t>(from + count)};
    }

    [[nodiscard]] std::vector<std::byte> joined(const std::vector<std::vector<std::byte>>& parts) {
        std::vector<std::byte> out;
        for (const auto& part : parts) {
            out.insert(out.end(), part.begin(), part.end());
        }
        return out;
    }

}  // namespace

TEST_F(TcpReassembly, ARunAboveTheGapIsHeldInOrder) {
    Queue queue;
    EXPECT_EQ(put(queue, 2000, 500).placed, Placed::Held);
    EXPECT_EQ(put(queue, 2500, 500).placed, Placed::Held);
    const Queue::Inserted last = put(queue, 3000, 500);
    EXPECT_EQ(last.placed, Placed::Held);
    EXPECT_EQ(last.kept, block(3000, 3500));
    EXPECT_FALSE(last.duplicate);
    EXPECT_EQ(queue.segments(), 3U);
    EXPECT_EQ(queue.bytes(), 1500U);
    EXPECT_EQ(queue.highest(), block(3000, 3500));
    EXPECT_TRUE(queue.holds());
    queue.release(pool);
}

TEST_F(TcpReassembly, ArrivalInAnyOrderReadsTheSame) {
    std::array<std::uint8_t, 4> order{0, 1, 2, 3};
    const std::vector<std::byte> expected =
        joined({slice(100, 0, 0, 100), slice(100, 1, 0, 100), slice(100, 2, 0, 100), slice(100, 3, 0, 100)});
    do {
        Queue queue;
        for (const std::uint8_t at : order) {
            EXPECT_EQ(put(queue, 1000 + (at * 100U), 100, at).placed, Placed::Held);
        }
        Queue::Run run = queue.take_run(pool, Sequence{1000});
        EXPECT_EQ(run.chain.count, 4U);
        EXPECT_EQ(run.bytes, 400U);
        EXPECT_FALSE(run.duplicate);
        EXPECT_EQ(read(run), expected);
        EXPECT_FALSE(queue.holds());
        EXPECT_EQ(queue.bytes(), 0U);
        pool.release_all(run.chain);
    } while (std::ranges::next_permutation(order).found);
    EXPECT_EQ(pool.available(), pool.capacity());
}

TEST_F(TcpReassembly, ADuplicateOfHeldDataIsRefusedAndReported) {
    Queue queue;
    ASSERT_EQ(put(queue, 2000, 500).placed, Placed::Held);
    Packet packet                 = segment(200, 7);
    const Queue::Inserted refused = queue.insert(pool, packet, header_size, 200, Sequence{2100});
    EXPECT_EQ(refused.placed, Placed::Duplicate);
    EXPECT_EQ(refused.duplicate, block(2100, 2300));
    EXPECT_FALSE(packet.empty()) << "a refused packet stays with the caller";
    EXPECT_EQ(queue.segments(), 1U);
    EXPECT_EQ(queue.bytes(), 500U);
    queue.release(pool);
}

TEST_F(TcpReassembly, ANewcomerCoveredByTwoNeighboursIsADuplicate) {
    Queue queue;
    ASSERT_EQ(put(queue, 2000, 100).placed, Placed::Held);
    ASSERT_EQ(put(queue, 2100, 100).placed, Placed::Held);
    Packet packet                 = segment(100, 7);
    const Queue::Inserted refused = queue.insert(pool, packet, header_size, 100, Sequence{2050});
    EXPECT_EQ(refused.placed, Placed::Duplicate);
    EXPECT_EQ(refused.duplicate, block(2050, 2100));
    EXPECT_FALSE(packet.empty());
    EXPECT_EQ(queue.segments(), 2U);
    EXPECT_EQ(queue.bytes(), 200U);
    queue.release(pool);
}

TEST_F(TcpReassembly, ACoveringNewcomerReplacesWhatItCovers) {
    Queue queue;
    ASSERT_EQ(put(queue, 2100, 100).placed, Placed::Held);
    ASSERT_EQ(put(queue, 2300, 100).placed, Placed::Held);
    const std::size_t before    = pool.available();
    const Queue::Inserted taken = put(queue, 2000, 500);
    EXPECT_EQ(taken.placed, Placed::Held);
    EXPECT_EQ(taken.kept, block(2000, 2500));
    EXPECT_EQ(taken.duplicate, block(2100, 2200));
    EXPECT_EQ(queue.segments(), 1U);
    EXPECT_EQ(queue.bytes(), 500U);
    EXPECT_EQ(pool.available(), before + 1) << "two nodes released, one taken";
    queue.release(pool);
}

TEST_F(TcpReassembly, ACoveringNewcomerAtTheSameStartReplacesTheNode) {
    Queue queue;
    ASSERT_EQ(put(queue, 2000, 100).placed, Placed::Held);
    const Queue::Inserted taken = put(queue, 2000, 200);
    EXPECT_EQ(taken.placed, Placed::Held);
    EXPECT_EQ(taken.kept, block(2000, 2200));
    EXPECT_EQ(taken.duplicate, block(2000, 2100));
    EXPECT_EQ(queue.segments(), 1U);
    EXPECT_EQ(queue.highest(), block(2000, 2200));
    queue.release(pool);
}

TEST_F(TcpReassembly, HeldBytesWinAPartialOverlapOnBothSides) {
    Queue queue;
    ASSERT_EQ(put(queue, 2000, 100, 1).placed, Placed::Held);
    ASSERT_EQ(put(queue, 2300, 100, 2).placed, Placed::Held);
    const Queue::Inserted taken = put(queue, 2050, 300, 3);
    EXPECT_EQ(taken.placed, Placed::Held);
    EXPECT_EQ(taken.kept, block(2100, 2300));
    EXPECT_EQ(taken.duplicate, block(2050, 2100));
    EXPECT_EQ(queue.segments(), 3U);
    EXPECT_EQ(queue.bytes(), 400U);
    Queue::Run run = queue.take_run(pool, Sequence{2000});
    EXPECT_EQ(run.bytes, 400U);
    EXPECT_EQ(read(run), joined({slice(100, 1, 0, 100), slice(300, 3, 50, 200), slice(100, 2, 0, 100)}));
    EXPECT_FALSE(queue.holds());
    pool.release_all(run.chain);
}

TEST_F(TcpReassembly, TheJoinTakesTheRunAndStopsAtTheNextGap) {
    Queue queue;
    ASSERT_EQ(put(queue, 1100, 100).placed, Placed::Held);
    ASSERT_EQ(put(queue, 1200, 100).placed, Placed::Held);
    ASSERT_EQ(put(queue, 1400, 100).placed, Placed::Held);
    Queue::Run run = queue.take_run(pool, Sequence{1100});
    EXPECT_EQ(run.chain.count, 2U);
    EXPECT_EQ(run.bytes, 200U);
    EXPECT_FALSE(run.duplicate);
    EXPECT_EQ(queue.segments(), 1U);
    EXPECT_EQ(queue.bytes(), 100U);
    EXPECT_EQ(queue.highest(), block(1400, 1500));
    pool.release_all(run.chain);
    queue.release(pool);
}

TEST_F(TcpReassembly, TheJoinTrimsANodeRcvNxtPassedInPart) {
    Queue queue;
    ASSERT_EQ(put(queue, 1100, 200).placed, Placed::Held);
    Queue::Run run = queue.take_run(pool, Sequence{1150});
    EXPECT_EQ(run.bytes, 150U);
    EXPECT_EQ(run.duplicate, block(1100, 1150));
    EXPECT_EQ(read(run), slice(200, 0, 50, 150));
    EXPECT_FALSE(queue.holds());
    pool.release_all(run.chain);
}

TEST_F(TcpReassembly, TheJoinReleasesANodeRcvNxtPassedEntirely) {
    Queue queue;
    ASSERT_EQ(put(queue, 1100, 50).placed, Placed::Held);
    ASSERT_EQ(put(queue, 1200, 100).placed, Placed::Held);
    const std::size_t before = pool.available();
    Queue::Run run           = queue.take_run(pool, Sequence{1200});
    EXPECT_EQ(run.duplicate, block(1100, 1150));
    EXPECT_EQ(run.bytes, 100U);
    EXPECT_EQ(run.chain.count, 1U);
    EXPECT_EQ(pool.available(), before + 1) << "the passed node is back in the pool";
    pool.release_all(run.chain);
}

TEST_F(TcpReassembly, TheJoinStopsAtARememberedFin) {
    Queue queue;
    ASSERT_EQ(put(queue, 1100, 100).placed, Placed::Held);
    ASSERT_EQ(put(queue, 1200, 100).placed, Placed::Held);  // at the FIN: past the peer's end
    queue.remember_fin(Sequence{1200});
    Queue::Run run = queue.take_run(pool, Sequence{1100});
    EXPECT_EQ(run.chain.count, 1U);
    EXPECT_EQ(run.bytes, 100U) << "the run ends at the FIN";
    EXPECT_EQ(queue.segments(), 1U) << "the node at the FIN stays held";
    EXPECT_EQ(queue.bytes(), 100U);
    EXPECT_EQ(queue.fin(), Sequence{1200});
    pool.release_all(run.chain);
    queue.release(pool);
}

TEST_F(TcpReassembly, TheJoinCutsANodeThatStraddlesARememberedFin) {
    Queue queue;
    ASSERT_EQ(put(queue, 1100, 100, 3).placed, Placed::Held);
    queue.remember_fin(Sequence{1150});
    Queue::Run run = queue.take_run(pool, Sequence{1100});
    EXPECT_EQ(run.chain.count, 1U);
    EXPECT_EQ(run.bytes, 50U);
    EXPECT_EQ(read(run), slice(100, 3, 0, 50)) << "the node's prefix, below the FIN";
    EXPECT_EQ(queue.segments(), 0U);
    EXPECT_EQ(queue.bytes(), 0U) << "the bytes past the FIN left the count with the node";
    EXPECT_EQ(queue.fin(), Sequence{1150});
    pool.release_all(run.chain);
    queue.release(pool);
}

TEST_F(TcpReassembly, ReleaseHighestReturnsTheTailAndForgetsTheFin) {
    Queue queue;
    ASSERT_EQ(put(queue, 2000, 100).placed, Placed::Held);
    ASSERT_EQ(put(queue, 3000, 100).placed, Placed::Held);
    queue.remember_fin(Sequence{3100});
    EXPECT_EQ(queue.fin(), Sequence{3100});
    const std::size_t before = pool.available();
    EXPECT_EQ(queue.release_highest(pool), block(3000, 3100));
    EXPECT_FALSE(queue.fin());
    EXPECT_EQ(queue.highest(), block(2000, 2100));
    EXPECT_EQ(queue.segments(), 1U);
    EXPECT_EQ(queue.bytes(), 100U);
    EXPECT_EQ(pool.available(), before + 1);
    queue.release(pool);
}

TEST_F(TcpReassembly, AnEmptyPoolRefusesAndThePacketStaysWithTheCaller) {
    Pool small{1};
    Queue queue;
    ASSERT_EQ(put(queue, small, 2000, 100).placed, Placed::Held);
    Packet packet                 = segment(100, 7);
    const Queue::Inserted refused = queue.insert(small, packet, header_size, 100, Sequence{3000});
    EXPECT_EQ(refused.placed, Placed::NoNode);
    EXPECT_FALSE(packet.empty()) << "a refused packet stays with the caller";
    EXPECT_EQ(queue.segments(), 1U);
    EXPECT_EQ(queue.highest(), block(2000, 2100));
    queue.release(small);
}

TEST_F(TcpReassembly, AReplacingNewcomerNeedsNoFreeNode) {
    Pool small{1};
    Queue queue;
    ASSERT_EQ(put(queue, small, 2000, 100).placed, Placed::Held);
    const Queue::Inserted taken = put(queue, small, 2000, 200);
    EXPECT_EQ(taken.placed, Placed::Held);
    EXPECT_EQ(taken.kept, block(2000, 2200));
    EXPECT_EQ(queue.segments(), 1U);
    queue.release(small);
}

TEST_F(TcpReassembly, ARememberedFinKeepsTheQueueHolding) {
    Queue queue;
    EXPECT_FALSE(queue.holds());
    queue.remember_fin(Sequence{5000});
    EXPECT_TRUE(queue.holds());
    EXPECT_EQ(queue.segments(), 0U);
    EXPECT_EQ(queue.fin(), Sequence{5000});
    queue.release(pool);  // as the peer's FIN in order and a terminal event forget it
    EXPECT_FALSE(queue.holds());
    EXPECT_FALSE(queue.fin());
}

TEST_F(TcpReassembly, WrapsAcrossTheTopOfTheSequenceSpace) {
    const std::array<std::uint32_t, 4> firsts{0xffffff00U, 0xffffff80U, 0x00000000U, 0x00000080U};
    Queue queue;
    for (const std::size_t at : {2U, 0U, 3U, 1U}) {
        EXPECT_EQ(put(queue, firsts[at], 128, static_cast<std::uint8_t>(at)).placed, Placed::Held);
    }
    EXPECT_EQ(queue.highest(), block(0x00000080U, 0x00000100U));
    Queue::Run run = queue.take_run(pool, Sequence{0xffffff00U});
    EXPECT_EQ(run.chain.count, 4U);
    EXPECT_EQ(run.bytes, 512U);
    EXPECT_EQ(read(run),
              joined({slice(128, 0, 0, 128), slice(128, 1, 0, 128), slice(128, 2, 0, 128), slice(128, 3, 0, 128)}));
    EXPECT_FALSE(queue.holds());
    pool.release_all(run.chain);
}

TEST_F(TcpReassembly, ARandomStreamIsReadBackByteExact) {
    constexpr std::uint32_t start       = 1000;
    constexpr std::size_t total         = 4000;
    const std::vector<std::byte> stream = aloe::frames::pattern(total, 11);
    std::mt19937 engine{20261113};  // NOLINT(cert-msc51-cpp): a fixed seed makes a failure reproducible

    struct Cut {
        std::size_t from   = 0;
        std::size_t length = 0;
    };

    for (int round = 0; round < 200; ++round) {
        SCOPED_TRACE(round);
        std::vector<Cut> cuts;
        for (std::size_t from = 0; from < total;) {
            const std::size_t length =
                std::min(total - from, std::uniform_int_distribution<std::size_t>{1, 300}(engine));
            cuts.push_back({.from = from, .length = length});
            from += length;
        }
        const Cut held_back = cuts.front();

        // Every segment but the one at the start; a fifth widened over the next one, a fifth delivered twice.
        std::vector<Cut> arrivals;
        for (std::size_t at = 1; at < cuts.size(); ++at) {
            Cut cut = cuts[at];
            if (at + 1 < cuts.size() && std::uniform_int_distribution<int>{0, 4}(engine) == 0) {
                cut.length += std::uniform_int_distribution<std::size_t>{1, cuts[at + 1].length}(engine);
            }
            arrivals.push_back(cut);
            if (std::uniform_int_distribution<int>{0, 4}(engine) == 0) {
                arrivals.push_back(cut);
            }
        }
        std::ranges::shuffle(arrivals, engine);

        Pool nodes{cuts.size()};
        Queue queue;
        for (const Cut& cut : arrivals) {
            Packet packet                  = framed(std::span{stream}.subspan(cut.from, cut.length));
            const Queue::Inserted inserted = queue.insert(nodes,
                                                          packet,
                                                          header_size,
                                                          static_cast<std::uint16_t>(cut.length),
                                                          Sequence{static_cast<std::uint32_t>(start + cut.from)});
            ASSERT_NE(inserted.placed, Placed::NoNode);

            // The held ranges, read from the pool's taken nodes: disjoint, and in step with the counts.
            std::vector<Block> ranges;
            std::size_t bytes = 0;
            for (std::uint32_t index = 0; index < nodes.capacity(); ++index) {
                const auto& node = nodes.node(index);
                if (!node.packet.empty()) {
                    ranges.push_back({.first = node.first, .end = node.first + node.length});
                    bytes += node.length;
                }
            }
            std::ranges::sort(ranges, [](const Block& a, const Block& b) { return a.first.before(b.first); });
            for (std::size_t at = 1; at < ranges.size(); ++at) {
                ASSERT_FALSE(ranges[at].first.before(ranges[at - 1].end)) << "held ranges overlap";
            }
            ASSERT_EQ(ranges.size(), queue.segments());
            ASSERT_LE(queue.segments(), nodes.capacity());
            ASSERT_EQ(bytes, queue.bytes());
        }

        Queue::Run run = queue.take_run(nodes, Sequence{static_cast<std::uint32_t>(start + held_back.length)});
        EXPECT_EQ(read(nodes, run),
                  std::vector<std::byte>(stream.begin() + static_cast<std::ptrdiff_t>(held_back.length), stream.end()));
        EXPECT_FALSE(queue.holds());
        nodes.release_all(run.chain);
        queue.release(nodes);
    }
}
