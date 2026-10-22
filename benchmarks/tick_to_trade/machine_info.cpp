#include "machine_info.hpp"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include <hugepages.hpp>
#include <sys/utsname.h>

namespace aloe::benchmarks {

    namespace {

        [[nodiscard]] std::string read_first_line(const std::filesystem::path& path) {
            std::ifstream file(path);
            std::string line;
            if (!file || !std::getline(file, line)) {
                return {};
            }
            return line;
        }

        void keep_when_present(std::string& field, const std::filesystem::path& path) {
            if (std::string line = read_first_line(path); !line.empty()) {
                field = std::move(line);
            }
        }

    }  // namespace

    std::string cpu_model_from_cpuinfo(const std::string_view cpuinfo) {
        std::size_t begin = 0;
        while (begin < cpuinfo.size()) {
            std::size_t end = cpuinfo.find('\n', begin);
            if (end == std::string_view::npos) {
                end = cpuinfo.size();
            }
            const std::string_view line = cpuinfo.substr(begin, end - begin);
            begin                       = end + 1;
            if (!line.starts_with("model name")) {
                continue;
            }
            const std::size_t colon = line.find(':');
            if (colon == std::string_view::npos) {
                continue;
            }
            std::string_view value  = line.substr(colon + 1);
            const std::size_t first = value.find_first_not_of(" \t");
            if (first == std::string_view::npos) {
                return "unknown";
            }
            value.remove_prefix(first);
            value.remove_suffix(value.size() - 1 - value.find_last_not_of(" \t"));
            return std::string(value);
        }
        return "unknown";
    }

    MachineInfo probe_machine() {
        MachineInfo machine;
        {
            std::ifstream file("/proc/cpuinfo");
            const std::string cpuinfo{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            machine.cpu = cpu_model_from_cpuinfo(cpuinfo);
        }
        machine.cpus = std::thread::hardware_concurrency();
        keep_when_present(machine.smt, "/sys/devices/system/cpu/smt/control");
        keep_when_present(machine.governor, "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
        keep_when_present(machine.clocksource, "/sys/devices/system/clocksource/clocksource0/current_clocksource");
        if (utsname names{}; uname(&names) == 0) {
            machine.kernel = names.release;
        }
        const std::string free_pages = read_first_line(hugepage_root / "free_hugepages");
        std::uint64_t value          = 0;
        const char* const last       = free_pages.data() + free_pages.size();
        if (const auto result = std::from_chars(free_pages.data(), last, value); result.ec == std::errc{}) {
            machine.hugepages_free = value;
        }
        return machine;
    }

}  // namespace aloe::benchmarks
