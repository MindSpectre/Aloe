#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace aloe::benchmarks {

    /// What a reader needs to know before trusting a number: every field says "unknown" when the machine does not
    /// expose it.
    struct MachineInfo {
        std::string cpu         = "unknown";  ///< `/proc/cpuinfo`'s first `model name`.
        unsigned cpus           = 0;          ///< `std::thread::hardware_concurrency()`.
        std::string smt         = "unknown";  ///< `/sys/devices/system/cpu/smt/control`.
        std::string governor    = "unknown";  ///< `/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor`.
        std::string clocksource = "unknown";  ///< `/sys/devices/system/clocksource/clocksource0/current_clocksource`.
        std::string kernel      = "unknown";  ///< `uname -r`.
        std::uint64_t hugepages_free = 0;     ///< Free 2 MB pages when probed; 0 under `--no-huge` or when unreadable.
    };

    [[nodiscard]] MachineInfo probe_machine();

    /// The value of the first `model name` line, or "unknown".
    [[nodiscard]] std::string cpu_model_from_cpuinfo(std::string_view cpuinfo);

}  // namespace aloe::benchmarks
