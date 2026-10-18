#pragma once

#include <chrono>
#include <tuple>

#include <tsc_clock.hpp>

namespace aloe::core {

#if defined(ALOE_USE_TSC_CLOCK)
    /// The clock every loop reads once per iteration and passes down as a stamp: `TscClock`, the default.
    using Clock = TscClock;
#else
    /// The clock every loop reads once per iteration and passes down as a stamp: `std::chrono::steady_clock`, in a
    /// build with USE_TSC_CLOCK=OFF.
    using Clock = std::chrono::steady_clock;
#endif

    /// A stamp of `Clock`: what `now` is everywhere in the stack.
    using TimePoint = Clock::time_point;

    /// Intervals and timeouts in configs and timer arithmetic.
    using Duration = std::chrono::nanoseconds;

    /// Calibrates `Clock` now, so that setup takes the TSC's 100 ms of calibration and any refusal, as an
    /// exception, rather than the first `Clock::now()`. Does nothing for the steady clock; a second call is free.
    inline void calibrate_clock() {
#if defined(ALOE_USE_TSC_CLOCK)
        std::ignore = TscClock::scale();
#endif
    }

}  // namespace aloe::core
