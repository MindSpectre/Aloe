#pragma once

#include <algorithm>
#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <frames.hpp>
#include <net_frames.hpp>

/**
 * A TCP segment over IPv4 over Ethernet, built the way a peer would send it, and the payload of a
 * parsed one. Fixture allocation is fine here; nothing in `common` links this.
 */
namespace aloe::frames {

    struct TcpSpec {
        wire::MacAddress destination_mac{};
        wire::MacAddress source_mac{};
        wire::Ipv4Address source{};
        wire::Ipv4Address destination{};
        std::uint16_t source_port      = 0;
        std::uint16_t destination_port = 0;
        wire::TcpSequence sequence{};
        wire::TcpSequence acknowledgement{};
        wire::TcpFlags flags{};
        std::uint16_t window = 65535;
        std::optional<std::uint16_t> mss;  ///< Written as the one option when present; set it on SYNs.
        Checksums checksums =
            Checksums::Correct;  ///< `Wrong` corrupts the TCP checksum only; the IPv4 header stays right.
    };

    /// The segment's bytes after the IPv4 header: header, the MSS option when given, then `payload`.
    [[nodiscard]] inline std::vector<std::byte> tcp_segment(const TcpSpec& spec,
                                                            const std::span<const std::byte> payload) {
        const std::size_t header = wire::TcpHeader::size + (spec.mss ? wire::TcpOptions::mss_size : 0);
        std::vector<std::byte> segment(header + payload.size());
        wire::TcpHeader{.source_port      = spec.source_port,
                        .destination_port = spec.destination_port,
                        .sequence         = spec.sequence,
                        .acknowledgement  = spec.acknowledgement,
                        .data_offset      = static_cast<std::uint8_t>(header),
                        .flags            = spec.flags,
                        .window           = spec.window}
            .write(segment);
        if (spec.mss) {
            wire::TcpOptions::write_mss(std::span<std::byte>{segment}.subspan(wire::TcpHeader::size), *spec.mss);
        }
        std::ranges::copy(payload, segment.begin() + static_cast<std::ptrdiff_t>(header));
        if (spec.checksums != Checksums::Zero) {
            const std::uint16_t value =
                spec.checksums == Checksums::Seeded
                    ? wire::ipv4_pseudo_header_sum(spec.source,
                                                   spec.destination,
                                                   wire::Ipv4Protocol::Tcp,
                                                   static_cast<std::uint16_t>(segment.size()))
                    : wire::ipv4_l4_checksum(spec.source, spec.destination, wire::Ipv4Protocol::Tcp, segment);
            wire::store_be16(std::span<std::byte>{segment}.subspan(wire::TcpHeader::checksum_offset, 2), value);
            if (spec.checksums == Checksums::Wrong) {
                segment[wire::TcpHeader::checksum_offset] ^= std::byte{0x01};
            }
        }
        return segment;
    }

    /// A TCP segment over IPv4 over Ethernet, as a peer puts it on the wire.
    [[nodiscard]] inline std::vector<std::byte> tcp_frame(const TcpSpec& spec,
                                                          const std::span<const std::byte> payload) {
        const std::vector<std::byte> segment = tcp_segment(spec, payload);
        std::vector<std::byte> ip(wire::Ipv4Header::size + segment.size());
        const std::span<std::byte> header = std::span<std::byte>{ip}.first(wire::Ipv4Header::size);
        wire::Ipv4Header{.total_length   = static_cast<std::uint16_t>(ip.size()),
                         .identification = 0x0102,
                         .ttl            = 64,
                         .protocol       = wire::Ipv4Protocol::Tcp,
                         .source         = spec.source,
                         .destination    = spec.destination}
            .write(header);
        if (spec.checksums == Checksums::Correct || spec.checksums == Checksums::Wrong) {
            // Wrong corrupts the TCP checksum only: the IPv4 header stays right, so the segment reaches TCP.
            wire::store_be16(header.subspan(wire::ipv4_checksum_offset, 2), wire::ipv4_header_checksum(header));
        }
        std::ranges::copy(segment, ip.begin() + static_cast<std::ptrdiff_t>(wire::Ipv4Header::size));
        return ethernet_frame(spec.destination_mac, spec.source_mac, ethertype_ipv4, ip);
    }

    /// The bytes after the TCP header of a parsed frame; empty when the frame is not TCP.
    [[nodiscard]] inline std::span<const std::byte> tcp_payload(const ParsedFrame& frame) {
        if (!frame.tcp) {
            return {};
        }
        return std::span<const std::byte>{frame.l4}.subspan(frame.tcp->data_offset);
    }

}  // namespace aloe::frames
