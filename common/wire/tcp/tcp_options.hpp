#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <bytes.hpp>

namespace aloe::wire {

    /**
     * @brief The TCP options the brick reads and writes: the maximum segment size and the RFC 7323 timestamps.
     *
     * `parse` walks the option list between the fixed header and the data offset: end-of-list
     * stops it, no-operation is one byte, every other kind is skipped by its length byte. A length
     * of 0 or 1, a length reaching past the list, a kind without a length byte, or an MSS whose
     * length is not 4 or a timestamp option whose length is not 10, is malformed and the brick drops the segment.
     */
    struct TcpOptions {
        static constexpr std::size_t mss_size = 4;  ///< Kind 2, length 4, two bytes of value.

        static constexpr std::size_t timestamps_size =
            12;  ///< The aligned block: nop, nop, kind 8, length 10, value, echo.

        std::optional<std::uint16_t> mss;
        std::optional<std::uint32_t> timestamp_value;  ///< RFC 7323 TSval, set together with the echo.
        std::optional<std::uint32_t> timestamp_echo;   ///< RFC 7323 TSecr.
        std::optional<std::size_t> timestamp_at;  ///< Where kind 8 starts in the bytes parsed: value at +2, echo at +6.

        [[nodiscard]] static constexpr std::optional<TcpOptions>
        parse(const std::span<const std::byte> options) noexcept {
            TcpOptions parsed;
            std::size_t at = 0;
            while (at < options.size()) {
                const auto kind = std::to_integer<std::uint8_t>(options[at]);
                if (kind == 0) {
                    break;  // end of list
                }
                if (kind == 1) {
                    ++at;  // no-operation
                    continue;
                }
                if (at + 1 >= options.size()) {
                    return std::nullopt;  // a kind with no length byte
                }
                const auto length = std::to_integer<std::size_t>(options[at + 1]);
                if (length < 2 || at + length > options.size()) {
                    return std::nullopt;
                }
                if (kind == 2) {
                    if (length != mss_size) {
                        return std::nullopt;
                    }
                    parsed.mss = load_be16(options.subspan(at + 2, 2));
                } else if (kind == 8) {
                    if (length != 10) {
                        return std::nullopt;
                    }
                    parsed.timestamp_value = load_be32(options.subspan(at + 2, 4));
                    parsed.timestamp_echo  = load_be32(options.subspan(at + 6, 4));
                    parsed.timestamp_at    = at;
                }
                at += length;
            }
            return parsed;
        }

        /// Writes the four-byte MSS option into the front of `out`.
        static constexpr void write_mss(const std::span<std::byte> out, const std::uint16_t mss) noexcept {
            assert(out.size() >= mss_size);
            out[0] = std::byte{2};
            out[1] = std::byte{mss_size};
            store_be16(out.subspan(2, 2), mss);
        }

        /// Writes the twelve-byte aligned timestamp block (nop, nop, kind 8, length 10, value, echo) into the front of
        /// `out`.
        static constexpr void
        write_timestamps(const std::span<std::byte> out, const std::uint32_t value, const std::uint32_t echo) noexcept {
            assert(out.size() >= timestamps_size);
            out[0] = std::byte{1};
            out[1] = std::byte{1};
            out[2] = std::byte{8};
            out[3] = std::byte{10};
            store_be32(out.subspan(4, 4), value);
            store_be32(out.subspan(8, 4), echo);
        }
    };

}  // namespace aloe::wire
