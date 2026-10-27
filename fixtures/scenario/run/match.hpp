#pragma once

#include <aloe/wire>
#include <optional>
#include <string>

#include <script.hpp>

namespace aloe::scenario {

    /// Nothing when `seen` satisfies `line`; else the first difference, one of "flags differ", "sequence differs",
    /// "ack differs", "window differs", "urgent differs", "options differ". `sack_base` is for the line's sack blocks.
    [[nodiscard]] std::optional<std::string>
    match(const PacketLine& line, const ObservedSegment& seen, wire::TcpSequence sack_base);

}  // namespace aloe::scenario
