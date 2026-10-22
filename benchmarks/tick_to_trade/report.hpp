#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <distribution.hpp>
#include <machine_info.hpp>

namespace aloe::benchmarks {

    /// How the run was made: enough to repeat it and to refuse a comparison across machines or builds.
    struct RunInfo {
        std::string command;               ///< argv, space-joined.
        std::string revision = "unknown";  ///< The git revision the binary was configured from.
        std::string build    = "unknown";  ///< The CMake build type.
        std::string timestamp;             ///< UTC, ISO 8601.
        double tsc_ghz         = 0;
        std::size_t rounds     = 0;
        std::size_t warmup     = 0;
        std::uint64_t gap_ns   = 0;
        std::string stack_core = "none";
        std::string peer_core  = "none";
        bool no_huge           = false;
    };

    /// One distribution of the result: a round, a breakdown leg or the hop.
    struct Row {
        std::string round;     ///< "tick", "gateway" or "hop".
        std::string level;     ///< "floor", "level0", "level1" or "-".
        std::string topology;  ///< "cross-core" or "same-core".
        std::size_t payload = 0;
        std::string metric;  ///< "round", a `Phase` name, `last_leg` or "hop".
        Summary summary;
        std::vector<std::uint64_t> samples;  ///< The distribution in nanoseconds, in round order, the warm-up skipped.
    };

    struct Report {
        MachineInfo machine;
        RunInfo run;
        std::vector<Row> rows;
    };

    [[nodiscard]] std::string json_escape(std::string_view text);

    /// The result file: schema 1, flat objects, one row per line, as the test fixes it.
    void write_json(std::ostream& out, const Report& report);

    /// The printed table: the machine and the run, then one block per (round, level, topology) with a line per
    /// payload and metric and the columns count, min, p50, p90, p99, p99.9, max in nanoseconds.
    void print_table(std::ostream& out, const Report& report);

    /// Every row's samples, one file per row in `directory`, created when missing:
    /// `<round>-<level>-<topology>-<payload>-<metric>.txt`, the hop's level left out, a comment line and then one
    /// sample per line in nanoseconds, in round order. Throws std::runtime_error naming what could not be written.
    void write_samples(const std::filesystem::path& directory, const Report& report);

}  // namespace aloe::benchmarks
