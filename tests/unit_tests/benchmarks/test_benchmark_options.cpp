#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <benchmark_options.hpp>
#include <gtest/gtest.h>

namespace {

    using namespace aloe::benchmarks;

    /// Parses `words` as argv, with a program name in front.
    [[nodiscard]] std::expected<Options, std::string> parse(std::vector<std::string> words) {
        words.insert(words.begin(), "tick_to_trade");
        std::vector<char*> argv;
        argv.reserve(words.size());
        for (std::string& word : words) {
            argv.push_back(word.data());
        }
        return parse_options(argv);
    }

    [[nodiscard]] std::size_t
    count(const std::vector<RoundConfig>& matrix, const Round round, const Level level, const Topology topology) {
        return static_cast<std::size_t>(std::ranges::count_if(matrix, [&](const RoundConfig& c) {
            return c.round == round && c.level == level && c.topology == topology;
        }));
    }

}  // namespace

TEST(BenchmarkOptions, DefaultsAreTheSpecsNumbers) {
    const auto options = parse({});
    ASSERT_TRUE(options.has_value()) << options.error();
    EXPECT_EQ(options->payloads, (std::vector<std::size_t>{64, 256, 1024, 1448, 1460}));
    EXPECT_EQ(options->measured, 100000U);
    EXPECT_EQ(options->warmup, 10000U);
    EXPECT_EQ(options->gap.count(), 0);
    EXPECT_EQ(options->topology, TopologyChoice::Cross);
    EXPECT_EQ(options->stack_core, 2U);
    EXPECT_EQ(options->peer_core, 3U);
    EXPECT_EQ(options->hugepages, 64U);
    EXPECT_FALSE(options->breakdown);
    EXPECT_FALSE(options->hop);
    EXPECT_FALSE(options->no_huge);
    EXPECT_EQ(options->eal,
              (std::vector<std::string>{"tick_to_trade", "--in-memory", "--no-pci", "--no-telemetry", "-l", "2"}));
}

TEST(BenchmarkOptions, EveryOptionParses) {
    const auto options =
        parse({"--round",     "tick",   "--level",       "0",           "--payload",   "64,1460",
               "--rounds",    "10",     "--warmup",      "2",           "--gap",       "500",
               "--topology",  "cross",  "--stack-core",  "none",        "--peer-core", "5",
               "--breakdown", "--hop",  "--arp-on-wire", "--hugepages", "8",           "--keep-hugepages",
               "--out",       "r.json", "--samples",     "raw",         "--",          "--no-huge",
               "--no-pci"});
    ASSERT_TRUE(options.has_value()) << options.error();
    EXPECT_EQ(options->rounds, (std::vector<Round>{Round::Tick}));
    EXPECT_EQ(options->levels, (std::vector<Level>{Level::Bricks}));
    EXPECT_EQ(options->payloads, (std::vector<std::size_t>{64, 1460}));
    EXPECT_EQ(options->measured, 10U);
    EXPECT_EQ(options->warmup, 2U);
    EXPECT_EQ(options->gap, std::chrono::nanoseconds{500});
    EXPECT_EQ(options->topology, TopologyChoice::Cross);
    EXPECT_EQ(options->stack_core, std::nullopt);
    EXPECT_EQ(options->peer_core, 5U);
    EXPECT_TRUE(options->breakdown);
    EXPECT_TRUE(options->hop);
    EXPECT_TRUE(options->arp_on_wire);
    EXPECT_EQ(options->hugepages, 8U);
    EXPECT_TRUE(options->keep_hugepages);
    EXPECT_EQ(options->out, "r.json");
    EXPECT_EQ(options->samples, "raw");
    EXPECT_TRUE(options->no_huge) << "detected among the EAL arguments";
    EXPECT_EQ(options->eal, (std::vector<std::string>{"tick_to_trade", "--no-huge", "--no-pci"}));
}

TEST(BenchmarkOptions, RejectsWhatTheSpecRulesOut) {
    EXPECT_FALSE(parse({"--payload", "4"}).has_value()) << "below the round prefix";
    EXPECT_FALSE(parse({"--payload", "1461"}).has_value()) << "above a full segment";
    EXPECT_FALSE(parse({"--rounds", "0"}).has_value());
    EXPECT_FALSE(parse({"--level", "2"}).has_value());
    EXPECT_FALSE(parse({"--topology", "sideways"}).has_value());
    EXPECT_FALSE(parse({"--bogus"}).has_value());
    EXPECT_FALSE(parse({"--rounds"}).has_value()) << "a value is missing";
}

TEST(BenchmarkOptions, NamesAnUnknownOption) {
    const auto options = parse({"--bogus"});
    ASSERT_FALSE(options.has_value());
    EXPECT_EQ(options.error(), "--bogus: unknown option");
}

TEST(BenchmarkOptions, RejectsAnOptionWhereAValueBelongs) {
    const auto out = parse({"--out", "--hop"});
    ASSERT_FALSE(out.has_value()) << "--hop is a forgotten value, not a file name";
    EXPECT_TRUE(out.error().starts_with("--out: ")) << out.error();
    const auto rounds = parse({"--rounds", "--warmup", "3"});
    ASSERT_FALSE(rounds.has_value());
    EXPECT_TRUE(rounds.error().starts_with("--rounds: ")) << rounds.error();
    EXPECT_FALSE(parse({"--out", "--"}).has_value()) << "the EAL's separator is no value either";
}

TEST(BenchmarkOptions, RejectsTheStacksCoreForThePeer) {
    const auto clash = parse({"--stack-core", "3"});
    ASSERT_FALSE(clash.has_value()) << "the peer's default core is 3";
    EXPECT_TRUE(clash.error().starts_with("--peer-core: ")) << clash.error();
    EXPECT_FALSE(parse({"--stack-core", "5", "--peer-core", "5"}).has_value());
    EXPECT_TRUE(parse({"--stack-core", "none", "--peer-core", "none"}).has_value()) << "two unpinned threads";
    EXPECT_TRUE(parse({"--stack-core", "3", "--peer-core", "none"}).has_value());
}

TEST(BenchmarkOptions, RejectsAMatrixWithNoConfiguration) {
    const auto empty = parse({"--level", "1", "--topology", "same"});
    ASSERT_FALSE(empty.has_value()) << "level 1 runs cross-core only";
    EXPECT_NE(empty.error().find("no configuration"), std::string::npos) << empty.error();
    EXPECT_TRUE(parse({"--level", "1", "--topology", "both"}).has_value());
}

TEST(BenchmarkOptions, RejectsArpOnTheWireWithTheSameCoreTopology) {
    EXPECT_FALSE(parse({"--arp-on-wire", "--topology", "same"}).has_value());
    EXPECT_FALSE(parse({"--topology", "both", "--arp-on-wire"}).has_value());
    EXPECT_TRUE(parse({"--arp-on-wire", "--topology", "cross"}).has_value());
}

TEST(BenchmarkOptions, DefaultMatrixIsEveryRoundLevelAndPayloadCrossCore) {
    const auto matrix = configurations(*parse({}));
    EXPECT_EQ(matrix.size(), 18U) << "tick: 3 levels x 5 payloads; gateway: 3 levels";
    EXPECT_EQ(count(matrix, Round::Tick, Level::Bricks, Topology::CrossCore), 5U);
    EXPECT_EQ(count(matrix, Round::Gateway, Level::Runtime, Topology::CrossCore), 1U);
    EXPECT_EQ(count(matrix, Round::Tick, Level::Floor, Topology::SameCore), 0U);
    const auto gateway = std::ranges::find_if(matrix, [](const RoundConfig& c) { return c.round == Round::Gateway; });
    ASSERT_NE(gateway, matrix.end());
    EXPECT_EQ(gateway->payload, order_size);
}

TEST(BenchmarkOptions, SameCoreAppliesToTheFloorAndLevelZeroOnly) {
    const auto both = configurations(*parse({"--topology", "both"}));
    EXPECT_EQ(both.size(), 30U) << "tick: (2 levels x 2 topologies + 1) x 5; gateway: 2 x 2 + 1";
    EXPECT_EQ(count(both, Round::Tick, Level::Runtime, Topology::SameCore), 0U);
    EXPECT_EQ(count(both, Round::Tick, Level::Floor, Topology::SameCore), 5U);
    const auto same = configurations(*parse({"--topology", "same"}));
    EXPECT_EQ(same.size(), 12U) << "level 1 cannot share a thread with the peer and is left out";
    EXPECT_EQ(count(same, Round::Gateway, Level::Runtime, Topology::CrossCore), 0U);
}

TEST(BenchmarkOptions, ConfigurationsCarryTheRunNumbers) {
    const auto matrix = configurations(*parse({"--rounds", "7", "--warmup", "3", "--gap", "20", "--breakdown"}));
    for (const RoundConfig& c : matrix) {
        EXPECT_EQ(c.rounds, 7U);
        EXPECT_EQ(c.warmup, 3U);
        EXPECT_EQ(c.gap, std::chrono::nanoseconds{20});
        EXPECT_EQ(c.breakdown, c.level == Level::Bricks) << "phase stamps exist at level 0 only";
    }
}
