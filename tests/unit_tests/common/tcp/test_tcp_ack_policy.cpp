#include <aloe/tcp>
#include <aloe/wire>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

namespace {

    using aloe::tcp::detail::AckPolicy;
    using Ack      = AckPolicy::Ack;
    using Sequence = aloe::wire::TcpSequence;

}  // namespace

TEST(TcpAckPolicy, ThreeAboveAGapGoAtOnceAndTheRestAtTheFlushWithSack) {
    AckPolicy policy;
    const Sequence next{1000};
    EXPECT_EQ(policy.on_out_of_order(next, true), Ack::Now);
    EXPECT_EQ(policy.on_out_of_order(next, true), Ack::Now);
    EXPECT_EQ(policy.on_out_of_order(next, true), Ack::Now);
    EXPECT_EQ(policy.on_out_of_order(next, true), Ack::Flush);
    EXPECT_EQ(policy.on_out_of_order(next, true), Ack::Flush);
}

TEST(TcpAckPolicy, EveryOneGoesAtOnceWithoutSack) {
    AckPolicy policy;
    for (int index = 0; index < 10; ++index) {
        EXPECT_EQ(policy.on_out_of_order(Sequence{1000}, false), Ack::Now) << index;
    }
}

TEST(TcpAckPolicy, TheCountStartsAgainWhenRcvNxtMoves) {
    AckPolicy policy;
    for (int index = 0; index < 3; ++index) {
        EXPECT_EQ(policy.on_out_of_order(Sequence{1000}, true), Ack::Now);
    }
    EXPECT_EQ(policy.on_out_of_order(Sequence{1000}, true), Ack::Flush);
    EXPECT_EQ(policy.on_out_of_order(Sequence{2000}, true), Ack::Now);
}

namespace {

    constexpr std::size_t full = 1448;

}  // namespace

TEST(TcpAckPolicy, WithADelayOneSegmentWaits) {
    AckPolicy policy;
    EXPECT_EQ(policy.on_data(100, full, Sequence{1101}, false, true), Ack::Delayed);
}

TEST(TcpAckPolicy, WithADelayTwoFullSegmentsWorthGoesAtOnce) {
    AckPolicy policy;
    EXPECT_EQ(policy.on_data(full, full, Sequence{1000 + full}, false, true), Ack::Delayed);
    EXPECT_EQ(policy.on_data(full, full, Sequence{1000 + (2 * full)}, false, true), Ack::Now);
    policy.acknowledged();
    // Counted in bytes: four half-sized segments are two full ones.
    for (std::size_t index = 1; index <= 3; ++index) {
        EXPECT_EQ(policy.on_data(full / 2, full, Sequence{static_cast<std::uint32_t>(index * full / 2)}, false, true),
                  Ack::Delayed)
            << index;
    }
    EXPECT_EQ(policy.on_data(full / 2, full, Sequence{static_cast<std::uint32_t>(2 * full)}, false, true), Ack::Now);
}

TEST(TcpAckPolicy, WithADelayHeldAboveStillGoesAtOnce) {
    AckPolicy policy;
    EXPECT_EQ(policy.on_data(100, full, Sequence{1100}, true, true), Ack::Now);
    EXPECT_EQ(policy.on_data(100, full, Sequence{1200}, true, false), Ack::Now) << "and without one";
    // So does data below the end of a refused segment, until RCV.NXT passes it.
    policy.gap(Sequence{1400});
    EXPECT_EQ(policy.on_data(100, full, Sequence{1300}, false, true), Ack::Now);
    EXPECT_EQ(policy.on_data(100, full, Sequence{1400}, false, true), Ack::Now);
    EXPECT_EQ(policy.on_data(100, full, Sequence{1500}, false, true), Ack::Delayed) << "past it: delayed again";
}

TEST(TcpAckPolicy, AnAckThatLeftStartsTheBytesAgain) {
    AckPolicy policy;
    EXPECT_EQ(policy.on_data(full, full, Sequence{1000 + full}, false, true), Ack::Delayed);
    policy.acknowledged();
    EXPECT_EQ(policy.on_data(full, full, Sequence{1000 + (2 * full)}, false, true), Ack::Delayed);
    EXPECT_EQ(policy.on_data(full, full, Sequence{1000 + (3 * full)}, false, true), Ack::Now);
}

TEST(TcpAckPolicy, WithoutADelayTheRuleIsTheSecondFullSegment) {
    AckPolicy policy;
    EXPECT_EQ(policy.on_data(full, full, Sequence{1000 + full}, false, false), Ack::Flush);
    EXPECT_EQ(policy.on_data(full, full, Sequence{1000 + (2 * full)}, false, false), Ack::Now);
    policy.acknowledged();
    for (std::size_t index = 1; index <= 4; ++index) {
        EXPECT_EQ(policy.on_data(full / 2, full, Sequence{static_cast<std::uint32_t>(index * full / 2)}, false, false),
                  Ack::Flush)
            << "short segments wait for the flush: " << index;
    }
}
