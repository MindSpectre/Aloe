#include <aloe/tcp>
#include <array>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

// PRR, RFC 9937 in bytes, with a segment of 1000 so that the RFC's figures, drawn in whole segments, read in
// thousands. A send follows a step wherever the window it left is above what is in flight.
namespace {

    using aloe::tcp::detail::RateReduction;
    constexpr std::uint16_t mss = 1000;

    /// One ACK of a figure: what is in flight when it arrives, the window the step leaves, and whether a segment may
    /// leave on it.
    struct Step {
        std::uint32_t in_flight = 0;
        std::uint32_t window    = 0;
        bool sends              = false;
    };

}  // namespace

TEST(TcpRateReduction, Figure1TheProportionalPartSpreadsTheReduction) {
    // RFC 9937, Figure 1, from ACK 3, where recovery begins, to ACK 21: twenty segments in flight, the first lost,
    // a target of ten. The row of what is in flight is the figure's. The figure counts whole segments, so its window
    // on every second ACK is one segment above what is in flight; in bytes the proportional part allows
    // ceil(prr_delivered * 10000 / 20000) - prr_out, half a segment, which lets the same segment out. At ACK 19 the
    // figure's 10 in flight is not above the target, so in bytes the conservative bound holds the window at the
    // target, min(10000 - 10000, ...) = 0, where the figure shows 11 and its ninth send; the ninth leaves at ACK 21.
    constexpr std::array steps{
        Step{.in_flight = 18000, .window = 18500, .sends = true },
        Step{.in_flight = 18000, .window = 18000, .sends = false},
        Step{.in_flight = 17000, .window = 17500, .sends = true },
        Step{.in_flight = 17000, .window = 17000, .sends = false},
        Step{.in_flight = 16000, .window = 16500, .sends = true },
        Step{.in_flight = 16000, .window = 16000, .sends = false},
        Step{.in_flight = 15000, .window = 15500, .sends = true },
        Step{.in_flight = 15000, .window = 15000, .sends = false},
        Step{.in_flight = 14000, .window = 14500, .sends = true },
        Step{.in_flight = 14000, .window = 14000, .sends = false},
        Step{.in_flight = 13000, .window = 13500, .sends = true },
        Step{.in_flight = 13000, .window = 13000, .sends = false},
        Step{.in_flight = 12000, .window = 12500, .sends = true },
        Step{.in_flight = 12000, .window = 12000, .sends = false},
        Step{.in_flight = 11000, .window = 11500, .sends = true },
        Step{.in_flight = 11000, .window = 11000, .sends = false},
        Step{.in_flight = 10000, .window = 10000, .sends = false},
        Step{.in_flight = 10000, .window = 10000, .sends = false},
        Step{.in_flight = 9000,  .window = 10000, .sends = true },
    };
    RateReduction reduction;
    reduction.begin(20000, 10000);
    std::uint32_t sent = 0;
    for (std::size_t index = 0; index < steps.size(); ++index) {
        const Step& step = steps[index];
        reduction.on_ack(1000, step.in_flight, false, mss);
        EXPECT_EQ(reduction.window(), step.window) << "ACK " << index + 3;
        EXPECT_EQ(reduction.window() > step.in_flight, step.sends) << "ACK " << index + 3;
        if (reduction.window() > step.in_flight) {
            reduction.on_sent(1000);
            ++sent;
        }
    }
    EXPECT_EQ(sent, 9U) << "nine segments for nineteen delivered";
    EXPECT_EQ(reduction.delivered(), 19000U);
}

TEST(TcpRateReduction, Figure2TheConservativeBoundFollowsWhatWasDelivered) {
    // RFC 9937, Figure 2: fifteen of twenty segments lost, so what is in flight, 4, is below the target and each ACK
    // lets out what it delivered: a window of 5 at each of ACKs 17 to 19.
    RateReduction reduction;
    reduction.begin(20000, 10000);
    reduction.on_ack(1000, 4000, false, mss);
    EXPECT_EQ(reduction.window(), 5000U);
    reduction.on_sent(1000);
    reduction.on_ack(1000, 4000, false, mss);
    EXPECT_EQ(reduction.window(), 5000U);
    reduction.on_sent(1000);
    reduction.on_ack(1000, 4000, false, mss);
    EXPECT_EQ(reduction.window(), 5000U);
}

TEST(TcpRateReduction, ASafeAckAddsOneSegmentBelowTheTarget) {
    RateReduction reduction;
    reduction.begin(20000, 10000);
    reduction.on_ack(1000, 4000, true, mss);
    EXPECT_EQ(reduction.window(), 6000U) << "what was delivered, and a segment for the progress";
}

TEST(TcpRateReduction, TheBoundNeverPassesTheTarget) {
    RateReduction reduction;
    reduction.begin(20000, 10000);
    reduction.on_ack(8000, 9500, true, mss);
    EXPECT_EQ(reduction.window(), 10000U) << "min(10000 - 9500, 8000 + 1000) above what is in flight";
}

TEST(TcpRateReduction, TheFirstStepAlwaysAllowsOneSegment) {
    RateReduction reduction;
    reduction.begin(20000, 10000);
    reduction.on_ack(1, 18000, false, mss);
    EXPECT_EQ(reduction.window(), 18001U) << "ceil(1 * 10000 / 20000) = 1: above what is in flight, so a resend leaves";
    reduction.begin(20000, 0);
    reduction.on_ack(1000, 18000, false, mss);
    EXPECT_EQ(reduction.window(), 19000U) << "a target of zero allows nothing, and nothing was sent: one segment";
    reduction.on_sent(1000);
    reduction.on_ack(1000, 18000, false, mss);
    EXPECT_EQ(reduction.window(), 18000U) << "only the first";
}

TEST(TcpRateReduction, AnAckThatDeliveredNothingChangesNothing) {
    RateReduction reduction;
    reduction.begin(20000, 10000);
    reduction.on_ack(1000, 18000, false, mss);
    ASSERT_EQ(reduction.window(), 18500U);
    reduction.on_ack(0, 5000, true, mss);
    EXPECT_EQ(reduction.window(), 18500U);
    EXPECT_EQ(reduction.delivered(), 1000U);
}

TEST(TcpRateReduction, AllowOneIsInFlightPlusOneSegment) {
    RateReduction reduction;
    reduction.begin(20000, 10000);
    reduction.allow_one(7000, mss);
    EXPECT_EQ(reduction.window(), 8000U);
    EXPECT_EQ(reduction.delivered(), 0U) << "no ACK: nothing was delivered";
}

TEST(TcpRateReduction, BeginStartsTheEpisodesCountsAgain) {
    RateReduction reduction;
    reduction.begin(20000, 10000);
    reduction.on_ack(3000, 18000, false, mss);
    reduction.on_sent(1000);
    EXPECT_EQ(reduction.recover_fs(), 20000U);
    EXPECT_EQ(reduction.delivered(), 3000U);
    reduction.begin(8000, 4000);
    EXPECT_EQ(reduction.recover_fs(), 8000U);
    EXPECT_EQ(reduction.delivered(), 0U);
    reduction.on_ack(1000, 7000, false, mss);
    EXPECT_EQ(reduction.window(), 7500U) << "ceil(1000 * 4000 / 8000), with nothing counted as sent";
}
