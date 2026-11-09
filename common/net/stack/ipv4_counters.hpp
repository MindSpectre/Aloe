#pragma once

#include <cstdint>

namespace aloe::net {

    /// Per-brick counters, all monotonic, read on the owning thread or after it has stopped.
    struct Ipv4Counters {
        std::uint64_t frames                  = 0;  ///< Handed to `process`.
        std::uint64_t arp_requests_received   = 0;
        std::uint64_t arp_replies_received    = 0;
        std::uint64_t datagrams_received      = 0;  ///< Valid IPv4 for us, whatever the protocol.
        std::uint64_t delivered_tcp           = 0;
        std::uint64_t delivered_udp           = 0;
        std::uint64_t resolutions             = 0;  ///< Mappings learned from ARP frames.
        std::uint64_t arp_requests_sent       = 0;
        std::uint64_t arp_replies_sent        = 0;
        std::uint64_t echo_replies_sent       = 0;
        std::uint64_t datagrams_sent          = 0;
        std::uint64_t transmit_refused        = 0;  ///< A frame the brick itself built that the queue refused.
        std::uint64_t dropped_short           = 0;
        std::uint64_t dropped_not_for_us      = 0;
        std::uint64_t dropped_ethertype       = 0;
        std::uint64_t dropped_arp_malformed   = 0;
        std::uint64_t dropped_arp_conflict    = 0;
        std::uint64_t dropped_arp_unsolicited = 0;
        std::uint64_t dropped_bad_header      = 0;
        std::uint64_t dropped_bad_checksum    = 0;
        std::uint64_t dropped_fragment        = 0;
        std::uint64_t dropped_protocol        = 0;
        std::uint64_t dropped_icmp            = 0;
        std::uint64_t dropped_martian = 0;  ///< A source no host sends from: a multicast, broadcast, loopback or our
                                            ///< own IPv4 address, or an ARP request from a group MAC.
        std::uint64_t dropped_tcp_broadcast = 0;  ///< TCP to a broadcast address: RFC 1122 makes TCP unicast only.
        std::uint64_t send_no_route         = 0;
        std::uint64_t send_unresolved       = 0;
        std::uint64_t send_oversized        = 0;
        std::uint64_t send_refused          = 0;

        friend constexpr bool operator==(const Ipv4Counters&, const Ipv4Counters&) noexcept = default;
    };

}  // namespace aloe::net
