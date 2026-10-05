#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <bytes.hpp>
#include <mac_address.hpp>

namespace aloe::wire {

    /// The ethertypes the brick acts on. Any other value is a frame it drops and counts.
    enum class EtherType : std::uint16_t {
        Ipv4 = 0x0800,
        Arp  = 0x0806,
    };

    struct EthernetHeader {
        MacAddress destination{};
        MacAddress source{};
        std::uint16_t ethertype = 0;

        friend constexpr bool operator==(const EthernetHeader&, const EthernetHeader&) noexcept = default;
    };

    /// Ethernet header length, the `14` in `mtu() + 14`.
    inline constexpr std::size_t ethernet_header_size = 14;

    /// The header of `frame`, or nothing for a frame shorter than one.
    [[nodiscard]] constexpr std::optional<EthernetHeader>
    parse_ethernet(const std::span<const std::byte> frame) noexcept {
        if (frame.size() < ethernet_header_size) {
            return std::nullopt;
        }
        return EthernetHeader{.destination = MacAddress::load(frame.first(MacAddress::size)),
                              .source      = MacAddress::load(frame.subspan(MacAddress::size, MacAddress::size)),
                              .ethertype   = load_be16(frame.subspan(2 * MacAddress::size, 2))};
    }

    /// Writes `header` into the first 14 bytes of `out`.
    constexpr void write_ethernet(std::span<std::byte> out, const EthernetHeader& header) noexcept {
        assert(out.size() >= ethernet_header_size);
        header.destination.store(out.first(MacAddress::size));
        header.source.store(out.subspan(MacAddress::size, MacAddress::size));
        store_be16(out.subspan(2 * MacAddress::size, 2), header.ethertype);
    }

}  // namespace aloe::wire
