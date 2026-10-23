#pragma once

#include <aloe/core>
#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace aloe::scenario {

    /// When an event happens: after the previous event, at a time since the start, or whenever.
    struct TimeSpec {
        enum class Kind : std::uint8_t { Relative, Absolute, Any };
        Kind kind = Kind::Relative;
        core::Duration low{};   ///< The window's start; `+t` and `t` have low == high. Unused for Any.
        core::Duration high{};  ///< The window's end.
        friend bool operator==(const TimeSpec&, const TimeSpec&) = default;
    };

    /// Whether the peer sends the packet (`<`, inbound to the stack) or the stack does (`>`).
    enum class Direction : std::uint8_t { Inbound, Outbound };

    /// One SACK block in relative sequence numbers.
    struct SackBlock {
        std::uint32_t first                                        = 0;
        std::uint32_t last                                         = 0;
        friend bool operator==(const SackBlock&, const SackBlock&) = default;
    };

    /// One TCP option of a packet line.
    struct TcpOption {
        enum class Kind : std::uint8_t { Nop, Eol, Mss, WindowScale, SackPermitted, Sack, Timestamps };
        Kind kind           = Kind::Nop;
        std::uint16_t mss   = 0;          ///< Mss.
        std::uint8_t shift  = 0;          ///< WindowScale.
        std::uint32_t value = 0;          ///< Timestamps: val.
        std::uint32_t echo  = 0;          ///< Timestamps: ecr.
        std::vector<SackBlock> blocks{};  ///< Sack, in relative numbers.
        friend bool operator==(const TcpOption&, const TcpOption&) = default;
    };

    /// The option list of a packet line; `any` is `<...>`.
    struct Options {
        bool any = false;
        std::vector<TcpOption> list{};
        friend bool operator==(const Options&, const Options&) = default;
    };

    /// A packet the peer sends or the stack is expected to send.
    struct PacketLine {
        Direction direction = Direction::Inbound;
        wire::TcpFlags flags{};
        std::uint32_t first  = 0;  ///< Relative sequence numbers.
        std::uint32_t last   = 0;
        std::uint32_t length = 0;
        std::optional<std::uint32_t> ack{};
        std::optional<std::uint16_t> window{};
        std::optional<std::uint16_t> urgent{};
        Options options{};
        friend bool operator==(const PacketLine&, const PacketLine&) = default;
    };

    /// A segment the stack sent, in the script's relative numbers.
    struct ObservedSegment {
        wire::TcpFlags flags{};
        std::uint32_t first  = 0;
        std::uint32_t last   = 0;
        std::uint32_t length = 0;
        std::uint32_t ack    = 0;  ///< Relative to the peer's ISN; meaningful with the Ack flag.
        std::uint16_t window = 0;
        std::uint16_t urgent = 0;
        std::vector<std::byte> options{};
        friend bool operator==(const ObservedSegment&, const ObservedSegment&) = default;
    };

    /// One argument of a call line.
    struct Argument {
        enum class Kind : std::uint8_t { Integer, Ellipsis, Word, Bracketed, String };
        Kind kind            = Kind::Ellipsis;
        std::int64_t integer = 0;
        std::string text{};
        friend bool operator==(const Argument&, const Argument&) = default;
    };

    /// A socket call the script makes, with the result it expects.
    struct CallLine {
        std::string name{};
        std::vector<Argument> arguments{};
        std::int64_t result = 0;
        std::string error{};
        std::string note{};
        friend bool operator==(const CallLine&, const CallLine&) = default;
    };

    /// One line of the script: its time, its source line and what it does.
    struct Event {
        TimeSpec time{};
        std::size_t line = 0;  ///< 1-based source line.
        std::variant<PacketLine, CallLine> action{};
    };

    /// A parsed script.
    struct Script {
        std::optional<core::Duration> tolerance{};
        std::vector<Event> events{};
    };

}  // namespace aloe::scenario
