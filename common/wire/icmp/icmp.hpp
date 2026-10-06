#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include <bytes.hpp>

namespace aloe::wire {

    /// The ICMP types the brick acts on. Any other byte is still representable, and dropped.
    enum class IcmpType : std::uint8_t {
        EchoReply   = 0,
        EchoRequest = 8,
    };

    struct IcmpHeader {
        static constexpr std::size_t size            = 8;
        static constexpr std::size_t checksum_offset = 2;

        IcmpType type          = IcmpType::EchoReply;
        std::uint8_t code      = 0;
        std::uint16_t checksum = 0;
        std::uint32_t rest     = 0;  ///< Identifier and sequence number for an echo.

        /// The header at the front of `message`, or nothing for a message under 8 bytes.
        [[nodiscard]] static constexpr std::optional<IcmpHeader>
        parse(const std::span<const std::byte> message) noexcept {
            if (message.size() < size) {
                return std::nullopt;
            }
            return IcmpHeader{.type     = IcmpType{std::to_integer<std::uint8_t>(message[0])},
                              .code     = std::to_integer<std::uint8_t>(message[1]),
                              .checksum = load_be16(message.subspan(checksum_offset, 2)),
                              .rest     = load_be32(message.subspan(4, 4))};
        }

        /// Writes the header into the first 8 bytes of `out`. The checksum covers the whole message and is the
        /// caller's to compute.
        constexpr void write(const std::span<std::byte> out) const noexcept {
            assert(out.size() >= size);
            out[0] = std::byte{std::to_underlying(type)};
            out[1] = std::byte{code};
            store_be16(out.subspan(checksum_offset, 2), checksum);
            store_be32(out.subspan(4, 4), rest);
        }

        friend constexpr bool operator==(const IcmpHeader&, const IcmpHeader&) noexcept = default;
    };

}  // namespace aloe::wire
