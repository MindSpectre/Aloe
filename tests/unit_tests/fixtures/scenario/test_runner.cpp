#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/scenario>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using aloe::scenario::Failure;
    using aloe::scenario::Harness;
    using aloe::scenario::HarnessConfig;
    using aloe::scenario::parse_script;
    using aloe::scenario::Runner;
    using aloe::scenario::RunnerConfig;

    constexpr std::string_view listen_preamble = "0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n"
                                                 "+0 setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0\n"
                                                 "+0 bind(3, ..., ...) = 0\n"
                                                 "+0 listen(3, 1) = 0\n";

    /// Parses and runs `text` on a fresh harness in the given checksum mode.
    std::expected<void, Failure>
    run_text(const std::string& text,
             const aloe::fabric::EmulatedOffloads offloads = aloe::fabric::EmulatedOffloads::None,
             const RunnerConfig& runner_config             = {},
             HarnessConfig config                          = {}) {
        const auto script = parse_script(text);
        EXPECT_TRUE(script.has_value()) << script.error().message;
        if (!script) {
            return std::unexpected{
                Failure{.line = script.error().line, .message = script.error().message}
            };
        }
        config.offloads = offloads;
        Harness harness{config};
        Runner runner{harness, *script, runner_config};
        return runner.run();
    }

    bool mentions(const std::expected<void, Failure>& result, const std::string_view word) {
        return !result.has_value() && result.error().message.find(word) != std::string_view::npos;
    }

    class RunnerTest : public ::testing::TestWithParam<aloe::fabric::EmulatedOffloads> {};

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             RunnerTest,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums));

}  // namespace

TEST_P(RunnerTest, AnExpectedSegmentWithinItsWindowPasses) {
    const auto result = run_text(std::string{listen_preamble} + "+0 < S 0:0(0) win 65535 <mss 1460>\n"
                                                                "+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>\n",
                                 GetParam());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, TheRetransmitComesAtPlusOne) {
    const auto result = run_text(std::string{listen_preamble} + "+0 < S 0:0(0) win 65535 <mss 1460>\n"
                                                                "+0 > S. 0:0(0) ack 1 <mss 1460>\n"
                                                                "+1 > S. 0:0(0) ack 1 <mss 1460>\n"
                                                                "+2 > S. 0:0(0) ack 1 <mss 1460>\n",
                                 GetParam());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, ASegmentThatComesEarlyFails) {
    const auto result = run_text(std::string{listen_preamble} + "+0 < S 0:0(0) win 65535 <mss 1460>\n"
                                                                "+0 > S. 0:0(0) ack 1 <mss 1460>\n"
                                                                "+2 > S. 0:0(0) ack 1 <mss 1460>\n",
                                 GetParam());
    EXPECT_TRUE(mentions(result, "early")) << (result ? "passed" : result.error().message);
    EXPECT_EQ(result.error().line, 7);
}

TEST_P(RunnerTest, AMissingSegmentFails) {
    const auto result = run_text(std::string{listen_preamble} + "+0 > S. 0:0(0) ack 1 <mss 1460>\n", GetParam());
    EXPECT_TRUE(mentions(result, "missing")) << (result ? "passed" : result.error().message);
    EXPECT_EQ(result.error().line, 5);
}

TEST_P(RunnerTest, AMismatchNamesTheFieldAndBothSegments) {
    const auto result = run_text(std::string{listen_preamble} + "+0 < S 0:0(0) win 65535 <mss 1460>\n"
                                                                "+0 > S 0:0(0) <mss 1460>\n",
                                 GetParam());
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(mentions(result, "flags differ")) << result.error().message;
    EXPECT_TRUE(mentions(result, "> S 0:0(0) <mss 1460>")) << result.error().message;
    EXPECT_TRUE(mentions(result, "> S. 0:0(0) ack 1 win 46720 <mss 1460>")) << result.error().message;
    EXPECT_EQ(result.error().line, 6);
}

TEST_P(RunnerTest, TheQueueSurvivesAnInboundLineBetweenTwoOutboundLines) {
    const auto result = run_text(std::string{listen_preamble} + "+0 < S 0:0(0) win 65535 <mss 1460>\n"
                                                                "+0 < . 1:1(0) ack 1 win 65535\n"
                                                                "+0 > S. 0:0(0) ack 1 <mss 1460>\n",
                                 GetParam());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, AnUnexpectedSegmentAtTheEndFails) {
    const auto result = run_text(std::string{listen_preamble} + "+0 < S 0:0(0) win 65535 <mss 1460>\n", GetParam());
    EXPECT_TRUE(mentions(result, "unexpected")) << (result ? "passed" : result.error().message);
    EXPECT_TRUE(mentions(result, "> S. 0:0(0) ack 1 win 46720 <mss 1460>"));
}

TEST_P(RunnerTest, AnAbsoluteTimeGoingBackwardsFails) {
    const auto result = run_text(std::string{listen_preamble} + "0.5 < S 0:0(0) win 65535 <mss 1460>\n"
                                                                "0.5 > S. 0:0(0) ack 1 <mss 1460>\n"
                                                                "0.2 < . 1:1(0) ack 1 win 65535\n",
                                 GetParam());
    EXPECT_TRUE(mentions(result, "backwards")) << (result ? "passed" : result.error().message);
    EXPECT_EQ(result.error().line, 7);
}

TEST_P(RunnerTest, AnAnyLineGivesUpAtTheBound) {
    const auto result = run_text(
        std::string{listen_preamble} + "* > S. 0:0(0) ack 1 <mss 1460>\n", GetParam(), RunnerConfig{.any_bound = 2s});
    EXPECT_TRUE(mentions(result, "missing")) << (result ? "passed" : result.error().message);
    EXPECT_TRUE(mentions(result, "2.000000"));
}

TEST_P(RunnerTest, TheScriptsToleranceOverridesTheRunners) {
    // The SYN-ACK retransmit lands at exactly one second; a window ending at 0.9 with a 200 ms tolerance admits it.
    const auto result = run_text("--tolerance_usecs=200000\n" + std::string{listen_preamble} +
                                     "+0 < S 0:0(0) win 65535 <mss 1460>\n"
                                     "+0 > S. 0:0(0) ack 1 <mss 1460>\n"
                                     "+0.9 > S. 0:0(0) ack 1 <mss 1460>\n",
                                 GetParam());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, AnUnboundCallFailsWithItsName) {
    const auto result = run_text(
        "0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n+0 getsockopt(3, SOL_TCP, TCP_INFO, ..., ...) = 0\n", GetParam());
    EXPECT_TRUE(mentions(result, "getsockopt")) << (result ? "passed" : result.error().message);
    EXPECT_EQ(result.error().line, 2);
}

TEST_P(RunnerTest, ABadChecksumFailsEvenWhenEveryFieldMatches) {
    HarnessConfig config;
    config.offloads = GetParam();
    Harness harness{config};
    // A forged SYN-ACK from the stack's own port with a corrupted TCP checksum: every field a script would check is
    // right.
    const auto forged = aloe::frames::tcp_frame(
        {
            .destination_mac  = config.peer_mac,
            .source_mac       = config.stack_mac,
            .source           = config.stack_address,
            .destination      = config.peer_address,
            .source_port      = config.stack_port,
            .destination_port = config.peer_port,
            .sequence         = aloe::wire::TcpSequence{7000},
            .acknowledgement  = config.peer_isn + 1,
            .flags            = {aloe::wire::TcpFlag::Syn, aloe::wire::TcpFlag::Ack},
            .window           = 46720,
            .mss              = 1460,
            .checksums        = aloe::frames::Checksums::Wrong
    },
        {});
    auto packet = harness.stack_port().allocate(0);
    ASSERT_TRUE(packet.has_value());
    ASSERT_TRUE(aloe::frames::fill(*packet, forged));
    std::array<aloe::fabric::Packet, 1> burst{std::move(*packet)};
    ASSERT_EQ(harness.stack_port().transmit(0, burst), 1);

    const auto script = parse_script("+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>\n");
    ASSERT_TRUE(script.has_value());
    Runner runner{harness, *script};
    const auto result = runner.run();
    EXPECT_TRUE(mentions(result, "checksum")) << (result ? "passed" : result.error().message);
}

TEST(RunnerFormat, PrefixesThePathAndLine) {
    EXPECT_EQ(aloe::scenario::format(Failure{.line = 12, .message = "missing: x"}, "tests/scenarios/tcp/a.pkt"),
              "tests/scenarios/tcp/a.pkt:12: missing: x");
}
