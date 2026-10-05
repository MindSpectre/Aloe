#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <bytes.hpp>

namespace aloe::wire {

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

}  // namespace aloe::wire
