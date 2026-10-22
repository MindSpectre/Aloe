#pragma once

#include <cstddef>
#include <optional>

#include <distribution.hpp>

namespace aloe::benchmarks {

    /// The one-way cache-line transfer between the calling thread's core and `other_core` (pinned there when given),
    /// as a distribution over `rounds` ping-pongs: half of each round trip, in ticks.
    [[nodiscard]] Distribution measure_hop(std::optional<unsigned> other_core, std::size_t rounds);

}  // namespace aloe::benchmarks
