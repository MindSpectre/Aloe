#include "tsc_clock.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#if defined(__x86_64__)
    #include <cpuid.h>

#endif

namespace aloe::core {

    namespace {

        bool cpu_has_invariant_tsc() noexcept {
#if defined(__x86_64__)
            unsigned eax = 0;
            unsigned ebx = 0;
            unsigned ecx = 0;
            unsigned edx = 0;
            if (__get_cpuid(0x8000'0007U, &eax, &ebx, &ecx, &edx) == 0) {
                return false;  // the leaf does not exist
            }
            return (edx & (1U << 8U)) != 0;
#else
            return false;
#endif
        }

        std::string kernel_clocksource() {
            std::ifstream file{"/sys/devices/system/clocksource/clocksource0/current_clocksource"};
            std::string name;
            std::getline(file, name);
            return name;
        }

        std::uint64_t monotonic_raw_nanoseconds() {
            timespec stamp{};
            if (::clock_gettime(CLOCK_MONOTONIC_RAW, &stamp) != 0) {
                throw std::system_error{errno, std::generic_category(), "clock_gettime(CLOCK_MONOTONIC_RAW)"};
            }
            return (static_cast<std::uint64_t>(stamp.tv_sec) * std::uint64_t{1'000'000'000}) +
                   static_cast<std::uint64_t>(stamp.tv_nsec);
        }

        constexpr int pairing_attempts = 5;  // as Linux's tsc_read_refs

    }  // namespace

    TscEvidence TscClock::probe() {
        TscEvidence evidence{.invariant_tsc = cpu_has_invariant_tsc(), .clocksource = kernel_clocksource()};
        const detail::TscPair first = detail::closest_pair(&get_ticks, &monotonic_raw_nanoseconds, pairing_attempts);
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        const detail::TscPair last = detail::closest_pair(&get_ticks, &monotonic_raw_nanoseconds, pairing_attempts);
        evidence.ticks             = last.ticks - first.ticks;
        evidence.nanoseconds       = last.nanoseconds - first.nanoseconds;
        return evidence;
    }

    TscScale TscClock::trust(const TscEvidence& evidence) {
        if (!evidence.invariant_tsc) {
            throw std::runtime_error{
                "core::Clock reads the TSC, but this CPU's TSC is not invariant; build with -DUSE_TSC_CLOCK=OFF"};
        }
        if (evidence.clocksource != "tsc") {
            throw std::runtime_error{std::format(
                "core::Clock reads the TSC, but the kernel's clocksource is \"{}\", not \"tsc\"; build with "
                "-DUSE_TSC_CLOCK=OFF",
                evidence.clocksource)};
        }
        return TscScale::measured(evidence.ticks, evidence.nanoseconds);
    }

}  // namespace aloe::core
