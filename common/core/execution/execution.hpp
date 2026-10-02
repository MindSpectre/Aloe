#pragma once

#include <exec/task.hpp>
#include <stdexec/execution.hpp>

namespace aloe::core {

    /**
     * @brief The single point where Aloe names its execution facilities.
     *
     * Aloe code spells senders, receivers and schedulers as `aloe::core::ex::...` and
     * coroutine tasks as `aloe::core::task<T>`, never as `stdexec::` or `exec::`
     * directly. The facilities come from stdexec today. Once the standard
     * library ships std::execution, only this header changes.
     */
    namespace ex = ::stdexec;

    /// Coroutine task type. Inside one, `co_await` accepts any sender.
    template <typename T>
    using task = ::exec::task<T>;

}  // namespace aloe::core
