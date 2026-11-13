#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using aloe::tcp::detail::SackBlocks;
    using Block    = aloe::wire::TcpSackBlock;
    using Sequence = aloe::wire::TcpSequence;
    using Blocks   = std::vector<Block>;

    [[nodiscard]] Block B(const std::uint32_t first, const std::uint32_t end) {
        return Block{.first = Sequence{first}, .end = Sequence{end}};
    }

    /// What the next pure ACK would carry with room for `room` blocks.
    [[nodiscard]] Blocks blocks(const SackBlocks& sack, const std::size_t room = 4) {
        std::array<Block, SackBlocks::capacity> out{};
        const std::size_t count = sack.report(out, room);
        return {out.begin(), out.begin() + static_cast<std::ptrdiff_t>(count)};
    }

    void held(SackBlocks& sack, const std::uint32_t first, const std::uint32_t end) {
        sack.held(Sequence{first}, Sequence{end});
    }

    void duplicate(SackBlocks& sack, const std::uint32_t first, const std::uint32_t end) {
        sack.duplicate(Sequence{first}, Sequence{end});
    }

    class TcpSackBlocks : public ::testing::Test {
    protected:
        TcpSackBlocks() {
            sack.offer(true);
            sack.answer(true);
        }

        SackBlocks sack;
    };

}  // namespace

TEST_F(TcpSackBlocks, TheNewestBlockIsFirst) {
    held(sack, 2000, 2100);
    held(sack, 4000, 4100);
    held(sack, 3000, 3100);
    EXPECT_EQ(blocks(sack), (Blocks{B(3000, 3100), B(4000, 4100), B(2000, 2100)}));
    EXPECT_EQ(sack.count(), 3U);
    EXPECT_TRUE(sack.pending());
}

TEST_F(TcpSackBlocks, ATouchingSegmentExtendsItsBlockAndMovesItToTheFront) {
    held(sack, 2000, 2100);
    held(sack, 4000, 4100);
    held(sack, 2100, 2200);
    EXPECT_EQ(blocks(sack), (Blocks{B(2000, 2200), B(4000, 4100)}));
}

TEST_F(TcpSackBlocks, ASegmentThatJoinsTwoBlocksMergesThem) {
    held(sack, 2000, 2100);
    held(sack, 2200, 2300);
    held(sack, 2100, 2200);
    EXPECT_EQ(blocks(sack), (Blocks{B(2000, 2300)}));
    EXPECT_EQ(sack.count(), 1U);
}

TEST_F(TcpSackBlocks, TheFifthRangeDropsTheOldest) {
    for (const std::uint32_t first : {2000U, 3000U, 4000U, 5000U, 6000U}) {
        held(sack, first, first + 100);
    }
    EXPECT_EQ(blocks(sack), (Blocks{B(6000, 6100), B(5000, 5100), B(4000, 4100), B(3000, 3100)}));
    EXPECT_EQ(sack.count(), 4U);
}

TEST_F(TcpSackBlocks, BlocksGoAsRcvNxtPassesThem) {
    held(sack, 2000, 2100);
    held(sack, 3000, 3100);
    sack.advanced(Sequence{2100});
    EXPECT_EQ(blocks(sack), (Blocks{B(3000, 3100)}));
    sack.advanced(Sequence{3050});
    EXPECT_EQ(blocks(sack), (Blocks{B(3050, 3100)}));
    sack.advanced(Sequence{3100});
    EXPECT_FALSE(sack.pending());
}

TEST_F(TcpSackBlocks, AReleaseCutsTheBlockBack) {
    held(sack, 2000, 2300);
    sack.released(Sequence{2200}, Sequence{2300});
    EXPECT_EQ(blocks(sack), (Blocks{B(2000, 2200)}));
    sack.released(Sequence{2000}, Sequence{2200});
    EXPECT_TRUE(blocks(sack).empty());
    EXPECT_FALSE(sack.pending());
}

TEST_F(TcpSackBlocks, AReleaseAtTheHeadOfABlockKeepsWhatFollows) {
    held(sack, 3000, 3100);
    held(sack, 2999, 3000);  // a FIN remembered just below held data joins its block
    EXPECT_EQ(blocks(sack), (Blocks{B(2999, 3100)}));
    sack.released(Sequence{2999}, Sequence{3000});
    EXPECT_EQ(blocks(sack), (Blocks{B(3000, 3100)}));
}

TEST_F(TcpSackBlocks, ThreeBlocksFitBesideTimestampsAndFourWithout) {
    for (const std::uint32_t first : {2000U, 3000U, 4000U, 5000U}) {
        held(sack, first, first + 100);
    }
    EXPECT_EQ(blocks(sack, 3).size(), 3U);
    EXPECT_EQ(blocks(sack, 4).size(), 4U);
    EXPECT_EQ(blocks(sack, 3), (Blocks{B(5000, 5100), B(4000, 4100), B(3000, 3100)})) << "the newest kept";
}

TEST_F(TcpSackBlocks, ADuplicateIsFirstAndForgottenOnceReported) {
    held(sack, 2000, 2100);
    duplicate(sack, 900, 1000);
    EXPECT_TRUE(sack.duplicate_pending());
    EXPECT_EQ(blocks(sack), (Blocks{B(900, 1000), B(2000, 2100)}));
    sack.reported();
    EXPECT_FALSE(sack.duplicate_pending());
    EXPECT_EQ(blocks(sack), (Blocks{B(2000, 2100)}));
}

TEST_F(TcpSackBlocks, ASecondDuplicateExtendsThePendingOneOnlyWhenTheyTouch) {
    duplicate(sack, 900, 950);
    duplicate(sack, 950, 1000);
    duplicate(sack, 1200, 1300);
    const Blocks report = blocks(sack);
    ASSERT_FALSE(report.empty());
    EXPECT_EQ(report.front(), B(900, 1000));
    EXPECT_EQ(report.size(), 1U) << "the one that did not touch was dropped";
}

TEST_F(TcpSackBlocks, APayloadSegmentForgetsThePendingDuplicate) {
    duplicate(sack, 900, 1000);
    sack.forget_duplicate();
    EXPECT_FALSE(sack.pending());
    EXPECT_FALSE(sack.duplicate_pending());
}

TEST_F(TcpSackBlocks, ADuplicateAloneIsPending) {
    duplicate(sack, 900, 1000);
    EXPECT_TRUE(sack.pending());
    EXPECT_EQ(blocks(sack), (Blocks{B(900, 1000)}));
    EXPECT_EQ(sack.count(), 0U) << "the duplicate is not a block held";
}

TEST_F(TcpSackBlocks, ARememberedFinJoinsTheBlockThatEndsAtIt) {
    held(sack, 2000, 2100);
    held(sack, 2100, 2101);
    EXPECT_EQ(blocks(sack), (Blocks{B(2000, 2101)}));
    held(sack, 5000, 5001);
    EXPECT_EQ(blocks(sack), (Blocks{B(5000, 5001), B(2000, 2101)}));
}

TEST_F(TcpSackBlocks, NothingIsHeldOrReportedWhenSackIsNotInUse) {
    sack.answer(false);
    held(sack, 2000, 2100);
    duplicate(sack, 900, 1000);
    EXPECT_FALSE(sack.pending());
    EXPECT_FALSE(sack.duplicate_pending());
    EXPECT_EQ(sack.count(), 0U);
    EXPECT_TRUE(blocks(sack).empty());
}

TEST_F(TcpSackBlocks, DropKeepsTheNegotiation) {
    held(sack, 2000, 2100);
    duplicate(sack, 900, 1000);
    sack.drop();
    EXPECT_EQ(sack.count(), 0U);
    EXPECT_FALSE(sack.pending());
    EXPECT_TRUE(sack.in_use());
}

TEST_F(TcpSackBlocks, WrapsAcrossTheTopOfTheSequenceSpace) {
    held(sack, 0xffffff00U, 0xffffff80U);
    held(sack, 0xffffff80U, 0x00000040U);
    EXPECT_EQ(blocks(sack), (Blocks{B(0xffffff00U, 0x00000040U)}));
    sack.advanced(Sequence{0xffffffc0U});
    EXPECT_EQ(blocks(sack), (Blocks{B(0xffffffc0U, 0x00000040U)}));
}
