#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <tsc_scale.hpp>

namespace aloe::benchmarks {

    /// One metric's percentiles in nanoseconds, read from the sorted samples by the nearest-rank rule.
    struct Summary {
        std::uint64_t count = 0;
        std::uint64_t min   = 0;
        std::uint64_t p50   = 0;
        std::uint64_t p90   = 0;
        std::uint64_t p99   = 0;
        std::uint64_t p999  = 0;
        std::uint64_t max   = 0;

        friend constexpr bool operator==(const Summary&, const Summary&) noexcept = default;
    };

    /**
     * @brief The raw TSC deltas of one metric, one per round.
     *
     * `record` is the only member a measured path calls and never allocates after construction; the conversion to
     * nanoseconds and the percentiles come after the run.
     */
    class Distribution {
    public:
        explicit Distribution(const std::size_t capacity) {
            ticks_.reserve(capacity);
        }

        /// Appends one sample. The capacity given at construction is never exceeded; asserted.
        void record(const std::uint64_t ticks) noexcept {
            assert(ticks_.size() < ticks_.capacity() && "more samples than the distribution was sized for");
            ticks_.push_back(ticks);
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return ticks_.size();
        }

        [[nodiscard]] std::span<const std::uint64_t> ticks() const noexcept {
            return ticks_;
        }

        /// The samples after the first `skip`, in nanoseconds by `scale`, summarised. Throws std::invalid_argument
        /// when no sample is left.
        [[nodiscard]] Summary summarise(const core::TscScale& scale, const std::size_t skip) const {
            if (ticks_.size() <= skip) {
                throw std::invalid_argument{"no samples left after the warm-up"};
            }
            std::vector<std::uint64_t> nanoseconds;
            nanoseconds.reserve(ticks_.size() - skip);
            for (const std::uint64_t ticks : std::span{ticks_}.subspan(skip)) {
                nanoseconds.push_back(scale.nanoseconds(ticks));
            }
            return summarise_nanoseconds(std::move(nanoseconds));
        }

        /// Sorts `samples` and reads percentile p as the element of 1-based rank ceil(p * count): the nearest-rank
        /// rule, which never interpolates and so never reports a latency nobody saw. Throws std::invalid_argument
        /// for no samples.
        [[nodiscard]] static Summary summarise_nanoseconds(std::vector<std::uint64_t> samples) {
            if (samples.empty()) {
                throw std::invalid_argument{"a summary needs at least one sample"};
            }
            std::ranges::sort(samples);
            const auto at_permille = [&samples](const std::uint64_t permille) {
                const std::uint64_t rank = (samples.size() * permille + 999) / 1000;
                return samples[static_cast<std::size_t>(std::max<std::uint64_t>(rank, 1) - 1)];
            };
            return Summary{.count = samples.size(),
                           .min   = samples.front(),
                           .p50   = at_permille(500),
                           .p90   = at_permille(900),
                           .p99   = at_permille(990),
                           .p999  = at_permille(999),
                           .max   = samples.back()};
        }

    private:
        std::vector<std::uint64_t> ticks_;
    };

}  // namespace aloe::benchmarks
