#pragma once

#include <aloe/device>
#include <cstdint>

#include <detail/node_pool.hpp>

namespace aloe::tcp::detail {

    /// A received packet a connection holds for the application: the offset of its unconsumed payload, that payload's
    /// length and the next held packet.
    template <device::IsPacket Packet>
    struct HeldSegment {
        Packet packet{};
        std::uint16_t offset = 0;  ///< Of the unconsumed payload inside the packet's data.
        std::uint16_t length = 0;  ///< Unconsumed payload bytes.
        std::uint32_t next   = no_node;
    };

    /// The per-shard pool a connection chains its held segments on. The caller moves the packet into a node only once
    /// `acquire` gave one, so a refused packet stays with the caller.
    template <device::IsPacket Packet>
    using HeldSegmentPool = NodePool<HeldSegment<Packet>>;

}  // namespace aloe::tcp::detail
