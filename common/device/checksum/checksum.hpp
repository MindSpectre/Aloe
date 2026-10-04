#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include <address.hpp>
#include <bytes.hpp>
#include <protocol.hpp>

namespace aloe::device {

    /**
     * @brief Adds the 16-bit big-endian words of `data` to a running ones-complement sum.
     *
     * An odd trailing byte counts as the high byte of a final word, as RFC 1071 requires.
     * The sum is not folded; fold it with checksum_fold or checksum_finish.
     */
    [[nodiscard]] constexpr std::uint32_t checksum_add(std::uint32_t sum,
                                                       const std::span<const std::byte> data) noexcept {
        std::size_t index = 0;
        for (; index + 1 < data.size(); index += 2) {
            sum += load_be16(data.subspan(index, 2));
        }
        if (index < data.size()) {
            sum += std::to_integer<unsigned>(data[index]) << 8U;
        }
        return sum;
    }

    /// Folds the carries of a running sum into 16 bits. Not inverted.
    [[nodiscard]] constexpr std::uint16_t checksum_fold(std::uint32_t sum) noexcept {
        while ((sum >> 16U) != 0) {
            sum = (sum & 0xffffU) + (sum >> 16U);
        }
        return static_cast<std::uint16_t>(sum);
    }

    /// Folds and inverts a running sum: the value to store, big-endian, in a checksum field.
    [[nodiscard]] constexpr std::uint16_t checksum_finish(const std::uint32_t sum) noexcept {
        return static_cast<std::uint16_t>(~checksum_fold(sum));
    }

    /**
     * @brief The Internet checksum of `data`.
     *
     * Over a header or segment whose checksum field is zero, this is the value to store in that
     * field. Over one with the field in place, zero means the checksum is good.
     */
    [[nodiscard]] constexpr std::uint16_t internet_checksum(const std::span<const std::byte> data) noexcept {
        return checksum_finish(checksum_add(0, data));
    }

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

}  // namespace aloe::device
