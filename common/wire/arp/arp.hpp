#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include <bytes.hpp>
#include <ethernet.hpp>
#include <ipv4_address.hpp>
#include <mac_address.hpp>

namespace aloe::wire {

    enum class ArpOperation : std::uint16_t {
        Request = 1,
        Reply   = 2,
    };

    namespace detail {
        inline constexpr std::uint16_t arp_hardware_ethernet = 1;
        inline constexpr std::uint8_t arp_hardware_length    = MacAddress::size;
        inline constexpr std::uint8_t arp_protocol_length    = Ipv4Address::size;
    }  // namespace detail

    /// An ARP packet for Ethernet over IPv4, the only kind the brick parses.
    struct ArpPacket {
        static constexpr std::size_t size = 28;

        ArpOperation operation = ArpOperation::Request;
        MacAddress sender_mac{};
        Ipv4Address sender_ip{};
        MacAddress target_mac{};
        Ipv4Address target_ip{};

        /**
         * @brief The ARP packet in `payload`, or nothing.
         *
         * Nothing for a payload under 28 bytes, a hardware type other than Ethernet, a protocol other
         * than IPv4, address lengths other than 6 and 4, or an operation other than request and reply.
         * Bytes past the 28th are padding and ignored.
         */
        [[nodiscard]] static constexpr std::optional<ArpPacket>
        parse(const std::span<const std::byte> payload) noexcept {
            if (payload.size() < size) {
                return std::nullopt;
            }
            if (load_be16(payload.first(2)) != detail::arp_hardware_ethernet ||
                load_be16(payload.subspan(2, 2)) != std::to_underlying(EtherType::Ipv4) ||
                payload[4] != std::byte{detail::arp_hardware_length} ||
                payload[5] != std::byte{detail::arp_protocol_length}) {
                return std::nullopt;
            }
            const std::uint16_t code = load_be16(payload.subspan(6, 2));
            if (code != std::to_underlying(ArpOperation::Request) && code != std::to_underlying(ArpOperation::Reply)) {
                return std::nullopt;
            }
            return ArpPacket{.operation  = ArpOperation{code},
                             .sender_mac = MacAddress::load(payload.subspan(8, 6)),
                             .sender_ip  = Ipv4Address::load(payload.subspan(14, 4)),
                             .target_mac = MacAddress::load(payload.subspan(18, 6)),
                             .target_ip  = Ipv4Address::load(payload.subspan(24, 4))};
        }

        /// Writes the packet into the first 28 bytes of `out`.
        constexpr void write(const std::span<std::byte> out) const noexcept {
            assert(out.size() >= size);
            store_be16(out.first(2), detail::arp_hardware_ethernet);
            store_be16(out.subspan(2, 2), std::to_underlying(EtherType::Ipv4));
            out[4] = std::byte{detail::arp_hardware_length};
            out[5] = std::byte{detail::arp_protocol_length};
            store_be16(out.subspan(6, 2), std::to_underlying(operation));
            sender_mac.store(out.subspan(8, 6));
            sender_ip.store(out.subspan(14, 4));
            target_mac.store(out.subspan(18, 6));
            target_ip.store(out.subspan(24, 4));
        }

        friend constexpr bool operator==(const ArpPacket&, const ArpPacket&) noexcept = default;
    };

}  // namespace aloe::wire
