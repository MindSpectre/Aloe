#include <aloe/frames>
#include <aloe/scenario>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using aloe::scenario::Harness;
    using aloe::scenario::HarnessConfig;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;
    using TimePoint = aloe::core::TimePoint;

    /// The peer's SYN towards the harness's listen port, built from its configuration.
    std::vector<std::byte> peer_syn(const HarnessConfig& config) {
        return aloe::frames::tcp_frame({.destination_mac  = config.stack_mac,
                                        .source_mac       = config.peer_mac,
                                        .source           = config.peer_address,
                                        .destination      = config.stack_address,
                                        .source_port      = config.peer_port,
                                        .destination_port = config.stack_port,
                                        .sequence         = config.peer_isn,
                                        .flags            = TcpFlags{TcpFlag::Syn},
                                        .mss              = 1460},
                                       {});
    }

    class HarnessTest : public ::testing::TestWithParam<aloe::fabric::EmulatedOffloads> {};

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             HarnessTest,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums));

}  // namespace

TEST_P(HarnessTest, ASynGetsASynAckWithGoodChecksums) {
    HarnessConfig config;
    config.offloads = GetParam();
    Harness harness{config};
    ASSERT_TRUE(harness.tcp().listen(config.stack_port).has_value());
    harness.inject(peer_syn(config), TimePoint{});
    harness.flush(TimePoint{});
    const auto collected = harness.collect();
    ASSERT_EQ(collected.size(), 1);
    ASSERT_TRUE(collected[0].frame.tcp.has_value());
    EXPECT_EQ(collected[0].frame.tcp->flags, (TcpFlags{TcpFlag::Syn, TcpFlag::Ack}));
    EXPECT_EQ(collected[0].frame.tcp->acknowledgement, config.peer_isn + 1);
    EXPECT_TRUE(collected[0].ipv4_checksum_ok);
    EXPECT_TRUE(collected[0].tcp_checksum_ok);
    EXPECT_TRUE(harness.collect().empty()) << "collected once";
}

TEST_P(HarnessTest, StepFiresTheWheelAndTheRetransmitComesOut) {
    HarnessConfig config;
    config.offloads = GetParam();
    Harness harness{config};
    ASSERT_TRUE(harness.tcp().listen(config.stack_port).has_value());
    harness.inject(peer_syn(config), TimePoint{});
    harness.flush(TimePoint{});
    ASSERT_EQ(harness.collect().size(), 1);
    TimePoint now{};
    for (int tick = 0; tick < 999; ++tick) {
        now += 1ms;
        harness.step(now);
        harness.flush(now);
        ASSERT_TRUE(harness.collect().empty()) << "nothing before one second, at tick " << tick;
    }
    now += 1ms;
    harness.step(now);
    harness.flush(now);
    const auto collected = harness.collect();
    ASSERT_EQ(collected.size(), 1) << "the SYN-ACK again at one second";
    EXPECT_EQ(collected[0].frame.tcp->flags, (TcpFlags{TcpFlag::Syn, TcpFlag::Ack}));
    EXPECT_EQ(harness.tcp().counters().retransmits, 1);
}

TEST_P(HarnessTest, StepHandsTheStackAFrameTheFabricDelivers) {
    HarnessConfig config;
    config.offloads = GetParam();
    Harness harness{config};
    ASSERT_TRUE(harness.tcp().listen(config.stack_port).has_value());
    harness.peer_port().set_impairment({.delay = 5ms});
    harness.inject(peer_syn(config), TimePoint{});
    harness.flush(TimePoint{});
    ASSERT_TRUE(harness.collect().empty()) << "the SYN is still on the fabric";
    const TimePoint due = TimePoint{} + 5ms;
    harness.step(due);
    harness.flush(due);
    const auto collected = harness.collect();
    ASSERT_EQ(collected.size(), 1) << "the SYN-ACK in the step the SYN arrives";
    EXPECT_EQ(collected[0].frame.tcp->flags, (TcpFlags{TcpFlag::Syn, TcpFlag::Ack}));
}

TEST_P(HarnessTest, TheArpCacheOutlivesAnyScript) {
    // `net::Ipv4` exposes no config accessor; the lifetimes show through behaviour: five minutes in, past the
    // cache's default 60 s reachable and 120 s expiry, a reply still needs no ARP request.
    HarnessConfig config;
    config.offloads = GetParam();
    Harness harness{config};
    EXPECT_EQ(harness.wheel().resolution(), 1ms);
    ASSERT_TRUE(harness.tcp().listen(config.stack_port).has_value());
    const TimePoint later = TimePoint{} + std::chrono::minutes{5};
    harness.step(later);
    harness.inject(peer_syn(config), later);
    harness.flush(later);
    const auto collected = harness.collect();
    ASSERT_EQ(collected.size(), 1);
    EXPECT_EQ(collected[0].frame.tcp->flags, (TcpFlags{TcpFlag::Syn, TcpFlag::Ack}));
    EXPECT_EQ(harness.ip().counters().arp_requests_sent, 0);
    EXPECT_EQ(harness.ip().counters().send_unresolved, 0);
}
