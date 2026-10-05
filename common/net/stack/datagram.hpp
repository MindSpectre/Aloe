#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <address.hpp>
#include <packet.hpp>
#include <protocol.hpp>

namespace aloe::net {

    /**
     * @brief One IPv4 datagram the brick received for a transport: the whole frame, and what IPv4 parsed.
     *
     * The transport moves `packet` out to keep it; `l4()` is valid while it has not. A datagram lives in
     * the brick's list until the next `process`, which frees whatever was not taken.
     */
    template <device::IsPacket Packet>
    struct Datagram {
        Packet packet{};
        device::Ipv4Address source{};
        device::Ipv4Address destination{};
        device::Ipv4Protocol protocol       = device::Ipv4Protocol::Icmp;
        std::uint8_t l3_offset              = 0;  ///< Where the IPv4 header starts: 14 today.
        std::uint8_t l3_length              = 0;  ///< 20 to 60; options are accepted and left in place.
        std::uint16_t l4_length             = 0;  ///< From the total length, never from the frame.
        device::ChecksumVerdict l4_checksum = device::ChecksumVerdict::Unknown;  ///< The device's verdict, untouched.

        /// The segment: after the IPv4 header, `l4_length` bytes, padding excluded.
        [[nodiscard]] std::span<std::byte> l4() noexcept {
            return packet.data().subspan(static_cast<std::size_t>(l3_offset) + l3_length, l4_length);
        }
    };

    /// A mapping learned from an ARP frame, reported so a loop can forward it to the other shards.
    struct ArpResolution {
        device::Ipv4Address address{};
        device::MacAddress mac{};

        friend constexpr bool operator==(const ArpResolution&, const ArpResolution&) noexcept = default;
    };

}  // namespace aloe::net
