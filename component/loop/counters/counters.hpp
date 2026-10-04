#pragma once

#include <cstdint>

namespace aloe::loop {

    /// Per-shard counters, all monotonic. Read on the shard's thread, or after the shard has stopped.
    struct ShardCounters {
        std::uint64_t ticks              = 0;
        std::uint64_t idle_ticks         = 0;  ///< Steps in which nothing was received, run, fired or sent.
        std::uint64_t frames_received    = 0;
        std::uint64_t frames_transmitted = 0;  ///< Accepted by the device.
        std::uint64_t transmit_refused   = 0;  ///< `transmit` returned false, or a frame was dropped at drain.
        std::uint64_t inbox_received     = 0;
        std::uint64_t work_run           = 0;
        std::uint64_t timers_fired       = 0;
        std::uint64_t tasks_spawned      = 0;
        std::uint64_t tasks_completed    = 0;
        std::uint64_t tasks_stopped      = 0;
        std::uint64_t tasks_failed       = 0;

        friend constexpr bool operator==(const ShardCounters&, const ShardCounters&) noexcept = default;
    };

}  // namespace aloe::loop
