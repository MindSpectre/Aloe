#include "benchmark_options.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <string_view>
#include <system_error>
#include <utility>

namespace aloe::benchmarks {

    namespace {

        constexpr std::size_t max_payload = 1460;  ///< A full segment.

        template <typename T>
        [[nodiscard]] std::optional<T> parse_number(const std::string_view text) {
            T value{};
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size()) {
                return std::nullopt;
            }
            return value;
        }

        [[nodiscard]] std::unexpected<std::string> bad(const std::string_view option, const std::string_view why) {
            std::string message{option};
            message += ": ";
            message += why;
            return std::unexpected{std::move(message)};
        }

        [[nodiscard]] std::expected<std::optional<unsigned>, std::string> parse_core(const std::string_view option,
                                                                                     const std::string_view text) {
            if (text == "none") {
                return std::optional<unsigned>{};
            }
            const auto core = parse_number<unsigned>(text);
            if (!core) {
                return bad(option, "expects a core number or `none`");
            }
            return std::optional<unsigned>{*core};
        }

        [[nodiscard]] std::expected<std::vector<std::size_t>, std::string> parse_payloads(const std::string_view text) {
            std::vector<std::size_t> payloads;
            std::string_view rest = text;
            while (true) {
                const std::size_t comma = rest.find(',');
                const auto payload      = parse_number<std::size_t>(rest.substr(0, comma));
                if (!payload || *payload < round_prefix || *payload > max_payload) {
                    return bad("--payload", "expects byte counts from 8 to 1460, comma separated");
                }
                payloads.push_back(*payload);
                if (comma == std::string_view::npos) {
                    return payloads;
                }
                rest.remove_prefix(comma + 1);
            }
        }

        [[nodiscard]] std::vector<Topology> topologies(const TopologyChoice choice) {
            switch (choice) {
                case TopologyChoice::Cross:
                    return {Topology::CrossCore};
                case TopologyChoice::Same:
                    return {Topology::SameCore};
                case TopologyChoice::Both:
                    return {Topology::CrossCore, Topology::SameCore};
            }
            return {};
        }

    }  // namespace

    std::string usage() {
        return "usage: tick_to_trade [options] [-- <EAL arguments>]\n"
               "  --round tick|gateway|both     which critical path (both)\n"
               "  --level floor|0|1|all         the floor, the loop over the bricks, the runtime (all)\n"
               "  --payload <bytes,...>         tick payloads, 8..1460 (64,256,1024,1448,1460)\n"
               "  --rounds N                    measured rounds per configuration (100000)\n"
               "  --warmup N                    rounds discarded before them (10000)\n"
               "  --gap NS                      pause between rounds, nanoseconds (0)\n"
               "  --topology cross|same|both    the peer on its own core, or on the stack's thread for the floor "
               "and level 0 (cross)\n"
               "  --stack-core N|none           the stack's core, also the EAL's main lcore (2)\n"
               "  --peer-core N|none            the peer's core, not the stack's (3)\n"
               "  --breakdown                   phase stamps at level 0\n"
               "  --hop                         measure the cache-line hop between the two cores\n"
               "  --arp-on-wire                 resolve the peer by ARP instead of seeding the cache\n"
               "  --hugepages N                 2 MB pages the run needs free (64)\n"
               "  --keep-hugepages              leave pages this process reserved for the next run\n"
               "  --out FILE                    the result file (tick-to-trade-<timestamp>.json)\n"
               "  --                            what follows goes to the EAL verbatim; default: --in-memory "
               "--no-pci --no-telemetry -l <stack core>\n";
    }

    std::vector<std::string> default_eal_arguments(const std::optional<unsigned> stack_core) {
        return {"--in-memory", "--no-pci", "--no-telemetry", "-l", std::to_string(stack_core.value_or(0))};
    }

    std::expected<Options, std::string> parse_options(const std::span<char*> argv) {
        Options options;
        const std::string program = argv.empty() ? "tick_to_trade" : argv.front();
        std::vector<std::string> eal;
        bool eal_given = false;

        const std::span<char*> words = argv.empty() ? argv : argv.subspan(1);
        for (std::size_t i = 0; i < words.size(); ++i) {
            const std::string_view word{words[i]};
            if (word == "--") {
                eal_given = true;
                for (char* argument : words.subspan(i + 1)) {
                    eal.emplace_back(argument);
                }
                break;
            }
            if (word == "--help") {
                return std::unexpected{usage()};
            }
            if (word == "--breakdown") {
                options.breakdown = true;
                continue;
            }
            if (word == "--hop") {
                options.hop = true;
                continue;
            }
            if (word == "--arp-on-wire") {
                options.arp_on_wire = true;
                continue;
            }
            if (word == "--keep-hugepages") {
                options.keep_hugepages = true;
                continue;
            }
            static constexpr std::array<std::string_view, 11> valued{"--round",
                                                                     "--level",
                                                                     "--payload",
                                                                     "--rounds",
                                                                     "--warmup",
                                                                     "--gap",
                                                                     "--topology",
                                                                     "--stack-core",
                                                                     "--peer-core",
                                                                     "--hugepages",
                                                                     "--out"};
            if (std::ranges::find(valued, word) == valued.end()) {
                return bad(word, "unknown option");
            }
            if (i + 1 >= words.size()) {
                return bad(word, "expects a value");
            }
            const std::string_view value{words[++i]};
            if (value.starts_with("--")) {  // a forgotten value: `--out --hop` would write to a file named `--hop`
                return bad(word, std::format("expects a value, found the option {}", value));
            }
            if (word == "--round") {
                if (value == "tick") {
                    options.rounds = {Round::Tick};
                } else if (value == "gateway") {
                    options.rounds = {Round::Gateway};
                } else if (value == "both") {
                    options.rounds = {Round::Tick, Round::Gateway};
                } else {
                    return bad(word, "expects tick, gateway or both");
                }
            } else if (word == "--level") {
                if (value == "floor") {
                    options.levels = {Level::Floor};
                } else if (value == "0") {
                    options.levels = {Level::Bricks};
                } else if (value == "1") {
                    options.levels = {Level::Runtime};
                } else if (value == "all") {
                    options.levels = {Level::Floor, Level::Bricks, Level::Runtime};
                } else {
                    return bad(word, "expects floor, 0, 1 or all");
                }
            } else if (word == "--payload") {
                auto payloads = parse_payloads(value);
                if (!payloads) {
                    return std::unexpected{std::move(payloads.error())};
                }
                options.payloads = std::move(*payloads);
            } else if (word == "--rounds") {
                const auto count = parse_number<std::size_t>(value);
                if (!count || *count < 1) {
                    return bad(word, "expects an integer of at least 1");
                }
                options.measured = *count;
            } else if (word == "--warmup") {
                const auto count = parse_number<std::size_t>(value);
                if (!count) {
                    return bad(word, "expects an integer");
                }
                options.warmup = *count;
            } else if (word == "--gap") {
                const auto nanoseconds = parse_number<core::Duration::rep>(value);
                if (!nanoseconds || *nanoseconds < 0) {
                    return bad(word, "expects nanoseconds as an integer");
                }
                options.gap = core::Duration{*nanoseconds};
            } else if (word == "--topology") {
                if (value == "cross") {
                    options.topology = TopologyChoice::Cross;
                } else if (value == "same") {
                    options.topology = TopologyChoice::Same;
                } else if (value == "both") {
                    options.topology = TopologyChoice::Both;
                } else {
                    return bad(word, "expects cross, same or both");
                }
            } else if (word == "--stack-core" || word == "--peer-core") {
                const auto core = parse_core(word, value);
                if (!core) {
                    return std::unexpected{core.error()};
                }
                (word == "--stack-core" ? options.stack_core : options.peer_core) = *core;
            } else if (word == "--hugepages") {
                const auto pages = parse_number<std::uint64_t>(value);
                if (!pages) {
                    return bad(word, "expects an integer");
                }
                options.hugepages = *pages;
            } else if (word == "--out") {
                options.out = std::string{value};
            } else {
                return bad(word, "unknown option");  // in `valued` but not handled here
            }
        }

        if (options.arp_on_wire && options.topology != TopologyChoice::Cross) {
            return bad("--arp-on-wire",
                       "cannot be used with --topology same or both: the same-core topology cannot resolve ARP, the "
                       "inline peer cannot answer a request while the stack waits for the handshake");
        }
        if (options.stack_core && options.stack_core == options.peer_core) {
            return bad("--peer-core",
                       "cannot be the stack's core; the stack and the venue each spin on a core of their own");
        }
        if (configurations(options).empty()) {
            return bad("--topology same",
                       "leaves no configuration with --level 1, which runs cross-core only; use --topology cross "
                       "or both");
        }

        if (eal.empty()) {
            eal = default_eal_arguments(options.stack_core);
        }
        options.eal.push_back(program);
        options.eal.insert(options.eal.end(), eal.begin(), eal.end());
        options.no_huge =
            eal_given && std::ranges::any_of(eal, [](const std::string& argument) { return argument == "--no-huge"; });
        return options;
    }

    std::vector<RoundConfig> configurations(const Options& options) {
        std::vector<RoundConfig> matrix;
        for (const Round round : options.rounds) {
            for (const Level level : options.levels) {
                for (const Topology topology : topologies(options.topology)) {
                    if (topology == Topology::SameCore && level == Level::Runtime) {
                        continue;
                    }
                    const RoundConfig base{.round     = round,
                                           .level     = level,
                                           .topology  = topology,
                                           .payload   = order_size,
                                           .rounds    = options.measured,
                                           .warmup    = options.warmup,
                                           .gap       = options.gap,
                                           .breakdown = options.breakdown && level == Level::Bricks};
                    if (round == Round::Gateway) {
                        matrix.push_back(base);
                        continue;
                    }
                    for (const std::size_t payload : options.payloads) {
                        RoundConfig config = base;
                        config.payload     = payload;
                        matrix.push_back(config);
                    }
                }
            }
        }
        return matrix;
    }

}  // namespace aloe::benchmarks
