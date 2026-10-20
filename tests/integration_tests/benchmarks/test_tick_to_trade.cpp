#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <benchmark_options.hpp>
#include <benchmark_run.hpp>
#include <eal_environment.hpp>
#include <gtest/gtest.h>
#include <machine_info.hpp>
#include <run_state.hpp>
#include <tsc_clock.hpp>
#include <tsc_scale.hpp>

// The whole matrix, small, under the test EAL: every configuration runs to a clean end over one ring pair, the rows
// come out complete, and the stamp checks refuse a hole and an inverted pair. No root, no hugepages, no pinning.
namespace {

    using namespace aloe::benchmarks;

    const auto* const environment = ::testing::AddGlobalTestEnvironment(new aloe::testing::EalEnvironment{"net_null0"});

    [[nodiscard]] Options parse(std::vector<std::string> words) {
        words.insert(words.begin(), "tick_to_trade");
        std::vector<char*> argv;
        argv.reserve(words.size());
        for (std::string& word : words) {
            argv.push_back(word.data());
        }
        auto options = parse_options(argv);
        if (!options) {
            throw std::runtime_error{options.error()};
        }
        return *options;
    }

    [[nodiscard]] std::size_t count_metric(const Report& report, const std::string& metric) {
        return static_cast<std::size_t>(
            std::ranges::count_if(report.rows, [&](const Row& row) { return row.metric == metric; }));
    }

    /// What `summarise_round` refuses `state` with; empty when it does not refuse.
    [[nodiscard]] std::string refusal(const RunState& state, const RoundConfig& config) {
        try {
            std::ignore = summarise_round(state, aloe::core::TscClock::scale(), config);
        } catch (const std::runtime_error& error) {
            return error.what();
        }
        return {};
    }

}  // namespace

TEST(TickToTrade, TheWholeMatrixRunsSmall) {
    Options options     = parse({"--rounds",
                                 "60",
                                 "--warmup",
                                 "6",
                                 "--payload",
                                 "64,1460",
                                 "--topology",
                                 "both",
                                 "--stack-core",
                                 "none",
                                 "--peer-core",
                                 "none",
                                 "--breakdown",
                                 "--hop"});
    options.ring_name   = "whole-matrix";
    const Report report = run_benchmark(options, probe_machine(), "test");
    EXPECT_EQ(count_metric(report, "round"), 15U) << "tick: (2 + 2) x 2 + 2; gateway: 2 + 2 + 1";
    for (const Row& row : report.rows) {
        EXPECT_EQ(row.summary.count, 60U) << row.round << ' ' << row.level << ' ' << row.metric;
        EXPECT_GT(row.summary.min, 0U) << row.metric;
        EXPECT_LE(row.summary.min, row.summary.p50);
        EXPECT_LE(row.summary.p50, row.summary.p99);
        EXPECT_LE(row.summary.p99, row.summary.max);
    }
    for (const char* leg : {"receive", "ip", "tcp"}) {
        EXPECT_EQ(count_metric(report, leg), 4U) << leg << ": level 0 tick rows, 2 payloads x 2 topologies";
    }
    for (const char* leg : {"application", "tcp-flush", "queue-flush"}) {
        EXPECT_EQ(count_metric(report, leg), 6U) << leg << ": those four and the gateway's two topologies";
    }
    EXPECT_EQ(count_metric(report, "hop"), 1U);
    EXPECT_EQ(report.run.rounds, 60U);
    EXPECT_FALSE(report.machine.kernel.empty());
}

TEST(TickToTrade, SummariseRoundRefusesAHoleAndAnInvertedPair) {
    const RoundConfig config{.rounds = 3, .warmup = 0};
    RunState state{config};
    for (std::size_t round = 0; round < 3; ++round) {
        state.stamps.start[round] = 1000 * (round + 1);
        state.stamps.stop[round]  = 1000 * (round + 1) + 100;
    }
    const aloe::core::TscScale& scale = aloe::core::TscClock::scale();
    const auto rows                   = summarise_round(state, scale, config);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].summary.count, 3U);
    state.stamps.stop[1]   = 0;
    const std::string hole = refusal(state, config);
    EXPECT_NE(hole.find("round 1"), std::string::npos) << "a hole is a failure: " << hole;
    state.stamps.stop[1]       = state.stamps.start[1] - 1;
    const std::string inverted = refusal(state, config);
    EXPECT_NE(inverted.find("round 1"), std::string::npos) << "an inverted pair is a failure: " << inverted;
    EXPECT_NE(inverted.find("TSC"), std::string::npos) << "the peer stamps the tick round's start: " << inverted;
    RoundConfig gateway = config;
    gateway.round       = Round::Gateway;
    EXPECT_EQ(refusal(state, gateway).find("TSC"), std::string::npos) << "the gateway's stamps share the stack's core";
}

TEST(TickToTrade, SummariseRoundChecksEveryBreakdownLeg) {
    const RoundConfig config{.rounds = 3, .warmup = 0, .breakdown = true};  // the tick round, cross-core
    RunState state{config};
    const auto at = [&state](const Phase phase) -> std::vector<std::uint64_t>& {
        return state.stamps.phases[static_cast<std::size_t>(phase)];
    };
    for (std::size_t round = 0; round < 3; ++round) {
        std::uint64_t stamp       = 1000 * (round + 1);
        state.stamps.start[round] = stamp;
        for (std::vector<std::uint64_t>& phase : state.stamps.phases) {
            stamp        += 10;
            phase[round]  = stamp;
        }
        state.stamps.stop[round] = stamp + 10;
    }
    const auto rows = summarise_round(state, aloe::core::TscClock::scale(), config);
    EXPECT_EQ(rows.size(), phase_count + 2) << "the round, a leg per phase, the queue flush";

    at(Phase::Ip)[1]          = at(Phase::Received)[1] - 1;
    const std::string between = refusal(state, config);
    EXPECT_NE(between.find("round 1: the ip leg"), std::string::npos) << between;
    EXPECT_EQ(between.find("TSC"), std::string::npos) << "both ends are the stack's: " << between;
    at(Phase::Ip)[1] = at(Phase::Received)[1] + 1;

    at(Phase::Received)[2]  = state.stamps.start[2] - 1;
    const std::string first = refusal(state, config);
    EXPECT_NE(first.find("round 2: the receive leg"), std::string::npos) << first;
    EXPECT_NE(first.find("TSC"), std::string::npos) << "the peer's start against the stack's receive: " << first;
    at(Phase::Received)[2] = state.stamps.start[2] + 1;

    state.stamps.stop[0]   = at(Phase::TcpFlush)[0] - 1;  // still after the start
    const std::string last = refusal(state, config);
    EXPECT_NE(last.find("round 0: the queue-flush leg"), std::string::npos) << last;
    state.stamps.stop[0] = at(Phase::TcpFlush)[0] + 1;

    at(Phase::Tcp)[1]      = 0;
    const std::string hole = refusal(state, config);
    EXPECT_NE(hole.find("round 1 has no tcp stamp"), std::string::npos) << hole;
}
