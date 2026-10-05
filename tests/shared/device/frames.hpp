#pragma once

#include <algorithm>
#include <aloe/device>
#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

/**
 * Builders for the Ethernet frames the device tests exchange, and helpers to
 * move bytes in and out of packets of any backend.
 */
namespace aloe::testing {

    inline constexpr std::uint16_t ethertype_ipv4         = 0x0800;
    inline constexpr std::uint16_t ethertype_experimental = 0x88b5;  ///< Reserved for local experiments.

    /// `length` bytes of a repeating pattern that starts at `seed`.
    [[nodiscard]] inline std::vector<std::byte> pattern(std::size_t length, std::uint8_t seed = 0) {
        std::vector<std::byte> bytes(length);
        for (std::size_t index = 0; index < length; ++index) {
            bytes[index] = std::byte{static_cast<std::uint8_t>(seed + index)};
        }
        return bytes;
    }

    /// An Ethernet frame: header, then the payload as it is.
    [[nodiscard]] inline std::vector<std::byte> ethernet_frame(const wire::MacAddress& destination,
                                                               const wire::MacAddress& source,
                                                               std::uint16_t ethertype,
                                                               std::span<const std::byte> payload) {
        std::vector<std::byte> frame(wire::ethernet_header_size + payload.size());
        std::ranges::copy(destination.bytes(), frame.begin());
        std::ranges::copy(source.bytes(), frame.begin() + 6);
        wire::store_be16(std::span<std::byte>{frame}.subspan(12, 2), ethertype);
        std::ranges::copy(payload, frame.begin() + static_cast<std::ptrdiff_t>(wire::ethernet_header_size));
        return frame;
    }

    /// How the IPv4 and L4 checksum fields of a built frame are filled.
    enum class Checksums : std::uint8_t {
        Correct,  ///< Both computed.
        Zero,     ///< Both zero, as a caller leaves them before a software fill.
        Seeded,   ///< IPv4 zero, L4 holding the pseudo-header sum, as a caller leaves them for an offload.
        Wrong,    ///< Both computed, then corrupted.
    };

    struct Ipv4Spec {
        wire::MacAddress destination_mac;
        wire::MacAddress source_mac;
        wire::Ipv4Address source;
        wire::Ipv4Address destination;
        std::uint16_t source_port      = 0;
        std::uint16_t destination_port = 0;
        wire::Ipv4Protocol protocol    = wire::Ipv4Protocol::Udp;
        std::uint16_t flags_fragment   = 0x4000;  ///< Don't-fragment set, offset zero.
        Checksums checksums            = Checksums::Correct;
    };

    /**
     * @brief An IPv4 frame over Ethernet.
     *
     * UDP gets an 8-byte header, TCP a 20-byte header with only the ACK flag set. Any other
     * protocol carries the payload straight after the IPv4 header.
     */
    [[nodiscard]] inline std::vector<std::byte> ipv4_frame(const Ipv4Spec& spec, std::span<const std::byte> payload) {
        const std::size_t l4_header = spec.protocol == wire::Ipv4Protocol::Tcp   ? 20
                                      : spec.protocol == wire::Ipv4Protocol::Udp ? 8
                                                                                 : 0;
        const std::size_t l4_length = l4_header + payload.size();
        std::vector<std::byte> ip(20 + l4_length);
        const std::span<std::byte> header = std::span<std::byte>{ip}.first(20);
        header[0]                         = std::byte{0x45};
        wire::store_be16(header.subspan(2, 2), static_cast<std::uint16_t>(ip.size()));
        wire::store_be16(header.subspan(6, 2), spec.flags_fragment);
        header[8] = std::byte{64};
        header[9] = std::byte{std::to_underlying(spec.protocol)};
        std::ranges::copy(spec.source.bytes(), header.begin() + 12);
        std::ranges::copy(spec.destination.bytes(), header.begin() + 16);

        const std::span<std::byte> l4 = std::span<std::byte>{ip}.subspan(20);
        std::size_t checksum_offset   = 0;
        if (spec.protocol == wire::Ipv4Protocol::Udp) {
            wire::store_be16(l4.subspan(0, 2), spec.source_port);
            wire::store_be16(l4.subspan(2, 2), spec.destination_port);
            wire::store_be16(l4.subspan(4, 2), static_cast<std::uint16_t>(l4_length));
            checksum_offset = 6;
        } else if (spec.protocol == wire::Ipv4Protocol::Tcp) {
            wire::store_be16(l4.subspan(0, 2), spec.source_port);
            wire::store_be16(l4.subspan(2, 2), spec.destination_port);
            l4[12]          = std::byte{0x50};  // data offset 5 words
            l4[13]          = std::byte{0x10};  // ACK
            checksum_offset = 16;
        }
        std::ranges::copy(payload, l4.begin() + static_cast<std::ptrdiff_t>(l4_header));

        if (spec.checksums != Checksums::Zero) {
            if (spec.checksums == Checksums::Correct || spec.checksums == Checksums::Wrong) {
                wire::store_be16(header.subspan(wire::ipv4_checksum_offset, 2), wire::ipv4_header_checksum(header));
            }
            if (l4_header != 0) {
                const std::uint16_t value =
                    spec.checksums == Checksums::Seeded
                        ? wire::ipv4_pseudo_header_sum(
                              spec.source, spec.destination, spec.protocol, static_cast<std::uint16_t>(l4_length))
                        : wire::ipv4_l4_checksum(spec.source, spec.destination, spec.protocol, l4);
                wire::store_be16(l4.subspan(checksum_offset, 2), value);
            }
            if (spec.checksums == Checksums::Wrong) {
                header[10] ^= std::byte{0x01};
                if (l4_header != 0) {
                    l4[checksum_offset] ^= std::byte{0x01};
                }
            }
        }
        return ethernet_frame(spec.destination_mac, spec.source_mac, ethertype_ipv4, ip);
    }

    /**
     * @brief Inserts `words` 32-bit no-operation options into the IPv4 header of a built frame.
     *
     * The header length and total length grow, and the header checksum is recomputed when the
     * frame carried one. The L4 checksum does not change: it covers the pseudo-header, not the
     * IPv4 header.
     */
    [[nodiscard]] inline std::vector<std::byte> with_ipv4_options(std::vector<std::byte> frame, std::size_t words) {
        const auto begin = frame.begin() + static_cast<std::ptrdiff_t>(wire::ethernet_header_size + 20);
        frame.insert(begin, words * 4, std::byte{0x01});
        const std::span<std::byte> header =
            std::span<std::byte>{frame}.subspan(wire::ethernet_header_size, 20 + words * 4);
        header[0] = std::byte{static_cast<std::uint8_t>(0x40 | (5 + words))};
        wire::store_be16(header.subspan(2, 2),
                         static_cast<std::uint16_t>(wire::load_be16(header.subspan(2, 2)) + words * 4));
        if (wire::load_be16(header.subspan(wire::ipv4_checksum_offset, 2)) != 0) {
            wire::store_be16(header.subspan(wire::ipv4_checksum_offset, 2), wire::ipv4_header_checksum(header));
        }
        return frame;
    }

    /// The flow a built frame is steered by.
    [[nodiscard]] inline device::FlowTuple flow_of(const Ipv4Spec& spec) {
        const bool fragment = (spec.flags_fragment & 0x3fffU) != 0;
        return device::FlowTuple{.source           = spec.source,
                                 .destination      = spec.destination,
                                 .source_port      = fragment ? std::uint16_t{0} : spec.source_port,
                                 .destination_port = fragment ? std::uint16_t{0} : spec.destination_port,
                                 .protocol         = fragment ? std::nullopt : std::optional{spec.protocol}};
    }

    /// Appends `frame` to an empty packet. False when the packet has no room for it.
    template <device::IsPacket P>
    [[nodiscard]] bool fill(P& packet, std::span<const std::byte> frame) {
        const auto room = packet.append(frame.size());
        if (!room) {
            return false;
        }
        std::ranges::copy(frame, room->begin());
        return true;
    }

    /// A copy of a packet's data.
    template <device::IsPacket P>
    [[nodiscard]] std::vector<std::byte> bytes_of(P& packet) {
        const std::span<const std::byte> data = packet.data();
        return {data.begin(), data.end()};
    }

}  // namespace aloe::testing
