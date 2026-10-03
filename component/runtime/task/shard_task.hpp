#pragma once

#include <aloe/core>

#include <scheduler.hpp>

namespace aloe::runtime {

    /**
     * @brief The environment of a shard task: the concrete Scheduler, an in-place stop source,
     * `std::exception_ptr` errors, and the default allocator until the connection arena arrives.
     *
     * Because the scheduler type is concrete and its senders complete on the shard, a task with
     * this environment holds one pointer and never reschedules around a shard sender.
     */
    using ShardEnvironment = core::TaskEnvironment<Scheduler, core::ex::inplace_stop_source>;

    /// A coroutine task bound to a shard. Start it with `Scheduler::spawn`, or await it from another shard task.
    template <typename T>
    using task = core::task<T, ShardEnvironment>;

}  // namespace aloe::runtime
