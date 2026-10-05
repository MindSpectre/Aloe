#pragma once

#include <chrono>

namespace aloe::core {

    /// The clock every loop reads once per iteration and passes down as a stamp.
    using Clock = std::chrono::steady_clock;

    /// A stamp of `Clock`: what `now` is everywhere in the stack.
    using TimePoint = Clock::time_point;

    /// Intervals and timeouts in configs and timer arithmetic.
    using Duration = std::chrono::nanoseconds;

}  // namespace aloe::core
