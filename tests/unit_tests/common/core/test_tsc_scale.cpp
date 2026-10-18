#include <aloe/core>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <tuple>

#include <gtest/gtest.h>

namespace {

    using aloe::core::TscScale;

    // A scale measured over `measured_ticks` and `measured_nanoseconds`, applied to `ticks`. The expectations are
    // the tick counts worked out by hand: a second, a year (31,536,000 s) or a single tick at the stated rate.
    struct Conversion {
        std::string_view name;
        std::uint64_t measured_ticks       = 0;
        std::uint64_t measured_nanoseconds = 0;
        std::uint64_t ticks                = 0;
        std::uint64_t nanoseconds          = 0;
        std::uint64_t tolerance            = 0;
    };

    // 100 ms of measurement, as calibration takes it. A year at 4.7 GHz is 1.48e17 ticks: a 64-bit product of the
    // ticks and a 32-bit multiplier overflows after about four seconds, so only a split or wide product passes.
    // A 100 MHz TSC needs ten nanoseconds per tick, which a multiplier shifted by 32 bits cannot hold.
    constexpr Conversion conversions[] = {
        {"4.7 GHz, one second",      470'000'000, 100'000'000, 4'700'000'000,           1'000'000'000,          1         },
        {"4.7 GHz, one year",        470'000'000, 100'000'000, 148'219'200'000'000'000, 31'536'000'000'000'000, 31'536'000},
        {"2.5 GHz, one microsecond", 250'000'000, 100'000'000, 2'500,                   1'000,                  1         },
        {"100 MHz, one tick",        10'000'000,  100'000'000, 1,                       10,                     0         },
        {"100 MHz, one year",        10'000'000,  100'000'000, 3'153'600'000'000'000,   31'536'000'000'000'000, 31'536'000},
    };

}  // namespace

TEST(TscScale, ConvertsTicksAtTheMeasuredRate) {
    for (const Conversion& conversion : conversions) {
        const TscScale scale = TscScale::measured(conversion.measured_ticks, conversion.measured_nanoseconds);
        const std::uint64_t nanoseconds = scale.nanoseconds(conversion.ticks);
        const std::uint64_t error       = nanoseconds > conversion.nanoseconds ? nanoseconds - conversion.nanoseconds
                                                                               : conversion.nanoseconds - nanoseconds;
        EXPECT_LE(error, conversion.tolerance) << conversion.name << ": " << nanoseconds << " ns";
    }
}

TEST(TscScale, RefusesAMeasurementItCannotScale) {
    EXPECT_THROW(std::ignore = TscScale::measured(0, 100'000'000), std::invalid_argument);
    EXPECT_THROW(std::ignore = TscScale::measured(470'000'000, 0), std::invalid_argument);
    // One tick in five seconds: more nanoseconds per tick than a 32-bit multiplier holds with no shift at all.
    EXPECT_THROW(std::ignore = TscScale::measured(1, 5'000'000'000), std::invalid_argument);
    // Ten thousand ticks per picosecond: a multiplier that rounds to zero and would stop the clock.
    EXPECT_THROW(std::ignore = TscScale::measured(10'000'000'000'000, 1), std::invalid_argument);
}
