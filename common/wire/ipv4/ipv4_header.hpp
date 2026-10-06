#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include <bytes.hpp>
#include <ipv4_address.hpp>
#include <ipv4_checksum.hpp>
#include <ipv4_protocol.hpp>

namespace aloe::wire {

    namespace detail {
        inline constexpr unsigned ipv4_flag_dont_fragment   = 0x4000U;
        inline constexpr unsigned ipv4_flag_more_fragments  = 0x2000U;
        inline constexpr unsigned ipv4_fragment_offset_mask = 0x1fffU;
    }  // namespace detail

    /**
     * @brief An IPv4 header as the brick reads and writes it.
     *
     * Options are accepted on receive, where `header_length` says how long the header is, and never
     * written: `write` always produces the 20-byte header. `checksum` is the field as found on
     * parse and as given on write, zero when the device will fill it.
     */
    struct Ipv4Header {
        static constexpr std::size_t size = 20;  ///< Without options: the header Aloe writes.

        std::uint8_t header_length    = 20;
        std::uint8_t dscp_ecn         = 0;
        std::uint16_t total_length    = 0;
        std::uint16_t identification  = 0;
        bool dont_fragment            = true;
        bool more_fragments           = false;
        std::uint16_t fragment_offset = 0;  ///< In units of eight bytes.
        std::uint8_t ttl              = 64;
        Ipv4Protocol protocol         = Ipv4Protocol::Icmp;
        std::uint16_t checksum        = 0;
        Ipv4Address source{};
        Ipv4Address destination{};

        [[nodiscard]] constexpr bool is_fragment() const noexcept {
            return more_fragments || fragment_offset != 0;
        }

        /**
         * @brief The header at the front of `payload`, or nothing.
         *
         * Nothing under 20 bytes, for a version other than 4, a header length under 20 or past the
         * payload, or a total length under the header length or past the payload. Bytes past the total
         * length are padding and ignored.
         */
        [[nodiscard]] static constexpr std::optional<Ipv4Header>
        parse(const std::span<const std::byte> payload) noexcept {
            if (payload.size() < size) {
                return std::nullopt;
            }
            const auto first = std::to_integer<unsigned>(payload[0]);
            if ((first >> 4U) != 4U) {
                return std::nullopt;
            }
            const std::size_t length = (first & 0x0fU) * 4U;
            if (length < size || length > payload.size()) {
                return std::nullopt;
            }
            const std::uint16_t total = load_be16(payload.subspan(2, 2));
            if (total < length || total > payload.size()) {
                return std::nullopt;
            }
            const unsigned flags_fragment = load_be16(payload.subspan(6, 2));
            return Ipv4Header{
                .header_length   = static_cast<std::uint8_t>(length),
                .dscp_ecn        = std::to_integer<std::uint8_t>(payload[1]),
                .total_length    = total,
                .identification  = load_be16(payload.subspan(4, 2)),
                .dont_fragment   = (flags_fragment & detail::ipv4_flag_dont_fragment) != 0,
                .more_fragments  = (flags_fragment & detail::ipv4_flag_more_fragments) != 0,
                .fragment_offset = static_cast<std::uint16_t>(flags_fragment & detail::ipv4_fragment_offset_mask),
                .ttl             = std::to_integer<std::uint8_t>(payload[8]),
                .protocol        = Ipv4Protocol{std::to_integer<std::uint8_t>(payload[9])},
                .checksum        = load_be16(payload.subspan(ipv4_checksum_offset, 2)),
                .source          = Ipv4Address::load(payload.subspan(12, 4)),
                .destination     = Ipv4Address::load(payload.subspan(16, 4)),
            };
        }

        /// Writes the header as 20 bytes into the front of `out`. `header_length` must be 20.
        constexpr void write(const std::span<std::byte> out) const noexcept {
            assert(out.size() >= size);
            assert(header_length == size && "options are never written");
            out[0] = std::byte{0x45};  // version 4, five words
            out[1] = std::byte{dscp_ecn};
            store_be16(out.subspan(2, 2), total_length);
            store_be16(out.subspan(4, 2), identification);
            unsigned flags_fragment = fragment_offset & detail::ipv4_fragment_offset_mask;
            if (dont_fragment) {
                flags_fragment |= detail::ipv4_flag_dont_fragment;
            }
            if (more_fragments) {
                flags_fragment |= detail::ipv4_flag_more_fragments;
            }
            store_be16(out.subspan(6, 2), static_cast<std::uint16_t>(flags_fragment));
            out[8] = std::byte{ttl};
            out[9] = std::byte{std::to_underlying(protocol)};
            store_be16(out.subspan(ipv4_checksum_offset, 2), checksum);
            source.store(out.subspan(12, 4));
            destination.store(out.subspan(16, 4));
        }

        friend constexpr bool operator==(const Ipv4Header&, const Ipv4Header&) noexcept = default;
    };

}  // namespace aloe::wire
