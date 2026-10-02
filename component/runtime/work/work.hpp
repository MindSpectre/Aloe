#pragma once

#include <atomic>

namespace aloe::runtime {

    /**
     * @brief The intrusive node every unit of ready work is.
     *
     * An operation state derives from it and sets `run`. One node type serves both queues of a
     * shard: the inbox uses acquire and release on `next`, the run queue relaxed operations, which
     * are plain loads and stores on every supported target, so the same-shard path pays no fence.
     * `run` is called once per push, on the owning shard's thread, and may destroy or re-push the
     * node; whoever runs a chain therefore reads `next` before calling it.
     */
    struct Work {
        using Function = void (*)(Work&) noexcept;

        Work() noexcept = default;

        constexpr explicit Work(const Function function) noexcept
            : run{function} {
        }

        Work(const Work&)            = delete;
        Work& operator=(const Work&) = delete;
        Work(Work&&)                 = delete;
        Work& operator=(Work&&)      = delete;
        ~Work()                      = default;

        std::atomic<Work*> next{nullptr};
        Function run = nullptr;
    };

}  // namespace aloe::runtime
