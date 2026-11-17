#pragma once

#include <cstdint>

namespace aloe::loop {

    /// What one shard's device queue counted, all monotonic. Read on the shard's thread, or after it has stopped.
    struct ShardQueueCounters {
        std::uint64_t frames_received    = 0;
        std::uint64_t frames_transmitted = 0;  ///< Accepted by the device.
        std::uint64_t transmit_refused   = 0;  ///< `transmit` returned false, or a frame was dropped at drain.

        friend constexpr bool operator==(const ShardQueueCounters&, const ShardQueueCounters&) noexcept = default;
    };

}  // namespace aloe::loop
