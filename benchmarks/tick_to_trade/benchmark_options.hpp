#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <clock.hpp>
#include <round_config.hpp>

namespace aloe::benchmarks {

    enum class TopologyChoice : std::uint8_t {
        Cross,  ///< The default.
        Same,   ///< The floor and level 0 on one thread; level 1 left out.
        Both,   ///< Cross-core everywhere, plus same-core for the floor and level 0.
    };

    /// What the command line decided. Every member has the spec's default.
    struct Options {
        std::vector<Round> rounds{Round::Tick, Round::Gateway};
        std::vector<Level> levels{Level::Floor, Level::Bricks, Level::Runtime};
        std::vector<std::size_t> payloads{64, 256, 1024, 1448, 1460};
        std::size_t measured               = 100000;
        std::size_t warmup                 = 10000;
        core::Duration gap                 = core::Duration::zero();
        TopologyChoice topology            = TopologyChoice::Cross;
        std::optional<unsigned> stack_core = 2;  ///< Also the EAL's main lcore; `none` leaves it unpinned.
        std::optional<unsigned> peer_core  = 3;
        bool breakdown                     = false;
        bool hop                           = false;
        bool arp_on_wire                   = false;
        std::uint64_t hugepages            = 64;  ///< 2 MB pages the run needs free.
        bool keep_hugepages                = false;
        bool no_huge = false;  ///< `--no-huge` was passed after `--`: no reservation, and the result file says so.
        std::string out;       ///< The result file; empty means `tick-to-trade-<timestamp>.json` here.
        std::vector<std::string> eal;  ///< argv for the EAL, the program name first.
    };

    /// `argv[0]` is the program; options come first, then `--` and the EAL's arguments verbatim.
    [[nodiscard]] std::expected<Options, std::string> parse_options(std::span<char*> argv);

    [[nodiscard]] std::string usage();

    /// In-memory, no PCI scan, no telemetry, the main lcore on `stack_core` (0 when unpinned); hugepages by default.
    [[nodiscard]] std::vector<std::string> default_eal_arguments(std::optional<unsigned> stack_core);

    /// The matrix the options describe, in the order the table prints: rounds, then levels, then topologies, then
    /// payloads. The gateway round has one configuration per level and topology, its payload `order_size`; level 1
    /// is cross-core only; breakdown applies to level 0 only.
    [[nodiscard]] std::vector<RoundConfig> configurations(const Options& options);

}  // namespace aloe::benchmarks
