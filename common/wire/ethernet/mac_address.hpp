#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

namespace aloe::wire {
    namespace detail {
        [[nodiscard]] constexpr std::optional<unsigned> hex_digit(const char character) noexcept {
            if (character >= '0' && character <= '9') {
                return static_cast<unsigned>(character - '0');
            }
            if (character >= 'a' && character <= 'f') {
                return static_cast<unsigned>(character - 'a') + 10U;
            }
            if (character >= 'A' && character <= 'F') {
                return static_cast<unsigned>(character - 'A') + 10U;
            }
            return std::nullopt;
        }
    }  // namespace detail

    /**
     * @brief A 48-bit Ethernet address, stored in transmission order.
     */
    class MacAddress {
    public:
        static constexpr std::size_t size = 6;
        using Bytes                       = std::array<std::byte, size>;

        constexpr MacAddress() = default;

        constexpr explicit MacAddress(const Bytes& bytes) noexcept
            : bytes_{bytes} {
        }

        constexpr MacAddress(const std::uint8_t b0,
                             const std::uint8_t b1,
                             const std::uint8_t b2,
                             const std::uint8_t b3,
                             const std::uint8_t b4,
                             const std::uint8_t b5) noexcept
            : bytes_{std::byte{b0}, std::byte{b1}, std::byte{b2}, std::byte{b3}, std::byte{b4}, std::byte{b5}} {
        }

        [[nodiscard]] static constexpr MacAddress broadcast() noexcept {
            return {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        }

        /// Parses `aa:bb:cc:dd:ee:ff`, either letter case. Anything else yields nothing.
        [[nodiscard]] static constexpr std::optional<MacAddress> parse(const std::string_view text) noexcept {
            if (text.size() != 17) {
                return std::nullopt;
            }
            Bytes bytes{};
            for (std::size_t index = 0; index < size; ++index) {
                const std::size_t position = index * 3;
                const auto high            = detail::hex_digit(text[position]);
                const auto low             = detail::hex_digit(text[position + 1]);
                if (!high || !low) {
                    return std::nullopt;
                }
                if (index + 1 < size && text[position + 2] != ':') {
                    return std::nullopt;
                }
                bytes[index] = std::byte{static_cast<std::uint8_t>((*high << 4U) | *low)};
            }
            return MacAddress{bytes};
        }

        [[nodiscard]] constexpr const Bytes& bytes() const noexcept {
            return bytes_;
        }

        /// Reads an address from the first `size` bytes, as the wire carries it.
        [[nodiscard]] static constexpr MacAddress load(const std::span<const std::byte> bytes) noexcept {
            assert(bytes.size() >= size);
            Bytes raw{};
            std::ranges::copy(bytes.first(size), raw.begin());
            return MacAddress{raw};
        }

        /// Writes the address into the first `size` bytes of `out`.
        constexpr void store(const std::span<std::byte> out) const noexcept {
            assert(out.size() >= size);
            std::ranges::copy(bytes_, out.begin());
        }

        [[nodiscard]] constexpr bool is_broadcast() const noexcept {
            return *this == broadcast();
        }

        /// The group bit, which broadcast frames set too.
        [[nodiscard]] constexpr bool is_multicast() const noexcept {
            return (std::to_integer<unsigned>(bytes_[0]) & 1U) != 0;
        }

        [[nodiscard]] std::string to_string() const {
            return std::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                               std::to_integer<unsigned>(bytes_[0]),
                               std::to_integer<unsigned>(bytes_[1]),
                               std::to_integer<unsigned>(bytes_[2]),
                               std::to_integer<unsigned>(bytes_[3]),
                               std::to_integer<unsigned>(bytes_[4]),
                               std::to_integer<unsigned>(bytes_[5]));
        }

        friend constexpr bool operator==(const MacAddress&, const MacAddress&) noexcept = default;

        friend constexpr std::strong_ordering operator<=>(const MacAddress&, const MacAddress&) noexcept = default;

        friend std::ostream& operator<<(std::ostream& stream, const MacAddress& address) {
            return stream << address.to_string();
        }

    private:
        Bytes bytes_{};
    };
}  // namespace aloe::wire

template <>
struct std::formatter<aloe::wire::MacAddress> : std::formatter<std::string_view> {
    auto format(const aloe::wire::MacAddress& address, auto& context) const {
        return std::formatter<std::string_view>::format(address.to_string(), context);
    }
};
