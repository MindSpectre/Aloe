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
    state.stamps.stop[1] = 0;
    EXPECT_THROW(std::ignore = summarise_round(state, scale, config), std::runtime_error);
    state.stamps.stop[1] = state.stamps.start[1] - 1;
    try {
        std::ignore = summarise_round(state, scale, config);
        FAIL() << "an inverted pair is a failure";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string{error.what()}.find("round 1"), std::string::npos) << error.what();
    }
}
