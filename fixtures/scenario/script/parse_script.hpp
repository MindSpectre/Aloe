#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

#include <script.hpp>

namespace aloe::scenario {

    /// Where a script failed to parse: 1-based line and column, and what was wrong.
    struct ParseError {
        std::size_t line   = 0;
        std::size_t column = 0;
        std::string message{};
    };

    /// Parses a script's text into its events, or reports the first line that does not parse.
    [[nodiscard]] std::expected<Script, ParseError> parse_script(std::string_view text);

}  // namespace aloe::scenario
