#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include <bytes.hpp>

namespace aloe::net {

    /// The ICMP types the brick acts on. Any other byte is still representable, and dropped.
    enum class IcmpType : std::uint8_t {
        EchoReply   = 0,
        EchoRequest = 8,
    };

    struct IcmpHeader {
        IcmpType type          = IcmpType::EchoReply;
        std::uint8_t code      = 0;
        std::uint16_t checksum = 0;
        std::uint32_t rest     = 0;  ///< Identifier and sequence number for an echo.

        friend constexpr bool operator==(const IcmpHeader&, const IcmpHeader&) noexcept = default;
    };

    inline constexpr std::size_t icmp_header_size     = 8;
    inline constexpr std::size_t icmp_checksum_offset = 2;

    /// The header at the front of `message`, or nothing for a message under 8 bytes.
    [[nodiscard]] constexpr std::optional<IcmpHeader> parse_icmp(const std::span<const std::byte> message) noexcept {
        if (message.size() < icmp_header_size) {
            return std::nullopt;
        }
        return IcmpHeader{.type     = IcmpType{std::to_integer<std::uint8_t>(message[0])},
                          .code     = std::to_integer<std::uint8_t>(message[1]),
                          .checksum = device::load_be16(message.subspan(icmp_checksum_offset, 2)),
                          .rest     = device::load_be32(message.subspan(4, 4))};
    }

    /// Writes `header` into the first 8 bytes of `out`. The checksum covers the whole message and is the caller's to
    /// compute.
    constexpr void write_icmp(std::span<std::byte> out, const IcmpHeader& header) noexcept {
        assert(out.size() >= icmp_header_size);
        out[0] = std::byte{std::to_underlying(header.type)};
        out[1] = std::byte{header.code};
        device::store_be16(out.subspan(icmp_checksum_offset, 2), header.checksum);
        device::store_be32(out.subspan(4, 4), header.rest);
    }

}  // namespace aloe::net
