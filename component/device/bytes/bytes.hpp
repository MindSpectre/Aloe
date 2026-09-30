#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

namespace aloe {

    /// Reads a big-endian 16-bit value from the first two bytes.
    [[nodiscard]] constexpr std::uint16_t load_be16(std::span<const std::byte> bytes) noexcept {
        assert(bytes.size() >= 2);
        return static_cast<std::uint16_t>((std::to_integer<unsigned>(bytes[0]) << 8U) |
                                          std::to_integer<unsigned>(bytes[1]));
    }

    /// Reads a big-endian 32-bit value from the first four bytes.
    [[nodiscard]] constexpr std::uint32_t load_be32(std::span<const std::byte> bytes) noexcept {
        assert(bytes.size() >= 4);
        return (std::to_integer<std::uint32_t>(bytes[0]) << 24U) | (std::to_integer<std::uint32_t>(bytes[1]) << 16U) |
               (std::to_integer<std::uint32_t>(bytes[2]) << 8U) | std::to_integer<std::uint32_t>(bytes[3]);
    }

    /// Writes a 16-bit value big-endian into the first two bytes.
    constexpr void store_be16(std::span<std::byte> bytes, std::uint16_t value) noexcept {
        assert(bytes.size() >= 2);
        bytes[0] = std::byte{static_cast<std::uint8_t>(value >> 8U)};
        bytes[1] = std::byte{static_cast<std::uint8_t>(value)};
    }

    /// Writes a 32-bit value big-endian into the first four bytes.
    constexpr void store_be32(std::span<std::byte> bytes, std::uint32_t value) noexcept {
        assert(bytes.size() >= 4);
        bytes[0] = std::byte{static_cast<std::uint8_t>(value >> 24U)};
        bytes[1] = std::byte{static_cast<std::uint8_t>(value >> 16U)};
        bytes[2] = std::byte{static_cast<std::uint8_t>(value >> 8U)};
        bytes[3] = std::byte{static_cast<std::uint8_t>(value)};
    }

}  // namespace aloe
