#pragma once

#include <aloe/wire>
#include <cstdint>
#include <optional>

namespace aloe::net::detail {

    /**
     * @brief Where a unicast datagram goes and which addresses are ours: the subnet, the gateway and the two
     * broadcasts, as route questions with no side effect. `Ipv4::send` and `Ipv4::prebuild` decide the same way.
     */
    class Routing {
    public:
        Routing(const wire::Ipv4Address address,
                const std::uint8_t prefix,
                const std::optional<wire::Ipv4Address> gateway) noexcept
            : address_{address},
              subnet_{address, prefix},
              gateway_{gateway} {
        }

        /// The destination itself on the subnet, the gateway off it, or nothing when there is none. Broadcast and
        /// multicast destinations are not routed and answer with themselves.
        [[nodiscard]] std::optional<wire::Ipv4Address> next_hop(const wire::Ipv4Address destination) const noexcept {
            if (is_broadcast(destination) || destination.is_multicast() || subnet_.contains(destination)) {
                return destination;
            }
            return gateway_;
        }

        /// The limited broadcast, or the subnet broadcast where the subnet has one.
        [[nodiscard]] bool is_broadcast(const wire::Ipv4Address address) const noexcept {
            return address.is_limited_broadcast() || (subnet_.has_broadcast() && address == subnet_.broadcast());
        }

        /// Our unicast address or a broadcast.
        [[nodiscard]] bool for_me(const wire::Ipv4Address destination) const noexcept {
            return destination == address_ || is_broadcast(destination);
        }

    private:
        wire::Ipv4Address address_;
        wire::Ipv4Subnet subnet_;
        std::optional<wire::Ipv4Address> gateway_;
    };

}  // namespace aloe::net::detail
