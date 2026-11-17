#pragma once

#include <aloe/wire>

namespace aloe::net {

    /// A mapping learned from an ARP frame, reported so a loop can forward it to the other shards.
    struct ArpResolution {
        wire::Ipv4Address address{};
        wire::MacAddress mac{};

        friend constexpr bool operator==(const ArpResolution&, const ArpResolution&) noexcept = default;
    };

}  // namespace aloe::net
