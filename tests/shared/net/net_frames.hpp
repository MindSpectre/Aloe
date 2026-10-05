#pragma once

#include <algorithm>
#include <aloe/device>
#include <aloe/net>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <frames.hpp>
#include <gtest/gtest.h>

/**
 * Builders for the ARP and ICMP frames the net tests exchange with the brick, over the module's own
 * writers, and a parser for what the brick transmits.
 */
namespace aloe::testing {

    inline constexpr std::uint16_t ethertype_arp = 0x0806;

    [[nodiscard]] inline net::ArpPacket arp_request(const device::MacAddress sender_mac,
                                                    const device::Ipv4Address sender_ip,
                                                    const device::Ipv4Address target_ip) {
        return {.operation  = net::ArpOperation::Request,
                .sender_mac = sender_mac,
                .sender_ip  = sender_ip,
                .target_mac = {},
                .target_ip  = target_ip};
    }

    [[nodiscard]] inline net::ArpPacket arp_reply(const device::MacAddress sender_mac,
                                                  const device::Ipv4Address sender_ip,
                                                  const device::MacAddress target_mac,
                                                  const device::Ipv4Address target_ip) {
        return {.operation  = net::ArpOperation::Reply,
                .sender_mac = sender_mac,
                .sender_ip  = sender_ip,
                .target_mac = target_mac,
                .target_ip  = target_ip};
    }

    /// An ARP frame to `destination_mac` from the packet's sender, padded to the 60-byte minimum as a card pads it.
    [[nodiscard]] inline std::vector<std::byte> arp_frame(const net::ArpPacket& arp,
                                                          const device::MacAddress destination_mac) {
        std::vector<std::byte> payload(60 - device::ethernet_header_size);
        net::write_arp(payload, arp);
        return ethernet_frame(destination_mac, arp.sender_mac, ethertype_arp, payload);
    }

    struct EchoSpec {
        device::MacAddress destination_mac{};
        device::MacAddress source_mac{};
        device::Ipv4Address source{};
        device::Ipv4Address destination{};
        std::uint16_t identifier = 0x1234;
        std::uint16_t sequence   = 1;
        net::IcmpType type       = net::IcmpType::EchoRequest;
        std::uint8_t ttl         = 64;
        bool bad_icmp_checksum   = false;
        bool ipv4_checksum       = true;  ///< False leaves the IPv4 checksum field zero.
    };

    /// An ICMP echo over IPv4 over Ethernet, with `payload` after the 8-byte ICMP header.
    [[nodiscard]] inline std::vector<std::byte> icmp_echo_frame(const EchoSpec& spec,
                                                                const std::span<const std::byte> payload) {
        std::vector<std::byte> ip(net::ipv4_header_size + net::icmp_header_size + payload.size());
        const std::span<std::byte> message = std::span<std::byte>{ip}.subspan(net::ipv4_header_size);
        net::write_icmp(message,
                        {.type     = spec.type,
                         .code     = 0,
                         .checksum = 0,
                         .rest     = (static_cast<std::uint32_t>(spec.identifier) << 16U) | spec.sequence});
        std::ranges::copy(payload, message.begin() + static_cast<std::ptrdiff_t>(net::icmp_header_size));
        std::uint16_t checksum = device::internet_checksum(message);
        if (spec.bad_icmp_checksum) {
            checksum = static_cast<std::uint16_t>(checksum ^ 0x0001U);
        }
        device::store_be16(message.subspan(net::icmp_checksum_offset, 2), checksum);

        const std::span<std::byte> header = std::span<std::byte>{ip}.first(net::ipv4_header_size);
        net::write_ipv4(header,
                        {.total_length   = static_cast<std::uint16_t>(ip.size()),
                         .identification = 0x0102,
                         .ttl            = spec.ttl,
                         .protocol       = device::Ipv4Protocol::Icmp,
                         .source         = spec.source,
                         .destination    = spec.destination});
        if (spec.ipv4_checksum) {
            device::store_be16(header.subspan(device::ipv4_checksum_offset, 2), device::ipv4_header_checksum(header));
        }
        return ethernet_frame(spec.destination_mac, spec.source_mac, ethertype_ipv4, ip);
    }

    /// What the brick transmitted, taken apart for assertions.
    struct ParsedFrame {
        net::EthernetHeader ethernet{};
        std::optional<net::ArpPacket> arp;
        std::optional<net::Ipv4Header> ipv4;
        std::optional<net::IcmpHeader> icmp;
        std::vector<std::byte> ipv4_header;  ///< The header bytes, for a checksum assertion.
        std::vector<std::byte> l4;           ///< The segment or message: total length less the header, no padding.
    };

    [[nodiscard]] inline ParsedFrame parse_frame(const std::span<const std::byte> frame) {
        ParsedFrame parsed;
        const std::optional<net::EthernetHeader> ethernet = net::parse_ethernet(frame);
        EXPECT_TRUE(ethernet.has_value()) << "a frame shorter than an Ethernet header";
        if (!ethernet) {
            return parsed;
        }
        parsed.ethernet                          = *ethernet;
        const std::span<const std::byte> payload = frame.subspan(device::ethernet_header_size);
        if (ethernet->ethertype == ethertype_arp) {
            parsed.arp = net::parse_arp(payload);
            return parsed;
        }
        if (ethernet->ethertype != ethertype_ipv4) {
            return parsed;
        }
        parsed.ipv4 = net::parse_ipv4(payload);
        if (!parsed.ipv4) {
            return parsed;
        }
        const std::size_t header_length = parsed.ipv4->header_length;
        parsed.ipv4_header.assign(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(header_length));
        const std::span<const std::byte> l4 = payload.subspan(header_length, parsed.ipv4->total_length - header_length);
        parsed.l4.assign(l4.begin(), l4.end());
        if (parsed.ipv4->protocol == device::Ipv4Protocol::Icmp) {
            parsed.icmp = net::parse_icmp(l4);
        }
        return parsed;
    }

    /// Zero when the TCP or UDP checksum inside `l4` is right for the addresses and protocol of `header`.
    [[nodiscard]] inline std::uint16_t l4_checksum_residue(const net::Ipv4Header& header,
                                                           const std::span<const std::byte> l4) {
        const std::uint32_t pseudo = device::ipv4_pseudo_header_sum(
            header.source, header.destination, header.protocol, static_cast<std::uint16_t>(l4.size()));
        return device::checksum_finish(device::checksum_add(pseudo, l4));
    }

}  // namespace aloe::testing
