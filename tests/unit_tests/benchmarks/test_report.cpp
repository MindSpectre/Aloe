#include <sstream>
#include <string>

#include <gtest/gtest.h>
#include <machine_info.hpp>
#include <report.hpp>

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
            .rows = {{.round    = "tick",
                      .level    = "level0",
                      .topology = "cross-core",
                      .payload  = 64,
                      .metric   = "round",
                      .summary  = {
                          .count = 3, .min = 900, .p50 = 1000, .p90 = 1100, .p99 = 1200, .p999 = 1200, .max = 1300}}}
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
