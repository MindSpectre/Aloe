#pragma once

#include <aloe/core>
#include <aloe/device>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <arp_resolution.hpp>

namespace aloe::net {

    struct Ipv4Config {
        wire::Ipv4Address address{};
        std::uint8_t prefix                      = 24;
        /// Absent: an off-subnet destination is NoRoute. With `arp_learning` off it must have a `static_arp` entry.
        std::optional<wire::Ipv4Address> gateway = std::nullopt;
        std::uint8_t ttl                         = 64;
        std::size_t burst_capacity               = 64;  ///< The most packets one `process` takes; ethdev's burst.
        /// Neighbours known from the configuration: used without any ARP frame, and never changed by anything.
        std::vector<ArpResolution> static_arp{};
        /// Resolve neighbours on the wire and keep what is learned until it expires. Off by default: the brick then
        /// knows `static_arp` alone, sends no ARP request and learns from no frame.
        bool arp_learning                   = false;
        // The learned table's size and aging, read and checked only with `arp_learning` on.
        std::size_t arp_capacity            = 256;  ///< The learned entries held at most: positive, at most 2^30.
        core::Duration arp_reachable        = std::chrono::seconds{60};
        core::Duration arp_expire           = std::chrono::seconds{120};
        core::Duration arp_request_interval = std::chrono::seconds{1};
        /// With learning on: a reply addressed to us about an address this brick does not hold is reported in
        /// `resolved()` and not stored, so that a loop can forward it to the shard that asked.
        bool report_unsolicited_replies     = false;
    };

    struct SendRequest {
        wire::Ipv4Address destination{};
        wire::Ipv4Protocol protocol = wire::Ipv4Protocol::Udp;
        device::L4Checksum checksum = device::L4Checksum::None;  ///< Fill the L4 checksum field the caller left zero.
    };

    enum class SendError : std::uint8_t {
        NoRoute,     ///< Off the subnet with no gateway, or, with learning off, a next hop with no static entry.
        Unresolved,  ///< The next hop's MAC is unknown: a request is on its way, or the learned table is full; retry.
        Oversized,   ///< The segment does not fit the MTU.
        Refused,     ///< The ring and the device are both full.
    };

}  // namespace aloe::net
