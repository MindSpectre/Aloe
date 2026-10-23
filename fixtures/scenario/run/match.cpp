#include "match.hpp"

#include <aloe/wire>
#include <optional>
#include <string>

#include <notation.hpp>
#include <script.hpp>

namespace aloe::scenario {

    std::optional<std::string>
    match(const PacketLine& line, const ObservedSegment& seen, const wire::TcpSequence sack_base) {
        if (line.flags != seen.flags) {
            return "flags differ";
        }
        if (line.first != seen.first || line.last != seen.last || line.length != seen.length) {
            return "sequence differs";
        }
        if (line.ack && *line.ack != seen.ack) {
            return "ack differs";
        }
        if (line.window && *line.window != seen.window) {
            return "window differs";
        }
        if (line.urgent && *line.urgent != seen.urgent) {
            return "urgent differs";
        }
        if (!line.options.any && encode_options(line.options, sack_base) != seen.options) {
            return "options differ";
        }
        return std::nullopt;
    }

}  // namespace aloe::scenario
