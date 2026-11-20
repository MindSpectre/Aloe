#include <aloe/fabric>
#include <aloe/tcp>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>

namespace {
    using aloe::tcp::AckInfo;
    using aloe::tcp::NewReno;
    using aloe::tcp::NoCongestionControl;
    constexpr std::uint16_t mss = 1000;
    constexpr auto max_window   = std::numeric_limits<std::uint32_t>::max();

    class TcpNoCongestion : public ::testing::TestWithParam<aloe::fabric::EmulatedOffloads>,
                            protected aloe::testing::detail::FabricPair,
                            protected aloe::testing::TcpHarness<aloe::fabric::Port, NoCongestionControl> {
    protected:
        TcpNoCongestion()
            : FabricPair{GetParam()},
              TcpHarness{fabric, harness, port} {
        }
    };

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpNoCongestion,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
}  // namespace

TEST(NewReno, InitialWindowFollowsRfc6928) {
    EXPECT_EQ(NewReno::initial_window(1460), 14600U);
    EXPECT_EQ(NewReno::initial_window(1000), 10000U);
    EXPECT_EQ(NewReno::initial_window(536), 5360U);
    EXPECT_EQ(NewReno::initial_window(2000), 14600U) << "ten segments would be 20000: capped at 14600";
    EXPECT_EQ(NewReno::initial_window(9000), 18000U) << "two segments is the floor";
}

TEST(NewReno, ResetGivesTheInitialWindowAndAnUnboundedThreshold) {
    NewReno::State s;
    NewReno::reset(s, mss);
    EXPECT_EQ(NewReno::window(s), 10000U);
    EXPECT_EQ(s.ssthresh, max_window);
}

TEST(NewReno, SlowStartAddsAtMostOneSegmentPerAck) {
    NewReno::State s;
    NewReno::reset(s, mss);
    NewReno::on_ack(s, AckInfo{.acked = 1500, .flight = 10000, .mss = mss});
    EXPECT_EQ(NewReno::window(s), 11000U);
    NewReno::on_ack(s, AckInfo{.acked = 400, .flight = 10000, .mss = mss});
    EXPECT_EQ(NewReno::window(s), 11400U);
}

TEST(NewReno, CongestionAvoidanceAddsOneSegmentPerWindowAcknowledged) {
    NewReno::State s{.cwnd = 5000, .ssthresh = 5000, .acked_bytes = 0};
    for (int i = 0; i < 4; ++i) {
        NewReno::on_ack(s, AckInfo{.acked = 1000, .flight = 5000, .mss = mss});
        EXPECT_EQ(NewReno::window(s), 5000U) << "after " << i + 1 << " of 5 acks";
    }
    NewReno::on_ack(s, AckInfo{.acked = 1000, .flight = 5000, .mss = mss});
    EXPECT_EQ(NewReno::window(s), 6000U);
    EXPECT_EQ(s.acked_bytes, 0U);
}

TEST(NewReno, CongestionHalvesTheFlightAndLeavesTheWindow) {
    NewReno::State s;
    NewReno::reset(s, 1460);
    NewReno::on_congestion(s, 14600, 1460);
    EXPECT_EQ(s.ssthresh, 7300U);
    EXPECT_EQ(s.cwnd, 14600U) << "the way down to the target is the rate reduction's";
    EXPECT_EQ(NewReno::target(s), 7300U);
}

TEST(NewReno, CongestionFloorsTheTargetAtTwoSegments) {
    NewReno::State s;
    NewReno::reset(s, 1460);
    NewReno::on_congestion(s, 1460, 1460);
    EXPECT_EQ(s.ssthresh, 2920U);
    EXPECT_EQ(NewReno::target(s), 2920U);
}

TEST(NewReno, CongestionStartsTheAvoidanceCountAgain) {
    NewReno::State s{.cwnd = 5000, .ssthresh = 5000, .acked_bytes = 3000};
    NewReno::on_congestion(s, 5000, mss);
    EXPECT_EQ(s.acked_bytes, 0U);
}

TEST(NewReno, RecoveryEndSetsTheWindowToTheTarget) {
    NewReno::State s;
    NewReno::reset(s, 1460);
    NewReno::on_congestion(s, 14600, 1460);
    s.acked_bytes = 500;
    NewReno::on_recovery_end(s);
    EXPECT_EQ(s.cwnd, 7300U);
    EXPECT_EQ(s.ssthresh, 7300U);
    EXPECT_EQ(s.acked_bytes, 0U);
}

TEST(NewReno, ATimeoutRestartsAtWhatIsInFlightPlusOneSegment) {
    NewReno::State s;
    NewReno::reset(s, 1460);
    NewReno::on_timeout(s, 0, 1460);
    EXPECT_EQ(s.cwnd, 1460U) << "everything was marked lost: one segment";
    EXPECT_EQ(s.ssthresh, max_window) << "the target is on_congestion's";
    NewReno::on_timeout(s, 2920, 1460);
    EXPECT_EQ(s.cwnd, 4380U);
    EXPECT_EQ(s.ssthresh, max_window);
    NewReno::on_timeout(s, max_window - 100, 1460);
    EXPECT_EQ(s.cwnd, max_window) << "saturating";
}

TEST(NewReno, UndoRestoresTheLargerOfEachPair) {
    NewReno::State s;
    NewReno::reset(s, 1460);
    const NewReno::State copy = s;
    NewReno::on_congestion(s, 14600, 1460);
    NewReno::on_recovery_end(s);
    ASSERT_EQ(s.cwnd, 7300U);
    NewReno::on_undo(s, copy);
    EXPECT_EQ(s.cwnd, copy.cwnd);
    EXPECT_EQ(s.ssthresh, copy.ssthresh);
    NewReno::State grown{.cwnd = 20000, .ssthresh = 30000, .acked_bytes = 0};
    NewReno::on_undo(grown, NewReno::State{.cwnd = 14600, .ssthresh = 7300, .acked_bytes = 0});
    EXPECT_EQ(grown.cwnd, 20000U) << "a window grown past the copy's stays";
    EXPECT_EQ(grown.ssthresh, 30000U);
}

TEST(NewReno, IdleRestartsAtTheSmallerOfTheInitialWindowAndTheWindow) {
    NewReno::State s;
    NewReno::reset(s, mss);
    NewReno::on_congestion(s, 4000, mss);
    NewReno::on_timeout(s, 0, mss);
    NewReno::on_idle(s, mss);
    EXPECT_EQ(NewReno::window(s), 1000U) << "RFC 5681: min(IW, cwnd) never raises a window a timeout cut";
    EXPECT_EQ(s.ssthresh, 2000U) << "idle keeps the threshold";
    NewReno::State grown{.cwnd = 20000, .ssthresh = max_window, .acked_bytes = 0};
    NewReno::on_idle(grown, mss);
    EXPECT_EQ(NewReno::window(grown), 10000U) << "twenty segments restart at the initial ten";
}

TEST(NewReno, AdditionsSaturate) {
    NewReno::State s{.cwnd = max_window - 500, .ssthresh = max_window, .acked_bytes = 0};
    NewReno::on_ack(s, AckInfo{.acked = 1000, .flight = 1000, .mss = mss});
    EXPECT_EQ(NewReno::window(s), max_window);
}

TEST(NoCongestionControl, TheWindowAndTheTargetAreUnboundedAndNothingChangesThem) {
    NoCongestionControl::State s;
    NoCongestionControl::reset(s, mss);
    const NoCongestionControl::State copy = s;
    NoCongestionControl::on_ack(s, AckInfo{.acked = 1000, .flight = 10000, .mss = mss});
    NoCongestionControl::on_congestion(s, 10000, mss);
    EXPECT_EQ(NoCongestionControl::target(s), max_window) << "no target: the rate reduction has nothing to regulate";
    NoCongestionControl::on_recovery_end(s);
    NoCongestionControl::on_timeout(s, 0, mss);
    NoCongestionControl::on_idle(s, mss);
    NoCongestionControl::on_undo(s, copy);
    EXPECT_EQ(NoCongestionControl::window(s), max_window);
    EXPECT_EQ(NoCongestionControl::target(s), max_window);
}

TEST_P(TcpNoCongestion, APassiveOpenReportsAnUnboundedWindow) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->congestion_window(), max_window);
}
