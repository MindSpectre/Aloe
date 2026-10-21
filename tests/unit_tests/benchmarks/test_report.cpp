#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <machine_info.hpp>
#include <report.hpp>
#include <unistd.h>

namespace {

    using namespace aloe::benchmarks;

    [[nodiscard]] Report one_row_report() {
        return {
            .machine = {.cpu            = "Example CPU",
                        .cpus           = 12,
                        .smt            = "on",
                        .governor       = "performance",
                        .clocksource    = "tsc",
                        .kernel         = "7.1.4",
                        .hugepages_free = 64},
            .run     = {.command    = "tick_to_trade --rounds 3",
                        .revision   = "89785ad",
                        .build      = "release",
                        .timestamp  = "2026-10-21T10:00:00Z",
                        .tsc_ghz    = 4.7,
                        .rounds     = 3,
                        .warmup     = 1,
                        .gap_ns     = 0,
                        .stack_core = "2",
                        .peer_core  = "3",
                        .no_huge    = false},
            .rows    = {
                        {.round    = "tick",
                 .level    = "level0",
                 .topology = "cross-core",
                 .payload  = 64,
                 .metric   = "round",
                 .summary  = {.count = 3, .min = 900, .p50 = 1000, .p90 = 1100, .p99 = 1200, .p999 = 1200, .max = 1300},
                 .samples  = {}}}
        };
    }

}  // namespace

TEST(Report, JsonIsFlatAndExact) {
    std::ostringstream out;
    write_json(out, one_row_report());
    const std::string expected = R"({
  "schema": 1,
  "machine": {"cpu": "Example CPU", "cpus": 12, "smt": "on", "governor": "performance", "clocksource": "tsc", "kernel": "7.1.4", "hugepages_free": 64},
  "run": {"command": "tick_to_trade --rounds 3", "revision": "89785ad", "build": "release", "timestamp": "2026-10-21T10:00:00Z", "tsc_ghz": 4.700, "rounds": 3, "warmup": 1, "gap_ns": 0, "stack_core": "2", "peer_core": "3", "no_huge": false},
  "rows": [
    {"round": "tick", "level": "level0", "topology": "cross-core", "payload": 64, "metric": "round", "count": 3, "min": 900, "p50": 1000, "p90": 1100, "p99": 1200, "p999": 1200, "max": 1300}
  ]
}
)";
    EXPECT_EQ(out.str(), expected);
}

TEST(Report, JsonEscapesQuotesAndBackslashes) {
    EXPECT_EQ(json_escape(R"(a "quoted" \ path)"), R"(a \"quoted\" \\ path)");
    EXPECT_EQ(json_escape("line\nbreak"), R"(line\nbreak)");
}

TEST(Report, TableNamesTheBlockAndTheColumns) {
    std::ostringstream out;
    print_table(out, one_row_report());
    const std::string text = out.str();
    EXPECT_NE(text.find("tick level0 cross-core"), std::string::npos) << text;
    EXPECT_NE(text.find("p99.9"), std::string::npos) << text;
    EXPECT_NE(text.find("1300"), std::string::npos) << text;
    EXPECT_NE(text.find("Example CPU"), std::string::npos) << text;
}

TEST(Report, SamplesGoOneFilePerRowInRoundOrder) {
    Report report          = one_row_report();
    report.rows[0].samples = {1000, 900, 1300};
    report.rows.push_back({
        .round    = "hop",
        .level    = "-",
        .topology = "cross-core",
        .payload  = 0,
        .metric   = "hop",
        .summary  = {.count = 1, .min = 39, .p50 = 39, .p90 = 39, .p99 = 39, .p999 = 39, .max = 39},
        .samples  = {39}
    });
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / std::format("aloe-samples-test-{}", getpid());
    std::filesystem::remove_all(root);
    write_samples(root / "nested", report);  // creates the directories
    const std::filesystem::path tick = root / "nested" / "tick-level0-cross-core-64-round.txt";
    std::ifstream in{tick};
    const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    EXPECT_EQ(text,
              "# tick level0 cross-core payload 64 round: nanoseconds per measured round, in order\n1000\n900\n1300\n");
    EXPECT_TRUE(std::filesystem::exists(root / "nested" / "hop-cross-core-0-hop.txt")) << "the hop has no level";
    EXPECT_THROW(write_samples(tick, report), std::exception) << "a file where the directory should be";
    std::filesystem::remove_all(root);
}

TEST(MachineInfo, ProbeFillsWhatEveryLinuxHas) {
    const MachineInfo machine = probe_machine();
    EXPECT_GT(machine.cpus, 0U);
    EXPECT_NE(machine.kernel, "unknown");
    EXPECT_FALSE(machine.clocksource.empty());
}

TEST(MachineInfo, CpuModelComesFromTheFirstModelNameLine) {
    const std::string cpuinfo = "processor\t: 0\nvendor_id\t: AuthenticAMD\nmodel name\t: AMD Ryzen 5 7600X 6-Core "
                                "Processor\nprocessor\t: 1\nmodel name\t: AMD Ryzen 5 7600X 6-Core Processor\n";
    EXPECT_EQ(cpu_model_from_cpuinfo(cpuinfo), "AMD Ryzen 5 7600X 6-Core Processor");
    EXPECT_EQ(cpu_model_from_cpuinfo("nothing here"), "unknown");
}
