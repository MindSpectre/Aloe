#pragma once

#include <aloe/core>
#include <aloe/device>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace aloe::net {

    struct Ipv4Config {
        wire::Ipv4Address address{};
        std::uint8_t prefix                      = 24;
        std::optional<wire::Ipv4Address> gateway = std::nullopt;  ///< Absent: an off-subnet destination is NoRoute.
        std::uint8_t ttl                         = 64;
        std::size_t burst_capacity               = 64;   ///< The most packets one `process` takes; ethdev's burst.
        std::size_t arp_capacity                 = 256;  ///< A power of two of at least 8.
        core::Duration arp_reachable             = std::chrono::seconds{60};
        core::Duration arp_expire                = std::chrono::seconds{120};
        core::Duration arp_request_interval      = std::chrono::seconds{1};
        /// A multi-queue device's shards share one address, so the shard whose queue receives a reply a sibling
        /// solicited learns it in order to forward it. Off by default (RFC 826 merge rule).
        bool accept_unsolicited_replies          = false;
    };

    struct SendRequest {
        wire::Ipv4Address destination{};
        wire::Ipv4Protocol protocol = wire::Ipv4Protocol::Udp;
        device::L4Checksum checksum = device::L4Checksum::None;  ///< Fill the L4 checksum field the caller left zero.
    };

    enum class SendError : std::uint8_t {
        NoRoute,     ///< Off the subnet with no gateway configured.
        Unresolved,  ///< The next hop's MAC is unknown; a request is on its way, retry later.
        Oversized,   ///< The segment does not fit the MTU.
        Refused,     ///< The ring and the device are both full.
    };

}  // namespace aloe::net
