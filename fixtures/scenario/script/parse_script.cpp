#include "parse_script.hpp"

#include <algorithm>
#include <aloe/core>
#include <aloe/wire>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <script.hpp>

namespace aloe::scenario {

    namespace {

        namespace detail {

            /// A read position over one line of a script, past its comment; columns are 1-based.
            class Cursor {
            public:
                explicit Cursor(const std::string_view text) noexcept
                    : text_{text} {
                }

                void skip_spaces() noexcept {
                    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t')) {
                        ++pos_;
                    }
                }

                /// The next character, or NUL at the end.
                [[nodiscard]] char peek() const noexcept {
                    return pos_ < text_.size() ? text_[pos_] : '\0';
                }

                [[nodiscard]] char peek_after_next() const noexcept {
                    return pos_ + 1 < text_.size() ? text_[pos_ + 1] : '\0';
                }

                [[nodiscard]] bool at_end() noexcept {
                    skip_spaces();
                    return pos_ >= text_.size();
                }

                [[nodiscard]] std::size_t column() const noexcept {
                    return pos_ + 1;
                }

                [[nodiscard]] std::string_view rest() const noexcept {
                    return text_.substr(pos_);
                }

                bool consume(const char expected) noexcept {
                    if (peek() == expected && pos_ < text_.size()) {
                        ++pos_;
                        return true;
                    }
                    return false;
                }

                bool consume_text(const std::string_view expected) noexcept {
                    if (rest().starts_with(expected)) {
                        pos_ += expected.size();
                        return true;
                    }
                    return false;
                }

                /// Letters, digits, `_`, `|` and `.`.
                [[nodiscard]] std::string_view take_word() noexcept {
                    return take_while([](const char c) {
                        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                               c == '|' || c == '.';
                    });
                }

                /// An unsigned integer, or a decimal with an optional leading `.`; empty when there is none.
                [[nodiscard]] std::string_view take_number() noexcept {
                    const std::size_t start       = pos_;
                    const std::string_view digits = take_while(is_digit);
                    std::size_t count             = digits.size();
                    if (peek() == '.') {
                        ++pos_;
                        count += take_while(is_digit).size();
                    }
                    if (count == 0) {
                        pos_ = start;
                        return {};
                    }
                    return text_.substr(start, pos_ - start);
                }

                [[nodiscard]] std::string_view take_token() noexcept {
                    return take_while([](const char c) { return c != ' ' && c != '\t'; });
                }

                void move_to(const std::size_t position) noexcept {
                    pos_ = position;
                }

                [[nodiscard]] std::size_t position() const noexcept {
                    return pos_;
                }

                [[nodiscard]] static bool is_digit(const char c) noexcept {
                    return c >= '0' && c <= '9';
                }

            private:
                template <typename Predicate>
                std::string_view take_while(const Predicate keep) noexcept {
                    const std::size_t start = pos_;
                    while (pos_ < text_.size() && keep(text_[pos_])) {
                        ++pos_;
                    }
                    return text_.substr(start, pos_ - start);
                }

                std::string_view text_;
                std::size_t pos_ = 0;
            };

            using Failure = std::unexpected<ParseError>;

            [[nodiscard]] Failure fail(const std::size_t column, std::string message) {
                return Failure{
                    ParseError{.line = 0, .column = column, .message = std::move(message)}
                };
            }

            template <typename T>
            [[nodiscard]] std::expected<T, ParseError> take_unsigned(Cursor& cursor, const std::string_view what) {
                cursor.skip_spaces();
                const std::size_t column = cursor.column();
                if (!Cursor::is_digit(cursor.peek())) {
                    return fail(column, std::string{"expected "} + std::string{what});
                }
                std::string_view digits = cursor.rest();
                std::size_t length      = 0;
                while (length < digits.size() && Cursor::is_digit(digits[length])) {
                    ++length;
                }
                digits = digits.substr(0, length);
                T value{};
                const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
                if (error != std::errc{}) {
                    return fail(column, std::string{what} + " is out of range");
                }
                cursor.move_to(cursor.position() + length);
                return value;
            }

            [[nodiscard]] std::expected<core::Duration, ParseError> parse_seconds(Cursor& cursor) {
                const std::size_t column     = cursor.column();
                const std::string_view token = cursor.take_number();
                if (token.empty()) {
                    return fail(column, "expected a time");
                }
                std::string text;
                if (token.starts_with('.')) {
                    text = "0";
                }
                text                    += token;
                double seconds           = 0;
                const auto [end, error]  = std::from_chars(text.data(), text.data() + text.size(), seconds);
                if (error != std::errc{}) {
                    return fail(column, "time is out of range");
                }
                const auto micros =
                    std::chrono::round<std::chrono::microseconds>(std::chrono::duration<double>{seconds});
                return std::chrono::duration_cast<core::Duration>(micros);
            }

            [[nodiscard]] std::expected<TimeSpec, ParseError> parse_time(Cursor& cursor) {
                if (cursor.consume('*')) {
                    return TimeSpec{.kind = TimeSpec::Kind::Any, .low = {}, .high = {}};
                }
                const bool relative = cursor.consume('+');
                const auto low      = parse_seconds(cursor);
                if (!low) {
                    return std::unexpected{low.error()};
                }
                auto high = *low;
                if (cursor.consume('~')) {
                    if (relative) {
                        cursor.consume('+');
                    }
                    const auto end = parse_seconds(cursor);
                    if (!end) {
                        return std::unexpected{end.error()};
                    }
                    high = *end;
                }
                return TimeSpec{
                    .kind = relative ? TimeSpec::Kind::Relative : TimeSpec::Kind::Absolute, .low = *low, .high = high};
            }

            [[nodiscard]] std::expected<wire::TcpFlags, ParseError> parse_flags(Cursor& cursor) {
                const std::size_t column = cursor.column();
                if (cursor.peek() == '[' || cursor.peek() == '(') {
                    return fail(column, "'" + std::string{cursor.take_token()} + "' is not supported");
                }
                if (cursor.consume('-')) {
                    return wire::TcpFlags{};
                }
                const std::string_view word = cursor.take_word();
                if (word.empty()) {
                    return fail(column, "expected packet flags");
                }
                if (word == "udp" || word == "icmp" || word == "gre" || word == "mpls") {
                    return fail(column, "'" + std::string{word} + "' is not supported");
                }
                std::uint8_t raw = 0;
                for (std::size_t index = 0; index < word.size(); ++index) {
                    switch (word[index]) {
                        case 'F':
                            raw |= std::to_underlying(wire::TcpFlag::Fin);
                            break;
                        case 'S':
                            raw |= std::to_underlying(wire::TcpFlag::Syn);
                            break;
                        case 'R':
                            raw |= std::to_underlying(wire::TcpFlag::Rst);
                            break;
                        case 'P':
                            raw |= std::to_underlying(wire::TcpFlag::Psh);
                            break;
                        case 'U':
                            raw |= std::to_underlying(wire::TcpFlag::Urg);
                            break;
                        case '.':
                            raw |= std::to_underlying(wire::TcpFlag::Ack);
                            break;
                        default:
                            return fail(column + index, std::string{"unknown flag '"} + word[index] + "'");
                    }
                }
                return wire::TcpFlags::from_raw(raw);
            }

            /// `first:last(length)`, filled into the packet line.
            [[nodiscard]] std::expected<void, ParseError> parse_range(Cursor& cursor, PacketLine& packet) {
                cursor.skip_spaces();
                const std::size_t column = cursor.column();
                const auto first         = take_unsigned<std::uint32_t>(cursor, "a sequence number");
                if (!first) {
                    return std::unexpected{first.error()};
                }
                if (!cursor.consume(':')) {
                    return fail(cursor.column(), "expected ':' after the first sequence number");
                }
                const auto last = take_unsigned<std::uint32_t>(cursor, "a sequence number");
                if (!last) {
                    return std::unexpected{last.error()};
                }
                if (!cursor.consume('(')) {
                    return fail(cursor.column(), "expected '(length)' after first:last");
                }
                const auto length = take_unsigned<std::uint32_t>(cursor, "a length");
                if (!length) {
                    return std::unexpected{length.error()};
                }
                if (!cursor.consume(')')) {
                    return fail(cursor.column(), "expected ')' after the length");
                }
                if (*last < *first || *last - *first != *length) {
                    return fail(column,
                                "length " + std::to_string(*length) + " does not match first:last " +
                                    std::to_string(*first) + ":" + std::to_string(*last));
                }
                packet.first  = *first;
                packet.last   = *last;
                packet.length = *length;
                return {};
            }

            template <typename T>
            [[nodiscard]] std::expected<void, ParseError>
            parse_field(Cursor& cursor, std::optional<T>& field, const std::string_view name) {
                if (field.has_value()) {
                    return fail(cursor.column(), "duplicate '" + std::string{name} + "'");
                }
                const auto value = take_unsigned<T>(cursor, "a number after '" + std::string{name} + "'");
                if (!value) {
                    return std::unexpected{value.error()};
                }
                field = *value;
                return {};
            }

            /// `ack n`, `win n` and `urg n`, in any order, each at most once.
            [[nodiscard]] std::expected<void, ParseError> parse_fields(Cursor& cursor, PacketLine& packet) {
                while (true) {
                    cursor.skip_spaces();
                    const std::size_t start     = cursor.position();
                    const std::string_view word = cursor.take_word();
                    std::expected<void, ParseError> result;
                    if (word == "ack") {
                        result = parse_field(cursor, packet.ack, word);
                    } else if (word == "win") {
                        result = parse_field(cursor, packet.window, word);
                    } else if (word == "urg") {
                        result = parse_field(cursor, packet.urgent, word);
                    } else {
                        cursor.move_to(start);
                        return {};
                    }
                    if (!result) {
                        return result;
                    }
                }
            }

            [[nodiscard]] std::expected<SackBlock, ParseError> parse_sack_block(Cursor& cursor) {
                const auto first = take_unsigned<std::uint32_t>(cursor, "a sack block");
                if (!first) {
                    return std::unexpected{first.error()};
                }
                if (!cursor.consume(':')) {
                    return fail(cursor.column(), "expected ':' in a sack block");
                }
                const auto last = take_unsigned<std::uint32_t>(cursor, "the end of a sack block");
                if (!last) {
                    return std::unexpected{last.error()};
                }
                return SackBlock{.first = *first, .last = *last};
            }

            [[nodiscard]] std::expected<void, ParseError> parse_timestamps(Cursor& cursor, TcpOption& option) {
                cursor.skip_spaces();
                if (!cursor.consume_text("val")) {
                    return fail(cursor.column(), "option TS needs a value");
                }
                const auto value = take_unsigned<std::uint32_t>(cursor, "a value after 'val'");
                if (!value) {
                    return std::unexpected{value.error()};
                }
                cursor.skip_spaces();
                if (!cursor.consume_text("ecr")) {
                    return fail(cursor.column(), "option TS needs an 'ecr' value");
                }
                const auto echo = take_unsigned<std::uint32_t>(cursor, "a value after 'ecr'");
                if (!echo) {
                    return std::unexpected{echo.error()};
                }
                option.value = *value;
                option.echo  = *echo;
                return {};
            }

            [[nodiscard]] std::expected<void, ParseError> parse_sack(Cursor& cursor, TcpOption& option) {
                cursor.skip_spaces();
                if (!Cursor::is_digit(cursor.peek())) {
                    return fail(cursor.column(), "option sack needs a value");
                }
                while (true) {
                    const auto block = parse_sack_block(cursor);
                    if (!block) {
                        return std::unexpected{block.error()};
                    }
                    option.blocks.push_back(*block);
                    if (cursor.peek() == ',' && Cursor::is_digit(cursor.peek_after_next())) {
                        cursor.consume(',');
                        continue;
                    }
                    return {};
                }
            }

            /// The option's number, with the message the brief fixes when it is missing.
            template <typename T>
            [[nodiscard]] std::expected<T, ParseError> take_option_value(Cursor& cursor, const std::string_view name) {
                cursor.skip_spaces();
                if (!Cursor::is_digit(cursor.peek())) {
                    return fail(cursor.column(), "option " + std::string{name} + " needs a value");
                }
                return take_unsigned<T>(cursor, "a value");
            }

            [[nodiscard]] std::expected<TcpOption, ParseError> parse_option(Cursor& cursor) {
                cursor.skip_spaces();
                const std::size_t column    = cursor.column();
                const std::string_view name = cursor.take_word();
                TcpOption option;
                if (name == "nop") {
                    option.kind = TcpOption::Kind::Nop;
                } else if (name == "eol") {
                    option.kind = TcpOption::Kind::Eol;
                } else if (name == "sackOK") {
                    option.kind = TcpOption::Kind::SackPermitted;
                } else if (name == "mss") {
                    option.kind    = TcpOption::Kind::Mss;
                    const auto mss = take_option_value<std::uint16_t>(cursor, name);
                    if (!mss) {
                        return std::unexpected{mss.error()};
                    }
                    option.mss = *mss;
                } else if (name == "wscale") {
                    option.kind      = TcpOption::Kind::WindowScale;
                    const auto shift = take_option_value<std::uint8_t>(cursor, name);
                    if (!shift) {
                        return std::unexpected{shift.error()};
                    }
                    option.shift = *shift;
                } else if (name == "TS") {
                    option.kind = TcpOption::Kind::Timestamps;
                    if (const auto result = parse_timestamps(cursor, option); !result) {
                        return std::unexpected{result.error()};
                    }
                } else if (name == "sack") {
                    option.kind = TcpOption::Kind::Sack;
                    if (const auto result = parse_sack(cursor, option); !result) {
                        return std::unexpected{result.error()};
                    }
                } else {
                    return fail(column, "unknown option '" + std::string{name} + "'");
                }
                return option;
            }

            /// `<` already consumed: `...>` or comma-separated options, then `>`.
            [[nodiscard]] std::expected<Options, ParseError> parse_options(Cursor& cursor) {
                Options options;
                cursor.skip_spaces();
                if (cursor.consume_text("...")) {
                    options.any = true;
                } else {
                    while (true) {
                        const auto option = parse_option(cursor);
                        if (!option) {
                            return std::unexpected{option.error()};
                        }
                        options.list.push_back(*option);
                        cursor.skip_spaces();
                        if (!cursor.consume(',')) {
                            break;
                        }
                    }
                }
                cursor.skip_spaces();
                if (!cursor.consume('>')) {
                    return fail(cursor.column(), "expected '>' to close the options");
                }
                return options;
            }

            /// After the direction: flags, range, fields and options, then the end of the line.
            [[nodiscard]] std::expected<PacketLine, ParseError> parse_packet(Cursor& cursor,
                                                                             const Direction direction) {
                PacketLine packet;
                packet.direction = direction;
                cursor.skip_spaces();
                const auto flags = parse_flags(cursor);
                if (!flags) {
                    return std::unexpected{flags.error()};
                }
                packet.flags = *flags;
                if (const auto range = parse_range(cursor, packet); !range) {
                    return std::unexpected{range.error()};
                }
                if (const auto fields = parse_fields(cursor, packet); !fields) {
                    return std::unexpected{fields.error()};
                }
                cursor.skip_spaces();
                if (cursor.consume('<')) {
                    const auto options = parse_options(cursor);
                    if (!options) {
                        return std::unexpected{options.error()};
                    }
                    packet.options = *options;
                }
                if (!cursor.at_end()) {
                    return fail(cursor.column(), "unexpected '" + std::string{cursor.rest()} + "'");
                }
                return packet;
            }

            [[nodiscard]] std::expected<std::int64_t, ParseError> take_signed(Cursor& cursor,
                                                                              const std::string_view what) {
                cursor.skip_spaces();
                const bool negative = cursor.consume('-');
                const auto value    = take_unsigned<std::int64_t>(cursor, what);
                if (!value) {
                    return std::unexpected{value.error()};
                }
                return negative ? -*value : *value;
            }

            /// Text up to the `close` that matches an opener already consumed; the closer is consumed too.
            [[nodiscard]] std::expected<std::string, ParseError>
            take_enclosed(Cursor& cursor, const char open, const char close) {
                const std::size_t column = cursor.column();
                std::string text;
                int depth = 0;
                while (!cursor.rest().empty()) {
                    const char c = cursor.peek();
                    if (c == close && depth == 0) {
                        cursor.consume(close);
                        return text;
                    }
                    depth += c == open ? 1 : (c == close ? -1 : 0);
                    text  += c;
                    cursor.consume(c);
                }
                return fail(column, std::string{"expected '"} + close + "' to close '" + open + "'");
            }

            [[nodiscard]] std::expected<Argument, ParseError> parse_argument(Cursor& cursor) {
                cursor.skip_spaces();
                const std::size_t column = cursor.column();
                const char c             = cursor.peek();
                if (cursor.consume_text("...")) {
                    return Argument{.kind = Argument::Kind::Ellipsis};
                }
                if (c == '-' || Cursor::is_digit(c)) {
                    const auto value = take_signed(cursor, "an integer");
                    if (!value) {
                        return std::unexpected{value.error()};
                    }
                    return Argument{.kind = Argument::Kind::Integer, .integer = *value};
                }
                if (c == '[' || c == '"') {
                    cursor.consume(c);
                    const char close = c == '[' ? ']' : '"';
                    auto text        = take_enclosed(cursor, c == '[' ? '[' : '\0', close);
                    if (!text) {
                        return std::unexpected{text.error()};
                    }
                    return Argument{.kind = c == '[' ? Argument::Kind::Bracketed : Argument::Kind::String,
                                    .text = *std::move(text)};
                }
                const std::string_view word = cursor.take_word();
                if (word.empty() || word.contains('.')) {
                    return fail(column, "expected an argument");
                }
                return Argument{.kind = Argument::Kind::Word, .text = std::string{word}};
            }

            /// `(` consumed: arguments up to the closing `)`.
            [[nodiscard]] std::expected<std::vector<Argument>, ParseError> parse_arguments(Cursor& cursor) {
                std::vector<Argument> arguments;
                cursor.skip_spaces();
                if (cursor.consume(')')) {
                    return arguments;
                }
                while (true) {
                    auto argument = parse_argument(cursor);
                    if (!argument) {
                        return std::unexpected{argument.error()};
                    }
                    arguments.push_back(*std::move(argument));
                    cursor.skip_spaces();
                    if (cursor.consume(',')) {
                        continue;
                    }
                    if (cursor.consume(')')) {
                        return arguments;
                    }
                    return fail(cursor.column(), "expected ',' or ')' in the arguments");
                }
            }

            /// After the time: `name(args) = result [ERRNO [(note)]]`.
            [[nodiscard]] std::expected<CallLine, ParseError> parse_call(Cursor& cursor) {
                CallLine call;
                cursor.skip_spaces();
                const std::size_t column    = cursor.column();
                const std::string_view name = cursor.take_word();
                if (name.empty()) {
                    return fail(column, "expected a packet direction or a call");
                }
                call.name = std::string{name};
                if (!cursor.consume('(')) {
                    return fail(cursor.column(), "expected '(' after the call name");
                }
                auto arguments = parse_arguments(cursor);
                if (!arguments) {
                    return std::unexpected{arguments.error()};
                }
                call.arguments = *std::move(arguments);
                cursor.skip_spaces();
                if (!cursor.consume('=')) {
                    return fail(cursor.column(), "expected '=' after the call");
                }
                const auto result = take_signed(cursor, "a result");
                if (!result) {
                    return std::unexpected{result.error()};
                }
                call.result = *result;
                cursor.skip_spaces();
                call.error = std::string{cursor.take_word()};
                cursor.skip_spaces();
                if (!call.error.empty() && cursor.consume('(')) {
                    auto note = take_enclosed(cursor, '(', ')');
                    if (!note) {
                        return std::unexpected{note.error()};
                    }
                    call.note = *std::move(note);
                }
                if (!cursor.at_end()) {
                    return fail(cursor.column(), "unexpected '" + std::string{cursor.rest()} + "'");
                }
                return call;
            }

            /// `--name=value`, applied to the script; only before its first event.
            [[nodiscard]] std::expected<void, ParseError> parse_option_line(Cursor& cursor, Script& script) {
                const std::size_t column = cursor.column();
                if (!script.events.empty()) {
                    return fail(column, "an option line after the first event");
                }
                cursor.consume_text("--");
                const std::string_view name = cursor.take_word();
                if (name != "tolerance_usecs") {
                    return fail(column, "unknown option '--" + std::string{name} + "'");
                }
                if (!cursor.consume('=')) {
                    return fail(cursor.column(), "expected '=' after '--" + std::string{name} + "'");
                }
                const auto micros = take_unsigned<std::int64_t>(cursor, "a number of microseconds");
                if (!micros) {
                    return std::unexpected{micros.error()};
                }
                if (!cursor.at_end()) {
                    return fail(cursor.column(), "unexpected '" + std::string{cursor.rest()} + "'");
                }
                script.tolerance = std::chrono::duration_cast<core::Duration>(std::chrono::microseconds{*micros});
                return {};
            }

            [[nodiscard]] std::string_view strip(std::string_view line) noexcept {
                if (const std::size_t comment = line.find("//"); comment != std::string_view::npos) {
                    line = line.substr(0, comment);
                }
                while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
                    line.remove_suffix(1);
                }
                return line;
            }

            /// One stripped, non-empty line to an event; the line number is the caller's.
            [[nodiscard]] std::expected<Event, ParseError> parse_line(const std::string_view text) {
                Cursor cursor{text};
                cursor.skip_spaces();
                if (cursor.peek() == '`') {
                    return fail(cursor.column(),
                                "shell commands are not supported: the harness configures what defaults.sh would");
                }
                const auto time = parse_time(cursor);
                if (!time) {
                    return std::unexpected{time.error()};
                }
                cursor.skip_spaces();
                const char arrow = cursor.peek();
                if (arrow != '<' && arrow != '>') {
                    auto call = parse_call(cursor);
                    if (!call) {
                        return std::unexpected{call.error()};
                    }
                    return Event{.time = *time, .line = 0, .action = *std::move(call)};
                }
                cursor.consume(arrow);
                auto packet = parse_packet(cursor, arrow == '<' ? Direction::Inbound : Direction::Outbound);
                if (!packet) {
                    return std::unexpected{packet.error()};
                }
                return Event{.time = *time, .line = 0, .action = *packet};
            }

        }  // namespace detail

    }  // namespace

    std::expected<Script, ParseError> parse_script(const std::string_view text) {
        Script script;
        std::size_t line_number = 0;
        std::size_t start       = 0;
        while (start <= text.size()) {
            const std::size_t newline   = std::min(text.find('\n', start), text.size());
            const std::string_view line = detail::strip(text.substr(start, newline - start));
            start                       = newline + 1;
            ++line_number;
            if (std::ranges::all_of(line, [](const char c) { return c == ' ' || c == '\t'; })) {
                continue;
            }
            detail::Cursor cursor{line};
            cursor.skip_spaces();
            if (cursor.rest().starts_with("--")) {
                if (auto option = detail::parse_option_line(cursor, script); !option) {
                    ParseError error = option.error();
                    error.line       = line_number;
                    return std::unexpected{std::move(error)};
                }
                continue;
            }
            auto event = detail::parse_line(line);
            if (!event) {
                ParseError error = event.error();
                error.line       = line_number;
                return std::unexpected{std::move(error)};
            }
            event->line = line_number;
            script.events.push_back(*std::move(event));
        }
        return script;
    }

}  // namespace aloe::scenario
