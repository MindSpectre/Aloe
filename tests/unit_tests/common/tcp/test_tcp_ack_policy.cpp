#include <aloe/tcp>
#include <aloe/wire>

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
