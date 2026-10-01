#pragma once

#include <cstddef>
#include <type_traits>

namespace aloe::utils {

    /**
     * @brief Pins a member function as mutating: `force_non_const(this)` does not compile inside a
     * const member function.
     *
     * A const member function may still write through a pointer member, so the compiler never
     * objects when such a function is declared const by mistake; this does. It costs nothing: an
     * empty function with a static assertion.
     */
    template <typename T>
    constexpr void force_non_const(T* /*self*/) noexcept {
        static_assert(!std::is_const_v<T>, "this member function mutates what it points to and must not be const");
    }

    /**
     * @brief Pins a member function as an instance function: `force_non_static(this)` does not
     * compile inside a static member function, where there is no `this`, nor with a bare `nullptr`.
     */
    template <typename T>
    constexpr void force_non_static(const T* /*self*/) noexcept {
        static_assert(!std::is_same_v<T, std::nullptr_t>, "pass this, not nullptr");
    }

}  // namespace aloe::utils
