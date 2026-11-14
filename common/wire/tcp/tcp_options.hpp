#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <bytes.hpp>
#include <tcp_sequence.hpp>

namespace aloe::wire {

    /// One SACK block (RFC 2018): the sequence of its first byte and the sequence after its last.
    struct TcpSackBlock {
        TcpSequence first{};
        TcpSequence end{};

        friend constexpr bool operator==(const TcpSackBlock&, const TcpSackBlock&) noexcept = default;
    };

    /**
     * @brief The TCP options the brick reads and writes: the maximum segment size, the RFC 7323 timestamps and window
     * scale, and the RFC 2018 SACK-permitted and SACK options.
     *
     * `parse` walks the option list between the fixed header and the data offset: end-of-list
     * stops it, no-operation is one byte, every other kind is skipped by its length byte. A length
     * of 0 or 1, a length reaching past the list, a kind without a length byte, or an MSS whose
     * length is not 4, a timestamp option whose length is not 10, a window scale whose length is not 3 or a
     * SACK-permitted whose length is not 2, is malformed and the brick drops the segment. SACK blocks (kind 5) are
     * skipped by their length.
     */
    struct TcpOptions {
        static constexpr std::size_t mss_size = 4;  ///< Kind 2, length 4, two bytes of value.

        static constexpr std::size_t timestamps_size =
            12;  ///< The aligned block: nop, nop, kind 8, length 10, value, echo.

        static constexpr std::size_t window_scale_size   = 4;   ///< The aligned block: nop, kind 3, length 3, shift.
        static constexpr std::size_t sack_permitted_size = 4;   ///< The aligned block: nop, nop, kind 4, length 2.
        static constexpr std::size_t sack_block_size     = 8;   ///< Two sequences.
        static constexpr std::size_t max_sack_blocks     = 4;   ///< Linux's TCP_NUM_SACKS.
        static constexpr std::uint8_t max_window_scale   = 14;  ///< RFC 7323; Linux's TCP_MAX_WSCALE.

        std::optional<std::uint16_t> mss;
        std::optional<std::uint32_t> timestamp_value;  ///< RFC 7323 TSval, set together with the echo.
        std::optional<std::uint32_t> timestamp_echo;   ///< RFC 7323 TSecr.
        std::optional<std::size_t> timestamp_at;  ///< Where kind 8 starts in the bytes parsed: value at +2, echo at +6.
        std::optional<std::uint8_t>
            window_scale;             ///< Kind 3 as written, above 14 included; it means something only on a SYN.
        bool sack_permitted = false;  ///< Kind 4; it means something only on a SYN.

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
                } else if (kind == 3) {
                    if (length != 3) {
                        return std::nullopt;
                    }
                    parsed.window_scale = std::to_integer<std::uint8_t>(options[at + 2]);
                } else if (kind == 4) {
                    if (length != 2) {
                        return std::nullopt;
                    }
                    parsed.sack_permitted = true;
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

        /// Writes the twelve bytes Linux writes when it offers both: kind 4, length 2, kind 8, length 10, value, echo.
        static constexpr void write_sack_permitted_timestamps(const std::span<std::byte> out,
                                                              const std::uint32_t value,
                                                              const std::uint32_t echo) noexcept {
            assert(out.size() >= timestamps_size);
            out[0] = std::byte{4};
            out[1] = std::byte{2};
            out[2] = std::byte{8};
            out[3] = std::byte{10};
            store_be32(out.subspan(4, 4), value);
            store_be32(out.subspan(8, 4), echo);
        }

        /// Writes the aligned SACK-permitted block (nop, nop, kind 4, length 2) into the front of `out`.
        static constexpr void write_sack_permitted(const std::span<std::byte> out) noexcept {
            assert(out.size() >= sack_permitted_size);
            out[0] = std::byte{1};
            out[1] = std::byte{1};
            out[2] = std::byte{4};
            out[3] = std::byte{2};
        }

        /// Writes the aligned window scale block (nop, kind 3, length 3, shift) into the front of `out`.
        static constexpr void write_window_scale(const std::span<std::byte> out, const std::uint8_t shift) noexcept {
            assert(out.size() >= window_scale_size);
            out[0] = std::byte{1};
            out[1] = std::byte{3};
            out[2] = std::byte{3};
            out[3] = std::byte{shift};
        }

        /// The bytes a SACK option of `blocks` blocks takes: nop, nop, kind 5, length, then eight per block.
        [[nodiscard]] static constexpr std::size_t sack_size(const std::size_t blocks) noexcept {
            return 4 + sack_block_size * blocks;
        }

        /// Writes the aligned SACK option for one to four blocks, in the order given, into the front of `out`.
        static constexpr void write_sack(const std::span<std::byte> out,
                                         const std::span<const TcpSackBlock> blocks) noexcept {
            assert(!blocks.empty() && blocks.size() <= max_sack_blocks);
            assert(out.size() >= sack_size(blocks.size()));
            out[0]         = std::byte{1};
            out[1]         = std::byte{1};
            out[2]         = std::byte{5};
            out[3]         = static_cast<std::byte>(2 + sack_block_size * blocks.size());
            std::size_t at = 4;
            for (const TcpSackBlock& block : blocks) {
                store_be32(out.subspan(at, 4), block.first.value);
                store_be32(out.subspan(at + 4, 4), block.end.value);
                at += sack_block_size;
            }
        }
    };

}  // namespace aloe::wire
