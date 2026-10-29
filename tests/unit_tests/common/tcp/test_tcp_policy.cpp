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

    static_assert(aloe::tcp::IsCongestionControl<NewReno>);
    static_assert(aloe::tcp::IsCongestionControl<NoCongestionControl>);
    static_assert(aloe::tcp::IsCongestionControl<aloe::testing::RecordingCongestionControl>);

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

TEST(NewReno, LossHalvesTheFlightAndInflatesByThree) {
    NewReno::State s;
    NewReno::reset(s, mss);
    NewReno::on_loss(s, 10000, mss);
    EXPECT_EQ(s.ssthresh, 5000U);
    EXPECT_EQ(NewReno::window(s), 8000U);
    NewReno::on_dupack(s, mss);
    EXPECT_EQ(NewReno::window(s), 9000U);
    NewReno::on_partial_ack(s, 1000, mss);
    EXPECT_EQ(NewReno::window(s), 9000U) << "deflated by 1000, one segment added back";
    NewReno::on_partial_ack(s, 500, mss);
    EXPECT_EQ(NewReno::window(s), 8500U) << "less than a segment: nothing added back";
    NewReno::on_recovery_end(s, 3000, mss);
    EXPECT_EQ(NewReno::window(s), 4000U) << "min(5000, max(3000, 1000) + 1000)";
}

TEST(NewReno, LossFloorsTheThresholdAtTwoSegments) {
    NewReno::State s;
    NewReno::reset(s, mss);
    NewReno::on_loss(s, 1000, mss);
    EXPECT_EQ(s.ssthresh, 2000U);
    EXPECT_EQ(NewReno::window(s), 5000U);
}

TEST(NewReno, PartialAckNeverDeflatesBelowOneSegment) {
    NewReno::State s{.cwnd = 1200, .ssthresh = 5000, .acked_bytes = 0};
    NewReno::on_partial_ack(s, 900, mss);
    EXPECT_EQ(NewReno::window(s), 1000U);
}

TEST(NewReno, TimeoutRestartsAtOneSegment) {
    NewReno::State s;
    NewReno::reset(s, mss);
    NewReno::on_timeout(s, 4000, mss);
    EXPECT_EQ(s.ssthresh, 2000U);
    EXPECT_EQ(NewReno::window(s), 1000U);
}

TEST(NewReno, IdleRestartsAtTheSmallerOfTheInitialWindowAndTheWindow) {
    NewReno::State s;
    NewReno::reset(s, mss);
    NewReno::on_timeout(s, 4000, mss);
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

TEST(NoCongestionControl, TheWindowIsUnboundedAndNothingChangesIt) {
    NoCongestionControl::State s;
    NoCongestionControl::reset(s, mss);
    NoCongestionControl::on_loss(s, 10000, mss);
    NoCongestionControl::on_timeout(s, 10000, mss);
    EXPECT_EQ(NoCongestionControl::window(s), max_window);
}

TEST_P(TcpNoCongestion, APassiveOpenReportsAnUnboundedWindow) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->congestion_window(), max_window);
}
