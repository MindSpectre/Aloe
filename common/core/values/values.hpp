#pragma once

#include <utility>

namespace aloe::core {

    /**
     * @brief Discards values on purpose, where the discarding happens.
     *
     * `[[maybe_unused]]` marks a declaration and `std::ignore = f()` discards one result at a call.
     * This discards any number of values inside the body of a function, which is where a template
     * decides not to use something it was handed. Every argument is evaluated exactly once.
     */
    template <typename... Values>
    constexpr void unused_value(Values&&... values) noexcept {
        (static_cast<void>(values), ...);
    }

    /**
     * @brief GCC's `value ?: fallback` as a function: `value` when it converts to true, else
     * `fallback`.
     *
     * `value` is evaluated once. As with the conditional operator, two lvalues of one type yield
     * that lvalue, so nothing is copied; any other mix yields a value of the operands' common type.
     */
    template <typename T, typename U>
    constexpr decltype(auto) value_or(T&& value, U&& fallback) noexcept(noexcept(value ? std::forward<T>(value)
                                                                                       : std::forward<U>(fallback))) {
        return value ? std::forward<T>(value) : std::forward<U>(fallback);
    }

}  // namespace aloe::core
