#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include <address.hpp>
#include <bytes.hpp>
#include <ethernet.hpp>

namespace aloe::net {

    enum class ArpOperation : std::uint16_t {
        Request = 1,
        Reply   = 2,
    };

    /// An ARP packet for Ethernet over IPv4, the only kind the brick parses.
    struct ArpPacket {
        ArpOperation operation = ArpOperation::Request;
        device::MacAddress sender_mac{};
        device::Ipv4Address sender_ip{};
        device::MacAddress target_mac{};
        device::Ipv4Address target_ip{};

        friend constexpr bool operator==(const ArpPacket&, const ArpPacket&) noexcept = default;
    };

    inline constexpr std::size_t arp_packet_size = 28;

    namespace detail {
        inline constexpr std::uint16_t arp_hardware_ethernet = 1;
        inline constexpr std::uint8_t arp_hardware_length    = device::MacAddress::size;
        inline constexpr std::uint8_t arp_protocol_length    = device::Ipv4Address::size;
    }  // namespace detail

    /**
     * @brief The ARP packet in `payload`, or nothing.
     *
     * Nothing for a payload under 28 bytes, a hardware type other than Ethernet, a protocol other
     * than IPv4, address lengths other than 6 and 4, or an operation other than request and reply.
     * Bytes past the 28th are padding and ignored.
     */
    [[nodiscard]] constexpr std::optional<ArpPacket> parse_arp(const std::span<const std::byte> payload) noexcept {
        if (payload.size() < arp_packet_size) {
            return std::nullopt;
        }
        if (device::load_be16(payload.first(2)) != detail::arp_hardware_ethernet ||
            device::load_be16(payload.subspan(2, 2)) != std::to_underlying(EtherType::Ipv4) ||
            payload[4] != std::byte{detail::arp_hardware_length} ||
            payload[5] != std::byte{detail::arp_protocol_length}) {
            return std::nullopt;
        }
        const std::uint16_t operation = device::load_be16(payload.subspan(6, 2));
        if (operation != std::to_underlying(ArpOperation::Request) &&
            operation != std::to_underlying(ArpOperation::Reply)) {
            return std::nullopt;
        }
        return ArpPacket{.operation  = ArpOperation{operation},
                         .sender_mac = detail::load_mac(payload.subspan(8, 6)),
                         .sender_ip  = detail::load_ipv4(payload.subspan(14, 4)),
                         .target_mac = detail::load_mac(payload.subspan(18, 6)),
                         .target_ip  = detail::load_ipv4(payload.subspan(24, 4))};
    }

    /// Writes `packet` into the first 28 bytes of `out`.
    constexpr void write_arp(std::span<std::byte> out, const ArpPacket& packet) noexcept {
        assert(out.size() >= arp_packet_size);
        device::store_be16(out.first(2), detail::arp_hardware_ethernet);
        device::store_be16(out.subspan(2, 2), std::to_underlying(EtherType::Ipv4));
        out[4] = std::byte{detail::arp_hardware_length};
        out[5] = std::byte{detail::arp_protocol_length};
        device::store_be16(out.subspan(6, 2), std::to_underlying(packet.operation));
        detail::store_mac(out.subspan(8, 6), packet.sender_mac);
        detail::store_ipv4(out.subspan(14, 4), packet.sender_ip);
        detail::store_mac(out.subspan(18, 6), packet.target_mac);
        detail::store_ipv4(out.subspan(24, 4), packet.target_ip);
    }

}  // namespace aloe::net
