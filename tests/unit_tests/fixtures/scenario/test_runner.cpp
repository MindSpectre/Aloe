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

TEST_P(RunnerTest, AStarLineTakesASegmentAlreadyWaiting) {
    const auto at_once = run_text(std::string{listen_preamble} + "+0 < S 0:0(0) win 65535 <mss 1460>\n"
                                                                 "* > S. 0:0(0) ack 1 <mss 1460>\n",
                                  GetParam());
    EXPECT_TRUE(at_once.has_value()) << at_once.error().message;
    // The SYN-ACK was sent at 0; the `*` line is reached at 0.5 and still takes it.
    const auto later = run_text(std::string{listen_preamble} + "+0 < S 0:0(0) win 65535 <mss 1460>\n"
                                                               "+0.5 < . 1:1(0) ack 1 win 65535\n"
                                                               "* > S. 0:0(0) ack 1 <mss 1460>\n",
                                GetParam());
    EXPECT_TRUE(later.has_value()) << later.error().message;
}

TEST_P(RunnerTest, AnInboundUrgFails) {
    const auto result =
        run_text(std::string{listen_preamble} + "+0 < S 0:0(0) win 65535 urg 1 <mss 1460>\n", GetParam());
    EXPECT_TRUE(mentions(result, "inbound urg is not supported")) << (result ? "passed" : result.error().message);
    EXPECT_EQ(result.error().line, 5);
}

TEST_P(RunnerTest, InboundOptionsOutsideWholeWordsFail) {
    const auto result = run_text(std::string{listen_preamble} + "+0 < S 0:0(0) win 65535 <sackOK>\n", GetParam());
    EXPECT_TRUE(mentions(result, "inbound options must be a multiple of 4 bytes; pad with nop"))
        << (result ? "passed" : result.error().message);
    EXPECT_EQ(result.error().line, 5);
}

TEST(RunnerFormat, PrefixesThePathAndLine) {
    EXPECT_EQ(aloe::scenario::format(Failure{.line = 12, .message = "missing: x"}, "tests/scenarios/tcp/a.pkt"),
              "tests/scenarios/tcp/a.pkt:12: missing: x");
}

namespace {

    constexpr std::string_view passive_open = "0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n"
                                              "+0 setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0\n"
                                              "+0 bind(3, ..., ...) = 0\n"
                                              "+0 listen(3, 1) = 0\n"
                                              "+0 < S 0:0(0) win 65535 <mss 1460>\n"
                                              "+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>\n"
                                              "+0 < . 1:1(0) ack 1 win 65535\n"
                                              "+0 accept(3, ..., ...) = 4\n";

}  // namespace

TEST_P(RunnerTest, PassiveOpenWithAcceptAndClose) {
    const auto result = run_text(std::string{passive_open} + "+0 close(4) = 0\n"
                                                             "+0 > F. 1:1(0) ack 1 win 46720\n"
                                                             "+0 < . 1:1(0) ack 2 win 65535\n"
                                                             "+0 < F. 1:1(0) ack 2 win 65535\n"
                                                             "+0 > . 2:2(0) ack 2 win 46720\n",
                                 GetParam());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, AClosedConnectionsNumberIsFreedAtItsTerminalEvent) {
    const auto before = run_text(std::string{passive_open} + "+0 close(4) = 0\n"
                                                             "+0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 4\n",
                                 GetParam());
    EXPECT_TRUE(mentions(before, "descriptor 4 is in use")) << (before ? "passed" : before.error().message);

    const auto after = run_text(std::string{passive_open} + "+0 close(4) = 0\n"
                                                            "+0 > F. 1:1(0) ack 1 win 46720\n"
                                                            "+0 < . 1:1(0) ack 2 win 65535\n"
                                                            "+0 < F. 1:1(0) ack 2 win 65535\n"
                                                            "+0 > . 2:2(0) ack 2 win 46720\n"
                                                            "+0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 4\n",
                                GetParam());
    EXPECT_TRUE(after.has_value()) << after.error().message;
}

TEST_P(RunnerTest, ActiveOpenWithConnect) {
    const auto result = run_text("0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n"
                                 "+0 fcntl(3, F_SETFL, O_RDWR|O_NONBLOCK) = 0\n"
                                 "+0 connect(3, ..., ...) = -1 EINPROGRESS (Operation now in progress)\n"
                                 "+0 > S 0:0(0) win 46720 <mss 1460>\n"
                                 "+0 < S. 0:0(0) ack 1 win 65535 <mss 1460>\n"
                                 "+0 > . 1:1(0) ack 1 win 46720\n",
                                 GetParam());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, WriteSegmentsByThePeersMssAcrossInterleavedAcks) {
    const auto result = run_text("0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n"
                                 "+0 bind(3, ..., ...) = 0\n"
                                 "+0 listen(3, 1) = 0\n"
                                 "+0 < S 0:0(0) win 65535 <mss 1000>\n"
                                 "+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>\n"
                                 "+0 < . 1:1(0) ack 1 win 65535\n"
                                 "+0 accept(3, ..., ...) = 4\n"
                                 "+0 write(4, ..., 2500) = 2500\n"
                                 "+0 > P. 1:1001(1000) ack 1 win 46720\n"
                                 "+0 < . 1:1(0) ack 1001 win 65535\n"
                                 "+0 > P. 1001:2001(1000) ack 1 win 46720\n"
                                 "+0 < . 1:1(0) ack 2001 win 65535\n"
                                 "+0 > P. 2001:2501(500) ack 1 win 46720\n"
                                 "+0 < . 1:1(0) ack 2501 win 65535\n",
                                 GetParam());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, ReadConsumesWhatArrivedAndEagainWhenNothingIs) {
    const auto result = run_text(std::string{passive_open} + "+0 < P. 1:1001(1000) ack 1 win 65535\n"
                                                             "+0 > . 1:1(0) ack 1001 win 45720\n"
                                                             "+0 read(4, ..., 1000) = 1000\n"
                                                             "+0 > . 1:1(0) ack 1001 win 46720\n"
                                                             "+0 read(4, ..., 1000) = -1 EAGAIN\n",
                                 GetParam());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, WriteBeyondTheWindowFailsWithTheCountAccepted) {
    const auto result = run_text("0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n"
                                 "+0 bind(3, ..., ...) = 0\n"
                                 "+0 listen(3, 1) = 0\n"
                                 "+0 < S 0:0(0) win 500 <mss 1460>\n"
                                 "+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>\n"
                                 "+0 < . 1:1(0) ack 1 win 500\n"
                                 "+0 accept(3, ..., ...) = 4\n"
                                 "+0 write(4, ..., 1000) = 1000\n",
                                 GetParam());
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(mentions(result, "500")) << result.error().message;
    EXPECT_TRUE(mentions(result, "1000")) << result.error().message;
    EXPECT_EQ(result.error().line, 8);
}

TEST_P(RunnerTest, ReadBeyondUnreadFailsWithTheCounts) {
    const auto result = run_text(std::string{passive_open} + "+0 < P. 1:101(100) ack 1 win 65535\n"
                                                             "+0 > . 1:1(0) ack 101 win 46620\n"
                                                             "+0 read(4, ..., 1000) = 500\n",
                                 GetParam());
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(mentions(result, "100")) << result.error().message;
    EXPECT_TRUE(mentions(result, "500")) << result.error().message;
}

TEST_P(RunnerTest, AReadWithAWindowWaitsForTheTimeout) {
    HarnessConfig config;
    config.tcp.retries = 1;  // the SYN again at one second, TimedOut two seconds after that
    const auto result  = run_text("0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n"
                                  "+0 connect(3, ..., ...) = -1 EINPROGRESS (Operation now in progress)\n"
                                  "+0 > S 0:0(0) win 46720 <mss 1460>\n"
                                  "+1 > S 0:0(0) win 46720 <mss 1460>\n"
                                  // The window starts a second early so the read retries until TimedOut arrives at +2.
                                  "+1~+2.01 read(3, ..., 1) = -1 ETIMEDOUT\n",
                                  GetParam(),
                                  {},
                                  config);
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, AReadWithoutAWindowRunsOnce) {
    HarnessConfig config;
    config.tcp.retries = 1;
    const auto result  = run_text("0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n"
                                  "+0 connect(3, ..., ...) = -1 EINPROGRESS (Operation now in progress)\n"
                                  "+0 > S 0:0(0) win 46720 <mss 1460>\n"
                                  "+0.5 read(3, ..., 1) = -1 ETIMEDOUT\n",
                                  GetParam(),
                                  {},
                                  config);
    EXPECT_TRUE(mentions(result, "not satisfied")) << (result ? "passed" : result.error().message);
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(mentions(result, "SynSent")) << result.error().message;
}

TEST_P(RunnerTest, ResetGivesEconnresetAndTheSlotIsReleased) {
    HarnessConfig config;
    config.offloads   = GetParam();
    const auto script = parse_script(std::string{passive_open} + "+0 < R. 1:1(0) ack 1 win 65535\n"
                                                                 "+0 read(4, ..., 1000) = -1 ECONNRESET\n"
                                                                 "+0 < . 1:1(0) ack 1 win 65535\n"
                                                                 "+0 > R 1:1(0) win 0\n");
    ASSERT_TRUE(script.has_value()) << script.error().message;
    Harness harness{config};
    Runner runner{harness, *script};
    const auto result = runner.run();
    EXPECT_TRUE(result.has_value()) << result.error().message;
    // The RST must come from the no-connection path: a slot still indexed in Closed would answer with a RST too.
    EXPECT_EQ(harness.tcp().counters().dropped_no_connection, 1);
    EXPECT_EQ(harness.tcp().counters().dropped_closed, 0);
}

TEST_P(RunnerTest, AReleasedSlotIsReusedByTheNextSyn) {
    HarnessConfig config;
    config.offloads        = GetParam();
    config.tcp.connections = 1;
    const auto script      = parse_script(std::string{passive_open} + "+0 < R. 1:1(0) ack 1 win 65535\n"
                                                                      "+0 read(4, ..., 1000) = -1 ECONNRESET\n");
    ASSERT_TRUE(script.has_value()) << script.error().message;
    Harness harness{config};
    Runner runner{harness, *script};
    const auto result = runner.run();
    EXPECT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(harness.tcp().counters().connections_reset, 1);
    // The flow table holds no entry once the runner has released the slot.
    EXPECT_EQ(harness.tcp().table_size(), 0);
}

TEST_P(RunnerTest, ARefusedConnectGivesEconnrefused) {
    const auto result = run_text("0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n"
                                 "+0 connect(3, ..., ...) = -1 EINPROGRESS (Operation now in progress)\n"
                                 "+0 > S 0:0(0) win 46720 <mss 1460>\n"
                                 "+0 < R. 0:0(0) ack 1 win 0\n"
                                 "+0 read(3, ..., 1) = -1 ECONNREFUSED\n",
                                 GetParam());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, CloseOnAListenerUnlistens) {
    const auto result = run_text(std::string{listen_preamble} + "+0 close(3) = 0\n"
                                                                "+0 < S 0:0(0) win 65535 <mss 1460>\n"
                                                                "+0 > R. 0:0(0) ack 1 win 0\n",
                                 GetParam());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, ShutdownSendsAFinAndKeepsReading) {
    const auto result = run_text(std::string{passive_open} + "+0 shutdown(4, SHUT_WR) = 0\n"
                                                             "+0 > F. 1:1(0) ack 1 win 46720\n"
                                                             "+0 < . 1:1(0) ack 2 win 65535\n"
                                                             "+0 < P. 1:101(100) ack 2 win 65535\n"
                                                             "+0 > . 2:2(0) ack 101 win 46620\n"
                                                             "+0 read(4, ..., 100) = 100\n"
                                                             "+0 > . 2:2(0) ack 101 win 46720\n",
                                 GetParam());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_P(RunnerTest, AcceptWithNothingPendingFails) {
    const auto result = run_text(std::string{listen_preamble} + "+0 accept(3, ..., ...) = 4\n", GetParam());
    EXPECT_TRUE(mentions(result, "accept")) << (result ? "passed" : result.error().message);
    EXPECT_TRUE(mentions(result, "not satisfied"));
}

TEST_P(RunnerTest, AnUnknownSetsockoptOptionFails) {
    const auto result =
        run_text("0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n+0 setsockopt(3, SOL_TCP, TCP_MAXSEG, [1000], 4) = 0\n",
                 GetParam());
    EXPECT_TRUE(mentions(result, "TCP_MAXSEG")) << (result ? "passed" : result.error().message);
}

TEST_P(RunnerTest, ANegativeDescriptorOtherShutdownsAndFlagsFail) {
    const auto negative = run_text("0 socket(..., SOCK_STREAM, IPPROTO_TCP) = -1\n", GetParam());
    ASSERT_FALSE(negative.has_value());
    EXPECT_TRUE(mentions(negative, "socket: result -1 is not a descriptor")) << negative.error().message;

    const auto shut_rd = run_text(std::string{passive_open} + "+0 shutdown(4, SHUT_RD) = 0\n", GetParam());
    ASSERT_FALSE(shut_rd.has_value());
    EXPECT_TRUE(mentions(shut_rd, "unbound call shutdown(SHUT_RD)")) << shut_rd.error().message;

    const auto peek = run_text(std::string{passive_open} + "+0 recv(4, ..., 1000, MSG_PEEK) = -1 EAGAIN\n", GetParam());
    ASSERT_FALSE(peek.has_value());
    EXPECT_TRUE(mentions(peek, "recv: flags other than 0 are not supported")) << peek.error().message;
}
