#include "report.hpp"

#include <algorithm>
#include <format>
#include <tuple>
#include <utility>

namespace aloe::benchmarks {

    std::string json_escape(const std::string_view text) {
        std::string escaped;
        escaped.reserve(text.size());
        for (const char c : text) {
            switch (c) {
                case '"':
                    escaped += "\\\"";
                    break;
                case '\\':
                    escaped += "\\\\";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                case '\t':
                    escaped += "\\t";
                    break;
                case '\r':
                    escaped += "\\r";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        escaped += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    } else {
                        escaped += c;
                    }
            }
        }
        return escaped;
    }

    void write_json(std::ostream& out, const Report& report) {
        const MachineInfo& m = report.machine;
        const RunInfo& r     = report.run;
        out << "{\n  \"schema\": 1,\n";
        out << std::format("  \"machine\": {{\"cpu\": \"{}\", \"cpus\": {}, \"smt\": \"{}\", \"governor\": \"{}\", "
                           "\"clocksource\": \"{}\", \"kernel\": \"{}\", \"hugepages_free\": {}}},\n",
                           json_escape(m.cpu),
                           m.cpus,
                           json_escape(m.smt),
                           json_escape(m.governor),
                           json_escape(m.clocksource),
                           json_escape(m.kernel),
                           m.hugepages_free);
        out << std::format(
            "  \"run\": {{\"command\": \"{}\", \"revision\": \"{}\", \"build\": \"{}\", \"timestamp\": \"{}\", "
            "\"tsc_ghz\": {:.3f}, \"rounds\": {}, \"warmup\": {}, \"gap_ns\": {}, \"stack_core\": \"{}\", "
            "\"peer_core\": \"{}\", \"no_huge\": {}}},\n",
            json_escape(r.command),
            json_escape(r.revision),
            json_escape(r.build),
            json_escape(r.timestamp),
            r.tsc_ghz,
            r.rounds,
            r.warmup,
            r.gap_ns,
            json_escape(r.stack_core),
            json_escape(r.peer_core),
            r.no_huge ? "true" : "false");
        out << "  \"rows\": [\n";
        for (std::size_t i = 0; i < report.rows.size(); ++i) {
            const Row& row   = report.rows[i];
            const Summary& s = row.summary;
            out << std::format("    {{\"round\": \"{}\", \"level\": \"{}\", \"topology\": \"{}\", \"payload\": {}, "
                               "\"metric\": \"{}\", \"count\": {}, \"min\": {}, \"p50\": {}, \"p90\": {}, \"p99\": {}, "
                               "\"p999\": {}, \"max\": {}}}{}\n",
                               json_escape(row.round),
                               json_escape(row.level),
                               json_escape(row.topology),
                               row.payload,
                               json_escape(row.metric),
                               s.count,
                               s.min,
                               s.p50,
                               s.p90,
                               s.p99,
                               s.p999,
                               s.max,
                               i + 1 < report.rows.size() ? "," : "");
        }
        out << "  ]\n}\n";
    }

    void print_table(std::ostream& out, const Report& report) {
        const MachineInfo& m = report.machine;
        const RunInfo& r     = report.run;
        const std::string hugepages =
            r.no_huge ? std::string("no hugepages") : std::format("hugepages_free {}", m.hugepages_free);
        out << std::format("cpu {}, cpus {}, smt {}, governor {}, clocksource {}, kernel {}, {}\n",
                           m.cpu,
                           m.cpus,
                           m.smt,
                           m.governor,
                           m.clocksource,
                           m.kernel,
                           hugepages);
        out << std::format(
            "rounds {}, warmup {}, gap_ns {}, stack_core {}, peer_core {}, tsc_ghz {:.3f}, revision {}, build {}\n",
            r.rounds,
            r.warmup,
            r.gap_ns,
            r.stack_core,
            r.peer_core,
            r.tsc_ghz,
            r.revision,
            r.build);

        using Block = std::tuple<std::string, std::string, std::string>;
        std::vector<Block> blocks;
        for (const Row& row : report.rows) {
            if (Block block{row.round, row.level, row.topology}; std::ranges::find(blocks, block) == blocks.end()) {
                blocks.push_back(std::move(block));
            }
        }
        for (const auto& [round, level, topology] : blocks) {
            out << std::format("\n== {} {} {} ==\n", round, level, topology);
            out << std::format("{:>8}  {:<14}{:>8}{:>8}{:>8}{:>8}{:>8}{:>8}{:>8}\n",
                               "payload",
                               "metric",
                               "count",
                               "min",
                               "p50",
                               "p90",
                               "p99",
                               "p99.9",
                               "max");
            for (const Row& row : report.rows) {
                if (row.round != round || row.level != level || row.topology != topology) {
                    continue;
                }
                const Summary& s = row.summary;
                out << std::format("{:>8}  {:<14}{:>8}{:>8}{:>8}{:>8}{:>8}{:>8}{:>8}\n",
                                   row.payload,
                                   row.metric,
                                   s.count,
                                   s.min,
                                   s.p50,
                                   s.p90,
                                   s.p99,
                                   s.p999,
                                   s.max);
            }
        }
    }

}  // namespace aloe::benchmarks
