// The tick-to-trade benchmark: both rounds, three levels, a scripted venue on a second core, over DPDK's ring
// driver. See docs/benchmarks/tick-to-trade.md for how to run it and how to read the table.

#include <aloe/ethdev>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <print>
#include <span>
#include <string>

#include <benchmark_options.hpp>
#include <benchmark_run.hpp>
#include <clock.hpp>
#include <hugepages.hpp>
#include <machine_info.hpp>
#include <report.hpp>

#ifndef ALOE_BENCHMARK_REVISION
    #define ALOE_BENCHMARK_REVISION "unknown"
#endif
#ifndef ALOE_BENCHMARK_BUILD_TYPE
    #define ALOE_BENCHMARK_BUILD_TYPE "unknown"
#endif

namespace {

    [[nodiscard]] std::string joined(const std::span<char*> argv) {
        std::string command;
        for (const char* argument : argv) {
            command += command.empty() ? "" : " ";
            command += argument;
        }
        return command;
    }

    [[nodiscard]] std::string default_result_name() {
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        return std::format("tick-to-trade-{:%Y%m%d-%H%M%S}.json", now);
    }

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> arguments{argv, static_cast<std::size_t>(argc)};
    const auto options = aloe::benchmarks::parse_options(arguments);
    if (!options) {
        std::println(stderr, "{}", options.error());
        return EXIT_FAILURE;
    }
    try {
        aloe::core::calibrate_clock();
        std::optional<aloe::benchmarks::FileHugepageSysfs> sysfs;
        std::optional<aloe::benchmarks::HugepageReservation> hugepages;
        if (!options->no_huge) {
            sysfs.emplace();
            hugepages.emplace(*sysfs, options->hugepages, options->keep_hugepages);
            if (hugepages->reserved_by_us() > 0) {
                std::println("reserved {} hugepages for this run{}",
                             hugepages->reserved_by_us(),
                             options->keep_hugepages ? ", kept after it" : "");
            }
        }
        aloe::ethdev::Eal eal{options->eal};
        aloe::benchmarks::MachineInfo machine = aloe::benchmarks::probe_machine();
        if (options->no_huge) {
            machine.hugepages_free = 0;
        }
        aloe::benchmarks::Report report = aloe::benchmarks::run_benchmark(*options, machine, joined(arguments));
        report.run.revision             = ALOE_BENCHMARK_REVISION;
        report.run.build                = ALOE_BENCHMARK_BUILD_TYPE;
        aloe::benchmarks::print_table(std::cout, report);
        const std::string path = options->out.empty() ? default_result_name() : options->out;
        std::ofstream file{path};
        if (!file) {
            std::println(stderr, "cannot write {}", path);
            return EXIT_FAILURE;
        }
        aloe::benchmarks::write_json(file, report);
        std::println("results written to {}", path);
        return EXIT_SUCCESS;
    } catch (const aloe::benchmarks::HugepageError& error) {
        std::println(stderr, "hugepages: {}", error.what());
        return EXIT_FAILURE;
    } catch (const std::exception& error) {
        std::println(stderr, "tick_to_trade: {}", error.what());
        return EXIT_FAILURE;
    }
}
