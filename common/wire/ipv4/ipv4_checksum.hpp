#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include <bytes.hpp>
#include <checksum.hpp>
#include <ipv4_address.hpp>
#include <ipv4_protocol.hpp>

namespace aloe::wire {

    inline constexpr std::size_t ipv4_checksum_offset = 10;

    /// The checksum of an IPv4 header, whatever its checksum field currently holds.
    [[nodiscard]] constexpr std::uint16_t ipv4_header_checksum(const std::span<const std::byte> header) noexcept {
        assert(header.size() >= ipv4_checksum_offset + 2);
        std::uint32_t sum = checksum_add(0, header.first(ipv4_checksum_offset));
        sum               = checksum_add(sum, header.subspan(ipv4_checksum_offset + 2));
        return checksum_finish(sum);
    }

    /**
     * @brief The folded, not inverted sum of the IPv4 pseudo-header.
     *
     * This is what a caller writes into the L4 checksum field before asking a device to complete the
     * checksum, and what the device then continues from.
     */
    [[nodiscard]] constexpr std::uint16_t ipv4_pseudo_header_sum(const Ipv4Address source,
                                                                 const Ipv4Address destination,
                                                                 const Ipv4Protocol protocol,
                                                                 const std::uint16_t l4_length) noexcept {
        std::array<std::byte, 12> pseudo{};
        for (std::size_t index = 0; index < Ipv4Address::size; ++index) {
            pseudo[index]     = source.bytes()[index];
            pseudo[4 + index] = destination.bytes()[index];
        }
        pseudo[8] = std::byte{0};
        pseudo[9] = std::byte{std::to_underlying(protocol)};
        store_be16(std::span<std::byte>{pseudo}.subspan(10, 2), l4_length);
        return checksum_fold(checksum_add(0, pseudo));
    }

    /// The full TCP or UDP checksum of the segment `l4`, whose checksum field must be zero.
    [[nodiscard]] constexpr std::uint16_t ipv4_l4_checksum(const Ipv4Address source,
                                                           const Ipv4Address destination,
                                                           const Ipv4Protocol protocol,
                                                           const std::span<const std::byte> l4) noexcept {
        assert(l4.size() <= 0xffff);
        const std::uint32_t pseudo =
            ipv4_pseudo_header_sum(source, destination, protocol, static_cast<std::uint16_t>(l4.size()));
        return checksum_finish(checksum_add(pseudo, l4));
    }

}  // namespace aloe::wire
