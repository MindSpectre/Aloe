#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include <address.hpp>
#include <bytes.hpp>
#include <device.hpp>
#include <protocol.hpp>
#include <rss.hpp>

namespace aloe::fabric::detail {

    inline constexpr std::uint16_t ethertype_ipv4     = 0x0800;
    inline constexpr std::size_t ipv4_minimum_header  = 20;
    inline constexpr std::size_t tcp_checksum_offset  = 16;
    inline constexpr std::size_t tcp_minimum_header   = 20;
    inline constexpr std::size_t udp_checksum_offset  = 6;
    inline constexpr std::size_t udp_header_size      = 8;
    inline constexpr std::uint16_t ipv4_fragment_mask = 0x3fff;  ///< More-fragments bit and fragment offset.

    /// What the fabric needs to know about an IPv4 frame: where its headers are and what flow it belongs to.
    struct Ipv4Frame {
        std::size_t header_length = 0;  ///< IPv4 header length in bytes.
        std::size_t l4_offset     = 0;  ///< From the start of the frame.
        std::size_t l4_length     = 0;  ///< L4 bytes present, bounded by the frame and the total length.
        device::Ipv4Address source;
        device::Ipv4Address destination;
        device::Ipv4Protocol protocol{};
        bool fragment                  = false;
        bool has_ports                 = false;  ///< Unfragmented TCP or UDP with its ports present.
        std::uint16_t source_port      = 0;
        std::uint16_t destination_port = 0;

        [[nodiscard]] std::span<const std::byte> header(const std::span<const std::byte> frame) const noexcept {
            return frame.subspan(device::ethernet_header_size, header_length);
        }

        [[nodiscard]] std::span<const std::byte> l4(const std::span<const std::byte> frame) const noexcept {
            return frame.subspan(l4_offset, l4_length);
        }

        /// The absolute offset of the L4 checksum field, when the segment is TCP or UDP with a whole header.
        [[nodiscard]] std::optional<std::size_t> l4_checksum_offset() const noexcept {
            if (fragment) {
                return std::nullopt;
            }
            if (protocol == device::Ipv4Protocol::Tcp && l4_length >= tcp_minimum_header) {
                return l4_offset + tcp_checksum_offset;
            }
            if (protocol == device::Ipv4Protocol::Udp && l4_length >= udp_header_size) {
                return l4_offset + udp_checksum_offset;
            }
            return std::nullopt;
        }
    };

    /// Parses the IPv4 header of an Ethernet frame. Nothing for any other frame.
    [[nodiscard]] inline std::optional<Ipv4Frame> parse_ipv4(const std::span<const std::byte> frame) noexcept {
        if (frame.size() < device::ethernet_header_size + ipv4_minimum_header) {
            return std::nullopt;
        }
        if (device::load_be16(frame.subspan(12, 2)) != ethertype_ipv4) {
            return std::nullopt;
        }
        const std::span<const std::byte> ip = frame.subspan(device::ethernet_header_size);
        const auto version_ihl              = std::to_integer<unsigned>(ip[0]);
        if ((version_ihl >> 4U) != 4U) {
            return std::nullopt;
        }
        Ipv4Frame result;
        result.header_length = (version_ihl & 0x0fU) * 4U;
        if (result.header_length < ipv4_minimum_header || result.header_length > ip.size()) {
            return std::nullopt;
        }
        const std::size_t total_length = device::load_be16(ip.subspan(2, 2));
        if (total_length < result.header_length) {
            return std::nullopt;
        }
        result.fragment = (device::load_be16(ip.subspan(6, 2)) & ipv4_fragment_mask) != 0;
        result.protocol = static_cast<device::Ipv4Protocol>(std::to_integer<std::uint8_t>(ip[9]));
        result.source   = device::Ipv4Address{
            {ip[12], ip[13], ip[14], ip[15]}
        };
        result.destination = device::Ipv4Address{
            {ip[16], ip[17], ip[18], ip[19]}
        };
        result.l4_offset = device::ethernet_header_size + result.header_length;
        result.l4_length = std::min(total_length, ip.size()) - result.header_length;
        const bool ported =
            result.protocol == device::Ipv4Protocol::Tcp || result.protocol == device::Ipv4Protocol::Udp;
        if (ported && !result.fragment && result.l4_length >= 4) {
            result.has_ports        = true;
            result.source_port      = device::load_be16(frame.subspan(result.l4_offset, 2));
            result.destination_port = device::load_be16(frame.subspan(result.l4_offset + 2, 2));
        }
        return result;
    }

    /// The flow a frame is steered by. A fragment keeps only its addresses, so the 2-tuple rule applies.
    [[nodiscard]] inline device::FlowTuple flow_of(const Ipv4Frame& ipv4) noexcept {
        device::FlowTuple flow{.source = ipv4.source, .destination = ipv4.destination};
        if (!ipv4.fragment) {
            flow.protocol = ipv4.protocol;
        }
        if (ipv4.has_ports) {
            flow.source_port      = ipv4.source_port;
            flow.destination_port = ipv4.destination_port;
        }
        return flow;
    }

}  // namespace aloe::fabric::detail
