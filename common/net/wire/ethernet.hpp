#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <address.hpp>
#include <bytes.hpp>
#include <device.hpp>

namespace aloe::net {

    /// The ethertypes the brick acts on. Any other value is a frame it drops and counts.
    enum class EtherType : std::uint16_t {
        Ipv4 = 0x0800,
        Arp  = 0x0806,
    };

    struct EthernetHeader {
        device::MacAddress destination{};
        device::MacAddress source{};
        std::uint16_t ethertype = 0;

        friend constexpr bool operator==(const EthernetHeader&, const EthernetHeader&) noexcept = default;
    };

    inline constexpr std::size_t ethernet_header_size = device::ethernet_header_size;

    namespace detail {

        [[nodiscard]] constexpr device::MacAddress load_mac(const std::span<const std::byte> bytes) noexcept {
            assert(bytes.size() >= device::MacAddress::size);
            device::MacAddress::Bytes raw{};
            std::ranges::copy(bytes.first(device::MacAddress::size), raw.begin());
            return device::MacAddress{raw};
        }

        constexpr void store_mac(std::span<std::byte> out, const device::MacAddress address) noexcept {
            assert(out.size() >= device::MacAddress::size);
            std::ranges::copy(address.bytes(), out.begin());
        }

        [[nodiscard]] constexpr device::Ipv4Address load_ipv4(const std::span<const std::byte> bytes) noexcept {
            assert(bytes.size() >= device::Ipv4Address::size);
            device::Ipv4Address::Bytes raw{};
            std::ranges::copy(bytes.first(device::Ipv4Address::size), raw.begin());
            return device::Ipv4Address{raw};
        }

        constexpr void store_ipv4(std::span<std::byte> out, const device::Ipv4Address address) noexcept {
            assert(out.size() >= device::Ipv4Address::size);
            std::ranges::copy(address.bytes(), out.begin());
        }

    }  // namespace detail

    /// The header of `frame`, or nothing for a frame shorter than one.
    [[nodiscard]] constexpr std::optional<EthernetHeader>
    parse_ethernet(const std::span<const std::byte> frame) noexcept {
        if (frame.size() < ethernet_header_size) {
            return std::nullopt;
        }
        return EthernetHeader{.destination = detail::load_mac(frame.first(device::MacAddress::size)),
                              .source =
                                  detail::load_mac(frame.subspan(device::MacAddress::size, device::MacAddress::size)),
                              .ethertype = device::load_be16(frame.subspan(2 * device::MacAddress::size, 2))};
    }

    /// Writes `header` into the first 14 bytes of `out`.
    constexpr void write_ethernet(std::span<std::byte> out, const EthernetHeader& header) noexcept {
        assert(out.size() >= ethernet_header_size);
        detail::store_mac(out.first(device::MacAddress::size), header.destination);
        detail::store_mac(out.subspan(device::MacAddress::size, device::MacAddress::size), header.source);
        device::store_be16(out.subspan(2 * device::MacAddress::size, 2), header.ethertype);
    }

    /// RFC 1112: `01:00:5e`, then the low 23 bits of the group address.
    [[nodiscard]] constexpr device::MacAddress multicast_mac(const device::Ipv4Address group) noexcept {
        const device::Ipv4Address::Bytes& bytes = group.bytes();
        return {0x01,
                0x00,
                0x5e,
                static_cast<std::uint8_t>(std::to_integer<unsigned>(bytes[1]) & 0x7fU),
                std::to_integer<std::uint8_t>(bytes[2]),
                std::to_integer<std::uint8_t>(bytes[3])};
    }

}  // namespace aloe::net
