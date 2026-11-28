#pragma once

#include <aloe/wire>

namespace aloe::net {

    /// An IPv4 address and its MAC: an entry of `Ipv4Config::static_arp`, or a mapping `Ipv4::resolved()` reports for a
    /// loop to forward to the other shards, which an ARP frame gave an entry the brick holds or, with
    /// `report_unsolicited_replies`, which a reply to us carried about an address the brick does not hold and did not
    /// store.
    struct ArpResolution {
        wire::Ipv4Address address{};
        wire::MacAddress mac{};

        friend constexpr bool operator==(const ArpResolution&, const ArpResolution&) noexcept = default;
    };

}  // namespace aloe::net
