#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <bytes.hpp>

namespace aloe::wire {

    /**
     * @brief The TCP options the brick reads and writes: the maximum segment size, and nothing else.
     *
     * `parse` walks the option list between the fixed header and the data offset: end-of-list
     * stops it, no-operation is one byte, every other kind is skipped by its length byte. A length
     * of 0 or 1, a length reaching past the list, a kind without a length byte, or an MSS whose
     * length is not 4, is malformed and the brick drops the segment.
     */
    struct TcpOptions {
        static constexpr std::size_t mss_size = 4;  ///< Kind 2, length 4, two bytes of value.

        std::optional<std::uint16_t> mss;

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
    };

}  // namespace aloe::wire
