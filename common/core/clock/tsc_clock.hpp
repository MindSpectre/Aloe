#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <ratio>
#include <string>

#include <tsc_scale.hpp>

#if defined(__x86_64__)
    #include <x86intrin.h>
#endif

namespace aloe::core {

    namespace detail {

        /// A reference clock's stamp and the counter reading taken at the same moment.
        struct TscPair {
            std::uint64_t ticks       = 0;
            std::uint64_t nanoseconds = 0;
        };

        /// Reads the reference between two counter reads, `attempts` times, and pairs it with the middle of the
        /// tightest bracket: a preemption inside one attempt cannot skew the pair. Linux's `tsc_read_refs`
        /// brackets its reference reads the same way, over five attempts.
        template <typename ReadTicks, typename ReadNanoseconds>
        [[nodiscard]] TscPair closest_pair(ReadTicks read_ticks, ReadNanoseconds read_nanoseconds, const int attempts) {
            TscPair closest{};
            std::uint64_t tightest = std::numeric_limits<std::uint64_t>::max();
            for (int attempt = 0; attempt < attempts; ++attempt) {
                const std::uint64_t before      = read_ticks();
                const std::uint64_t nanoseconds = read_nanoseconds();
                const std::uint64_t after       = read_ticks();

                if (after - before < tightest) {
                    tightest = after - before;
                    closest  = {.ticks = before + ((after - before) / 2), .nanoseconds = nanoseconds};
                }
            }
            return closest;
        }

    }  // namespace detail

    /// What `TscClock::probe` finds on this machine, and what `TscClock::trust` judges.
    struct TscEvidence {
        bool invariant_tsc        = false;  ///< CPUID 0x80000007 EDX bit 8: one rate in every power state.
        std::string clocksource   = {};     ///< The kernel's current clocksource: "tsc" when Linux trusts the TSC.
        std::uint64_t ticks       = 0;      ///< TSC ticks across the measurement.
        std::uint64_t nanoseconds = 0;      ///< `CLOCK_MONOTONIC_RAW` nanoseconds across the same interval.
    };

    /**
     * @brief A steady clock that reads the CPU's time-stamp counter and converts it to nanoseconds.
     *
     * `now` costs a fenced `rdtsc`, a multiply and a shift, with no system call and no shared page to read. The
     * fence orders the read after every earlier load, as Linux's vDSO orders it, so a stamp taken after another
     * thread's stamp was seen is never earlier than it. The stamps count from the counter's zero, not from
     * `steady_clock`'s epoch: the two clocks' stamps do not mix.
     *
     * The scale is calibrated once per process, on the first `scale` or `now`, by `trust(probe())`: about 100 ms.
     * It refuses a machine whose TSC cannot be trusted, by throwing from `scale`; from `now`, which cannot throw,
     * the refusal terminates the program. Call `scale` (or `core::calibrate_clock`) during setup to take the
     * 100 ms and any refusal there.
     */
    class TscClock {
    public:
        using rep        = std::int64_t;
        using period     = std::nano;
        using duration   = std::chrono::nanoseconds;
        using time_point = std::chrono::time_point<TscClock>;

        static constexpr bool is_steady = true;

        /// The current stamp.
        [[nodiscard]] static time_point now() noexcept {
            return time_point{duration{static_cast<rep>(scale().nanoseconds(get_ticks()))}};
        }

        /// The raw counter, read after every earlier load has completed.
        [[nodiscard]] static std::uint64_t get_ticks() noexcept {
#if defined(__x86_64__)
            _mm_lfence();
            return __rdtsc();
#else
            return 0;  // no TSC: a probe measures no ticks and `trust` refuses
#endif
        }

        /// The process's scale, calibrated by the first call. Throws `std::runtime_error` when this machine's TSC
        /// cannot be trusted, and `std::invalid_argument` when its measurement cannot be scaled.
        [[nodiscard]] static const TscScale& scale() {
            static const TscScale calibrated = trust(probe());
            return calibrated;
        }

        /// Asks the CPU whether its TSC is invariant and the kernel which clocksource it uses, and counts the ticks
        /// across 100 ms of `CLOCK_MONOTONIC_RAW`, as DPDK calibrates a TSC whose frequency the CPU does not report;
        /// each end of the interval is a `detail::closest_pair` of five attempts. Throws `std::system_error` when
        /// `CLOCK_MONOTONIC_RAW` cannot be read.
        [[nodiscard]] static TscEvidence probe();

        /// The scale `evidence` measured, if the TSC is invariant and the kernel's clocksource is "tsc": the kernel
        /// checks that every core's counter agrees and stays stable, and leaves the TSC when it does not. Throws
        /// `std::runtime_error` otherwise, naming the build option that selects the steady clock instead.
        [[nodiscard]] static TscScale trust(const TscEvidence& evidence);
    };

}  // namespace aloe::core
