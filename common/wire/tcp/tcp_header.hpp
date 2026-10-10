#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <utility>

#include <bytes.hpp>
#include <tcp_sequence.hpp>

namespace aloe::wire {

    enum class TcpFlag : std::uint8_t {
        Fin = 0x01,
        Syn = 0x02,
        Rst = 0x04,
        Psh = 0x08,
        Ack = 0x10,
        Urg = 0x20,
        Ece = 0x40,
        Cwr = 0x80,
    };

    /// A set of TcpFlag; `raw()` is the wire byte.
    class TcpFlags {
    public:
        constexpr TcpFlags() noexcept = default;

        constexpr TcpFlags(const std::initializer_list<TcpFlag> flags) noexcept {
            for (const TcpFlag flag : flags) {
                raw_ = static_cast<std::uint8_t>(raw_ | std::to_underlying(flag));
            }
        }

        [[nodiscard]] static constexpr TcpFlags from_raw(const std::uint8_t byte) noexcept {
            TcpFlags flags;
            flags.raw_ = byte;
            return flags;
        }

        [[nodiscard]] constexpr bool has(const TcpFlag flag) const noexcept {
            return (raw_ & std::to_underlying(flag)) != 0;
        }

        [[nodiscard]] constexpr std::uint8_t raw() const noexcept {
            return raw_;
        }

        friend constexpr bool operator==(const TcpFlags&, const TcpFlags&) noexcept = default;

    private:
        std::uint8_t raw_ = 0;
    };

    /**
     * @brief A TCP header as the brick reads and writes it.
     *
     * `data_offset` is in bytes, 20 to 60; the wire encodes it in four-byte words. Options are
     * parsed separately by `TcpOptions` from the bytes between the fixed header and the offset.
     * `checksum` is the field as found on parse and as given on write: zero when the IP brick
     * fills it.
     */
    struct TcpHeader {
        static constexpr std::size_t size            = 20;
        static constexpr std::size_t checksum_offset = 16;
        static constexpr std::size_t max_size        = 60;

        std::uint16_t source_port      = 0;
        std::uint16_t destination_port = 0;
        TcpSequence sequence{};
        TcpSequence acknowledgement{};
        std::uint8_t data_offset = 20;  ///< Bytes, 20 to 60.
        TcpFlags flags{};
        std::uint16_t window         = 0;
        std::uint16_t checksum       = 0;
        std::uint16_t urgent_pointer = 0;

        /// Nothing under 20 bytes, for a data offset under 20 or past the segment.
        [[nodiscard]] static constexpr std::optional<TcpHeader>
        parse(const std::span<const std::byte> segment) noexcept {
            if (segment.size() < size) {
                return std::nullopt;
            }
            const std::size_t offset = (std::to_integer<unsigned>(segment[12]) >> 4U) * 4U;
            if (offset < size || offset > segment.size()) {
                return std::nullopt;
            }
            return TcpHeader{
                .source_port      = load_be16(segment.subspan(0, 2)),
                .destination_port = load_be16(segment.subspan(2, 2)),
                .sequence         = TcpSequence{load_be32(segment.subspan(4, 4))},
                .acknowledgement  = TcpSequence{load_be32(segment.subspan(8, 4))},
                .data_offset      = static_cast<std::uint8_t>(offset),
                .flags            = TcpFlags::from_raw(std::to_integer<std::uint8_t>(segment[13])),
                .window           = load_be16(segment.subspan(14, 2)),
                .checksum         = load_be16(segment.subspan(checksum_offset, 2)),
                .urgent_pointer   = load_be16(segment.subspan(18, 2)),
            };
        }

        /// Writes the 20 fixed bytes into the front of `out`; options are written separately after them.
        constexpr void write(const std::span<std::byte> out) const noexcept {
            assert(out.size() >= size);
            assert(data_offset >= size && data_offset <= max_size && data_offset % 4 == 0);
            store_be16(out.subspan(0, 2), source_port);
            store_be16(out.subspan(2, 2), destination_port);
            store_be32(out.subspan(4, 4), sequence.value);
            store_be32(out.subspan(8, 4), acknowledgement.value);
            out[12] = std::byte{static_cast<std::uint8_t>((data_offset / 4U) << 4U)};
            out[13] = std::byte{flags.raw()};
            store_be16(out.subspan(14, 2), window);
            store_be16(out.subspan(checksum_offset, 2), checksum);
            store_be16(out.subspan(18, 2), urgent_pointer);
        }

        friend constexpr bool operator==(const TcpHeader&, const TcpHeader&) noexcept = default;
    };

}  // namespace aloe::wire
