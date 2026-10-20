#include <algorithm>
#include <cstdint>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>

#include <distribution.hpp>
#include <gtest/gtest.h>
#include <tsc_scale.hpp>

namespace {

    using aloe::benchmarks::Distribution;
    using aloe::benchmarks::Summary;

    [[nodiscard]] std::vector<std::uint64_t> one_to(const std::uint64_t count) {
        std::vector<std::uint64_t> samples(count);
        std::iota(samples.begin(), samples.end(), std::uint64_t{1});
        std::ranges::shuffle(samples,
                             std::mt19937{42});  // NOLINT(cert-msc51-cpp): a fixed seed keeps the test reproducible
        return samples;
    }

}  // namespace

TEST(Distribution, NearestRankOverOneToOneHundred) {
    const Summary summary = Distribution::summarise_nanoseconds(one_to(100));
    EXPECT_EQ(summary, (Summary{.count = 100, .min = 1, .p50 = 50, .p90 = 90, .p99 = 99, .p999 = 100, .max = 100}));
}

TEST(Distribution, NearestRankOverOneToOneThousand) {
    const Summary summary = Distribution::summarise_nanoseconds(one_to(1000));
    EXPECT_EQ(summary,
              (Summary{.count = 1000, .min = 1, .p50 = 500, .p90 = 900, .p99 = 990, .p999 = 999, .max = 1000}));
}

TEST(Distribution, OneSampleIsEveryPercentile) {
    const Summary summary = Distribution::summarise_nanoseconds({7});
    EXPECT_EQ(summary, (Summary{.count = 1, .min = 7, .p50 = 7, .p90 = 7, .p99 = 7, .p999 = 7, .max = 7}));
}

TEST(Distribution, NoSamplesThrows) {
    EXPECT_THROW(std::ignore = Distribution::summarise_nanoseconds({}), std::invalid_argument);
}

TEST(Distribution, SummariseSkipsTheWarmupAndScalesTicks) {
    const aloe::core::TscScale half = aloe::core::TscScale::measured(2, 1);  // one nanosecond per two ticks
    Distribution distribution{3};
    distribution.record(1000);
    distribution.record(2000);
    distribution.record(4000);
    EXPECT_EQ(distribution.size(), 3U);
    const Summary summary = distribution.summarise(half, 1);
    EXPECT_EQ(summary.count, 2U);
    EXPECT_EQ(summary.min, 1000U);
    EXPECT_EQ(summary.p50, 1000U);
    EXPECT_EQ(summary.max, 2000U);
}

TEST(Distribution, SummariseWithEverythingSkippedThrows) {
    Distribution distribution{2};
    distribution.record(1);
    distribution.record(2);
    EXPECT_THROW(std::ignore = distribution.summarise(aloe::core::TscScale::measured(1, 1), 2), std::invalid_argument);
}
