#pragma once

#include <aloe/core>
#include <aloe/wire>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include <script.hpp>

namespace aloe::scenario {

    /// The option list as header bytes, in the order written; sack blocks get `sack_base` added. `any` has no
    /// encoding (asserted).
    [[nodiscard]] std::vector<std::byte> encode_options(const Options& options, wire::TcpSequence sack_base);

    /// Header bytes back into the script's words, sack blocks minus `sack_base`; an unknown kind as `kind N`;
    /// malformed bytes as `bad options`.
    [[nodiscard]] std::string describe_options(std::span<const std::byte> options, wire::TcpSequence sack_base);

    /// As written in a script: "> S. 0:0(0) ack 1 win 46720 <mss 1460>".
    [[nodiscard]] std::string describe(const PacketLine& line, wire::TcpSequence sack_base);

    /// The same notation for a segment the stack sent: always outbound.
    [[nodiscard]] std::string describe(const ObservedSegment& segment, wire::TcpSequence sack_base);

    /// "+0.010000" for ten milliseconds since the start: a sign, seconds, six decimals.
    [[nodiscard]] std::string format_time(core::Duration since_start);

}  // namespace aloe::scenario
