#pragma once

#include <string>
#include <vector>

#include <benchmark_options.hpp>
#include <machine_info.hpp>
#include <report.hpp>
#include <round_config.hpp>
#include <run_state.hpp>
#include <tsc_scale.hpp>

namespace aloe::benchmarks {

    /**
     * The rows of one finished configuration: the "round" distribution of stop minus start over every round, the
     * warm-up skipped, and with breakdown the legs between the phase stamps that are present. Throws
     * std::runtime_error naming the round when a start or stop stamp is missing, or when a stop precedes its start,
     * which across cores means the TSC is not synchronised between them.
     */
    [[nodiscard]] std::vector<Row>
    summarise_round(const RunState& state, const core::TscScale& scale, const RoundConfig& config);

    /**
     * The matrix of `options` over one ring pair and one peer thread, the rings drained between configurations, then
     * the hop when asked. The EAL is running. Throws std::runtime_error with the configuration and the cause on the
     * first configuration that fails its sanity checks; nothing is written then.
     */
    [[nodiscard]] Report run_benchmark(const Options& options, const MachineInfo& machine, std::string command);

}  // namespace aloe::benchmarks
