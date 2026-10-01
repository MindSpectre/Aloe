#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace aloe::device {
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

    /**
     * @brief A 32-bit IPv4 address, stored in network byte order.
     */
    class Ipv4Address {
    public:
        static constexpr std::size_t size = 4;
        using Bytes                       = std::array<std::byte, size>;

        constexpr Ipv4Address() = default;

        constexpr explicit Ipv4Address(const Bytes& bytes) noexcept
            : bytes_{bytes} {
        }

        constexpr Ipv4Address(const std::uint8_t a,
                              const std::uint8_t b,
                              const std::uint8_t c,
                              const std::uint8_t d) noexcept
            : bytes_{std::byte{a}, std::byte{b}, std::byte{c}, std::byte{d}} {
        }

        /// From a host-order value: 0xC0A80001 is 192.168.0.1.
        [[nodiscard]] static constexpr Ipv4Address from_uint32(const std::uint32_t value) noexcept {
            return {static_cast<std::uint8_t>(value >> 24U),
                    static_cast<std::uint8_t>(value >> 16U),
                    static_cast<std::uint8_t>(value >> 8U),
                    static_cast<std::uint8_t>(value)};
        }

        /// Parses dotted decimal, `192.168.0.1`. Anything else yields nothing.
        [[nodiscard]] static constexpr std::optional<Ipv4Address> parse(const std::string_view text) noexcept {
            Bytes bytes{};
            std::size_t index = 0;
            unsigned value    = 0;
            unsigned digits   = 0;
            for (const char character : text) {
                if (character >= '0' && character <= '9') {
                    value = value * 10U + static_cast<unsigned>(character - '0');
                    ++digits;
                    if (digits > 3 || value > 255U) {
                        return std::nullopt;
                    }
                } else if (character == '.') {
                    if (digits == 0 || index == size - 1) {
                        return std::nullopt;
                    }
                    bytes[index++] = std::byte{static_cast<std::uint8_t>(value)};
                    value          = 0;
                    digits         = 0;
                } else {
                    return std::nullopt;
                }
            }
            if (digits == 0 || index != size - 1) {
                return std::nullopt;
            }
            bytes[index] = std::byte{static_cast<std::uint8_t>(value)};
            return Ipv4Address{bytes};
        }

        [[nodiscard]] constexpr const Bytes& bytes() const noexcept {
            return bytes_;
        }

        /// As a host-order value: 192.168.0.1 is 0xC0A80001.
        [[nodiscard]] constexpr std::uint32_t to_uint32() const noexcept {
            return (std::to_integer<std::uint32_t>(bytes_[0]) << 24U) |
                   (std::to_integer<std::uint32_t>(bytes_[1]) << 16U) |
                   (std::to_integer<std::uint32_t>(bytes_[2]) << 8U) | std::to_integer<std::uint32_t>(bytes_[3]);
        }

        [[nodiscard]] std::string to_string() const {
            return std::format("{}.{}.{}.{}",
                               std::to_integer<unsigned>(bytes_[0]),
                               std::to_integer<unsigned>(bytes_[1]),
                               std::to_integer<unsigned>(bytes_[2]),
                               std::to_integer<unsigned>(bytes_[3]));
        }

        friend constexpr bool operator==(const Ipv4Address&, const Ipv4Address&) noexcept = default;

        friend constexpr std::strong_ordering operator<=>(const Ipv4Address&, const Ipv4Address&) noexcept = default;

        friend std::ostream& operator<<(std::ostream& stream, const Ipv4Address& address) {
            return stream << address.to_string();
        }

    private:
        Bytes bytes_{};
    };
}  // namespace aloe::device

template <>
struct std::formatter<aloe::device::MacAddress> : std::formatter<std::string_view> {
    auto format(const aloe::device::MacAddress& address, auto& context) const {
        return std::formatter<std::string_view>::format(address.to_string(), context);
    }
};

template <>
struct std::formatter<aloe::device::Ipv4Address> : std::formatter<std::string_view> {
    auto format(const aloe::device::Ipv4Address& address, auto& context) const {
        return std::formatter<std::string_view>::format(address.to_string(), context);
    }
};
