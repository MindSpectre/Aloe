#pragma once

#include <exec/completion_behavior.hpp>
#include <stdexec/execution.hpp>

namespace aloe::execution {

    /**
     * @brief The single point where Aloe names its execution facilities.
     *
     * Aloe code spells senders, receivers and schedulers as `aloe::execution::ex::...` and
     * coroutine tasks as `aloe::execution::task<T>`, never as `stdexec::` or `exec::` directly.
     * The facilities come from stdexec today. Once the standard library ships
     * std::execution, only this header changes.
     */
    namespace ex = ::stdexec;

    /**
     * @brief Coroutine task type, the one P3552 adds to C++26. Inside one, `co_await` accepts any sender.
     *
     * `Env` fixes the task's scheduler type, stop source, error types and allocator. With the
     * default environment the scheduler is type-erased; a runtime binds its concrete scheduler
     * through a TaskEnvironment.
     */
    template <typename T, typename Env = ex::env<>>
    using task = ::stdexec::task<T, Env>;

    /**
     * @brief A task environment naming a concrete scheduler.
     *
     * P3552 calls the member `scheduler_type`; stdexec spells it `start_scheduler_type` today.
     * That spelling lives here and nowhere else, like the namespace alias above.
     */
    template <typename Scheduler, typename StopSource = ex::inplace_stop_source>
    struct TaskEnvironment {
        using start_scheduler_type = Scheduler;
        using stop_source_type     = StopSource;
    };

    /**
     * @brief How a sender completes relative to where it started.
     *
     * A sender whose attributes answer `get_completion_behavior_t<set_value_t>` with
     * `completion_behavior::asynchronous_affine` promises to complete on the scheduler it was
     * started from. A task started on such a scheduler awaits every sender directly, with no
     * reschedule.
     */
    using completion_behavior = ::exec::completion_behavior;

    template <typename Tag>
    using get_completion_behavior_t = ::exec::get_completion_behavior_t<Tag>;

}  // namespace aloe::execution
