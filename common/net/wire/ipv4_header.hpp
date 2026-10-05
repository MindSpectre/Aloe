#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include <address.hpp>
#include <bytes.hpp>
#include <checksum.hpp>
#include <ethernet.hpp>
#include <protocol.hpp>

namespace aloe::net {

    /**
     * @brief An IPv4 header as the brick reads and writes it.
     *
     * Options are accepted on receive, where `header_length` says how long the header is, and never
     * written: `write_ipv4` always produces the 20-byte header. `checksum` is the field as found on
     * parse and as given on write, zero when the device will fill it.
     */
    struct Ipv4Header {
        std::uint8_t header_length    = 20;
        std::uint8_t dscp_ecn         = 0;
        std::uint16_t total_length    = 0;
        std::uint16_t identification  = 0;
        bool dont_fragment            = true;
        bool more_fragments           = false;
        std::uint16_t fragment_offset = 0;  ///< In units of eight bytes.
        std::uint8_t ttl              = 64;
        device::Ipv4Protocol protocol = device::Ipv4Protocol::Icmp;
        std::uint16_t checksum        = 0;
        device::Ipv4Address source{};
        device::Ipv4Address destination{};

        [[nodiscard]] constexpr bool is_fragment() const noexcept {
            return more_fragments || fragment_offset != 0;
        }

        friend constexpr bool operator==(const Ipv4Header&, const Ipv4Header&) noexcept = default;
    };

    inline constexpr std::size_t ipv4_header_size = 20;

    namespace detail {
        inline constexpr unsigned ipv4_flag_dont_fragment   = 0x4000U;
        inline constexpr unsigned ipv4_flag_more_fragments  = 0x2000U;
        inline constexpr unsigned ipv4_fragment_offset_mask = 0x1fffU;
    }  // namespace detail

    /**
     * @brief The header at the front of `payload`, or nothing.
     *
     * Nothing under 20 bytes, for a version other than 4, a header length under 20 or past the
     * payload, or a total length under the header length or past the payload. Bytes past the total
     * length are padding and ignored.
     */
    [[nodiscard]] constexpr std::optional<Ipv4Header> parse_ipv4(const std::span<const std::byte> payload) noexcept {
        if (payload.size() < ipv4_header_size) {
            return std::nullopt;
        }
        const auto first = std::to_integer<unsigned>(payload[0]);
        if ((first >> 4U) != 4U) {
            return std::nullopt;
        }
        const std::size_t header_length = (first & 0x0fU) * 4U;
        if (header_length < ipv4_header_size || header_length > payload.size()) {
            return std::nullopt;
        }
        const std::uint16_t total_length = device::load_be16(payload.subspan(2, 2));
        if (total_length < header_length || total_length > payload.size()) {
            return std::nullopt;
        }
        const unsigned flags_fragment = device::load_be16(payload.subspan(6, 2));
        return Ipv4Header{
            .header_length   = static_cast<std::uint8_t>(header_length),
            .dscp_ecn        = std::to_integer<std::uint8_t>(payload[1]),
            .total_length    = total_length,
            .identification  = device::load_be16(payload.subspan(4, 2)),
            .dont_fragment   = (flags_fragment & detail::ipv4_flag_dont_fragment) != 0,
            .more_fragments  = (flags_fragment & detail::ipv4_flag_more_fragments) != 0,
            .fragment_offset = static_cast<std::uint16_t>(flags_fragment & detail::ipv4_fragment_offset_mask),
            .ttl             = std::to_integer<std::uint8_t>(payload[8]),
            .protocol        = device::Ipv4Protocol{std::to_integer<std::uint8_t>(payload[9])},
            .checksum        = device::load_be16(payload.subspan(device::ipv4_checksum_offset, 2)),
            .source          = detail::load_ipv4(payload.subspan(12, 4)),
            .destination     = detail::load_ipv4(payload.subspan(16, 4)),
        };
    }

    /// Writes `header` as a 20-byte header into the front of `out`. `header.header_length` must be 20.
    constexpr void write_ipv4(std::span<std::byte> out, const Ipv4Header& header) noexcept {
        assert(out.size() >= ipv4_header_size);
        assert(header.header_length == ipv4_header_size && "options are never written");
        out[0] = std::byte{0x45};  // version 4, five words
        out[1] = std::byte{header.dscp_ecn};
        device::store_be16(out.subspan(2, 2), header.total_length);
        device::store_be16(out.subspan(4, 2), header.identification);
        unsigned flags_fragment = header.fragment_offset & detail::ipv4_fragment_offset_mask;
        if (header.dont_fragment) {
            flags_fragment |= detail::ipv4_flag_dont_fragment;
        }
        if (header.more_fragments) {
            flags_fragment |= detail::ipv4_flag_more_fragments;
        }
        device::store_be16(out.subspan(6, 2), static_cast<std::uint16_t>(flags_fragment));
        out[8] = std::byte{header.ttl};
        out[9] = std::byte{std::to_underlying(header.protocol)};
        device::store_be16(out.subspan(device::ipv4_checksum_offset, 2), header.checksum);
        detail::store_ipv4(out.subspan(12, 4), header.source);
        detail::store_ipv4(out.subspan(16, 4), header.destination);
    }

}  // namespace aloe::net
