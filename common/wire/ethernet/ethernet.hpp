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
        static constexpr std::size_t size = 14;  ///< The `14` in `mtu() + 14`.

        MacAddress destination{};
        MacAddress source{};
        std::uint16_t ethertype = 0;

        /// The header of `frame`, or nothing for a frame shorter than one.
        [[nodiscard]] static constexpr std::optional<EthernetHeader>
        parse(const std::span<const std::byte> frame) noexcept {
            if (frame.size() < size) {
                return std::nullopt;
            }
            return EthernetHeader{.destination = MacAddress::load(frame.first(MacAddress::size)),
                                  .source      = MacAddress::load(frame.subspan(MacAddress::size, MacAddress::size)),
                                  .ethertype   = load_be16(frame.subspan(2 * MacAddress::size, 2))};
        }

        /// Writes the header into the first 14 bytes of `out`.
        constexpr void write(const std::span<std::byte> out) const noexcept {
            assert(out.size() >= size);
            destination.store(out.first(MacAddress::size));
            source.store(out.subspan(MacAddress::size, MacAddress::size));
            store_be16(out.subspan(2 * MacAddress::size, 2), ethertype);
        }

        friend constexpr bool operator==(const EthernetHeader&, const EthernetHeader&) noexcept = default;
    };

}  // namespace aloe::wire
