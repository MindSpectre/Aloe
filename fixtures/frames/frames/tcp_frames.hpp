#pragma once

#include <algorithm>
#include <aloe/wire>
#include <cassert>
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
        std::optional<std::uint16_t> mss{};  ///< Written as the one option when present; set it on SYNs.
        std::vector<std::byte>
            options{};  ///< Raw option bytes, written as they are when non-empty; then `mss` stays absent.
        Checksums checksums =
            Checksums::Correct;  ///< `Wrong` corrupts the TCP checksum only; the IPv4 header stays right.
    };

    /// The four-byte MSS option.
    [[nodiscard]] inline std::vector<std::byte> mss_option(const std::uint16_t mss) {
        std::vector<std::byte> block(wire::TcpOptions::mss_size);
        wire::TcpOptions::write_mss(block, mss);
        return block;
    }

    /// The twelve-byte aligned timestamp block.
    [[nodiscard]] inline std::vector<std::byte> timestamps_option(const std::uint32_t value, const std::uint32_t echo) {
        std::vector<std::byte> block(wire::TcpOptions::timestamps_size);
        wire::TcpOptions::write_timestamps(block, value, echo);
        return block;
    }

    /// The four-byte aligned window scale block: nop, kind 3, length 3, shift.
    [[nodiscard]] inline std::vector<std::byte> window_scale_option(const std::uint8_t shift) {
        std::vector<std::byte> block(wire::TcpOptions::window_scale_size);
        wire::TcpOptions::write_window_scale(block, shift);
        return block;
    }

    /// The four-byte aligned SACK-permitted block: nop, nop, kind 4, length 2.
    [[nodiscard]] inline std::vector<std::byte> sack_permitted_option() {
        std::vector<std::byte> block(wire::TcpOptions::sack_permitted_size);
        wire::TcpOptions::write_sack_permitted(block);
        return block;
    }

    /// The twelve bytes Linux writes when it offers SACK and timestamps together.
    [[nodiscard]] inline std::vector<std::byte> sack_permitted_timestamps_option(const std::uint32_t value,
                                                                                 const std::uint32_t echo) {
        std::vector<std::byte> block(wire::TcpOptions::timestamps_size);
        wire::TcpOptions::write_sack_permitted_timestamps(block, value, echo);
        return block;
    }

    /// The aligned SACK option for one to four blocks, in the order given.
    [[nodiscard]] inline std::vector<std::byte> sack_option(const std::span<const wire::TcpSackBlock> blocks) {
        std::vector<std::byte> block(wire::TcpOptions::sack_size(blocks.size()));
        wire::TcpOptions::write_sack(block, blocks);
        return block;
    }

    /// The blocks one after the other: a SYN's `mss_option` then `timestamps_option`.
    [[nodiscard]] inline std::vector<std::byte> joined(const std::span<const std::vector<std::byte>> blocks) {
        std::vector<std::byte> out;
        for (const std::vector<std::byte>& block : blocks) {
            out.insert(out.end(), block.begin(), block.end());
        }
        return out;
    }

    /// The segment's bytes after the IPv4 header: header, the MSS option when given, then `payload`.
    [[nodiscard]] inline std::vector<std::byte> tcp_segment(const TcpSpec& spec,
                                                            const std::span<const std::byte> payload) {
        const std::size_t option_bytes =
            spec.options.empty() ? (spec.mss ? wire::TcpOptions::mss_size : 0) : spec.options.size();
        assert(spec.options.empty() || !spec.mss);
        assert(option_bytes % 4 == 0 && option_bytes <= wire::TcpHeader::max_size - wire::TcpHeader::size);
        const std::size_t header = wire::TcpHeader::size + option_bytes;
        std::vector<std::byte> segment(header + payload.size());
        wire::TcpHeader{.source_port      = spec.source_port,
                        .destination_port = spec.destination_port,
                        .sequence         = spec.sequence,
                        .acknowledgement  = spec.acknowledgement,
                        .data_offset      = static_cast<std::uint8_t>(header),
                        .flags            = spec.flags,
                        .window           = spec.window}
            .write(segment);
        if (!spec.options.empty()) {
            std::ranges::copy(spec.options, segment.begin() + static_cast<std::ptrdiff_t>(wire::TcpHeader::size));
        } else if (spec.mss) {
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

    /// The option bytes between the fixed header and the data offset of a parsed frame; empty when the frame is not
    /// TCP.
    [[nodiscard]] inline std::span<const std::byte> tcp_options_of(const ParsedFrame& frame) {
        if (!frame.tcp) {
            return {};
        }
        return std::span<const std::byte>{frame.l4}.subspan(wire::TcpHeader::size,
                                                            frame.tcp->data_offset - wire::TcpHeader::size);
    }

    /// The SACK blocks of a parsed frame, in the order written; empty when it carries none or is not TCP.
    [[nodiscard]] inline std::vector<wire::TcpSackBlock> sack_blocks_of(const ParsedFrame& frame) {
        std::vector<wire::TcpSackBlock> blocks;
        const std::span<const std::byte> options = tcp_options_of(frame);
        std::size_t at                           = 0;
        while (at < options.size()) {
            const auto kind = std::to_integer<std::uint8_t>(options[at]);
            if (kind == 0) {
                break;
            }
            if (kind == 1) {
                ++at;
                continue;
            }
            if (at + 1 >= options.size()) {
                break;
            }
            const auto length = std::to_integer<std::size_t>(options[at + 1]);
            if (length < 2 || at + length > options.size()) {
                break;
            }
            if (kind == 5) {
                for (std::size_t block  = at + 2; block + wire::TcpOptions::sack_block_size <= at + length;
                     block             += wire::TcpOptions::sack_block_size) {
                    blocks.push_back({.first = wire::TcpSequence{wire::load_be32(options.subspan(block, 4))},
                                      .end   = wire::TcpSequence{wire::load_be32(options.subspan(block + 4, 4))}});
                }
            }
            at += length;
        }
        return blocks;
    }

}  // namespace aloe::frames
