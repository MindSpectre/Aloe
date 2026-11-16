#pragma once

#include <aloe/loop>
#include <cstdint>

namespace aloe::runtime {

    /// Per-shard counters of the runtime, all monotonic. Read on the shard's thread, or after the shard has stopped.
    struct ShardCounters {
        loop::ShardQueueCounters queue{};  ///< What the shard's device queue counted.
        std::uint64_t ticks           = 0;
        std::uint64_t idle_ticks      = 0;  ///< Steps in which nothing was received, run, fired or sent.
        std::uint64_t inbox_received  = 0;
        std::uint64_t work_run        = 0;
        std::uint64_t timers_fired    = 0;
        std::uint64_t tasks_spawned   = 0;
        std::uint64_t tasks_completed = 0;
        std::uint64_t tasks_stopped   = 0;
        std::uint64_t tasks_failed    = 0;

        friend constexpr bool operator==(const ShardCounters&, const ShardCounters&) noexcept = default;
    };

}  // namespace aloe::runtime
