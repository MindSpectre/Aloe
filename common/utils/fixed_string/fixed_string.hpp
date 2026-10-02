#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace aloe::utils {

    /**
     * @brief A string literal usable as a template parameter.
     *
     *     template <FixedString Text> void greet();
     *     greet<"hello">();
     *
     * A structural type: every member is public and the array is compared by value, so two
     * instantiations with the same text are the same specialisation. The conversion from a literal
     * is implicit on purpose, so a call site writes the text and nothing else.
     */
    template <std::size_t N>
    struct FixedString {
        char value[N]{};

        // NOLINTNEXTLINE(google-explicit-constructor): the whole point is `f<"text">()`.
        constexpr explicit(false) FixedString(const char (&text)[N]) noexcept {
            std::copy_n(text, N, value);
        }

        [[nodiscard]] constexpr const char* data() const noexcept {
            return value;
        }

        /// Characters before the terminating zero.
        [[nodiscard]] constexpr std::size_t size() const noexcept {
            return N - 1;
        }

        [[nodiscard]] constexpr std::string_view view() const noexcept {
            return {value, N - 1};
        }
    };

}  // namespace aloe::utils
