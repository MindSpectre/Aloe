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

#include <mac_address.hpp>

namespace aloe::wire {

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

        /// Reads an address from the first `size` bytes, as the wire carries it.
        [[nodiscard]] static constexpr Ipv4Address load(const std::span<const std::byte> bytes) noexcept {
            assert(bytes.size() >= size);
            Bytes raw{};
            std::ranges::copy(bytes.first(size), raw.begin());
            return Ipv4Address{raw};
        }

        /// Writes the address into the first `size` bytes of `out`.
        constexpr void store(const std::span<std::byte> out) const noexcept {
            assert(out.size() >= size);
            std::ranges::copy(bytes_, out.begin());
        }

        /// As a host-order value: 192.168.0.1 is 0xC0A80001.
        [[nodiscard]] constexpr std::uint32_t to_uint32() const noexcept {
            return (std::to_integer<std::uint32_t>(bytes_[0]) << 24U) |
                   (std::to_integer<std::uint32_t>(bytes_[1]) << 16U) |
                   (std::to_integer<std::uint32_t>(bytes_[2]) << 8U) | std::to_integer<std::uint32_t>(bytes_[3]);
        }

        /// 255.255.255.255: every host on the link, never routed.
        [[nodiscard]] static constexpr Ipv4Address limited_broadcast() noexcept {
            return {255, 255, 255, 255};
        }

        /// 0.0.0.0: no address yet, as a source; never a destination.
        [[nodiscard]] constexpr bool is_unspecified() const noexcept {
            return *this == Ipv4Address{};
        }

        [[nodiscard]] constexpr bool is_limited_broadcast() const noexcept {
            return *this == limited_broadcast();
        }

        /// 224.0.0.0/4, the group addresses.
        [[nodiscard]] constexpr bool is_multicast() const noexcept {
            return (std::to_integer<unsigned>(bytes_[0]) & 0xf0U) == 0xe0U;
        }

        /// 127.0.0.0/8, which never leaves a host.
        [[nodiscard]] constexpr bool is_loopback() const noexcept {
            return std::to_integer<unsigned>(bytes_[0]) == 127U;
        }

        /// RFC 1112: `01:00:5e`, then the low 23 bits of the group address.
        [[nodiscard]] constexpr MacAddress multicast_mac() const noexcept {
            return {0x01,
                    0x00,
                    0x5e,
                    static_cast<std::uint8_t>(std::to_integer<unsigned>(bytes_[1]) & 0x7fU),
                    std::to_integer<std::uint8_t>(bytes_[2]),
                    std::to_integer<std::uint8_t>(bytes_[3])};
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

}  // namespace aloe::wire

template <>
struct std::formatter<aloe::wire::Ipv4Address> : std::formatter<std::string_view> {
    auto format(const aloe::wire::Ipv4Address& address, auto& context) const {
        return std::formatter<std::string_view>::format(address.to_string(), context);
    }
};
