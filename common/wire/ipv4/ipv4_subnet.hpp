#pragma once

#include <cassert>
#include <cstdint>

#include <ipv4_address.hpp>

namespace aloe::wire {

    /**
     * @brief An address with its prefix length: an interface's place on a subnet.
     *
     * The prefix is at most 32. A /31 (RFC 3021) and a /32 have no network or broadcast address of
     * their own; `has_broadcast` says whether this one does.
     */
    class Ipv4Subnet {
    public:
        static constexpr std::uint8_t max_prefix = 32;

        constexpr Ipv4Subnet(const Ipv4Address address, const std::uint8_t prefix) noexcept
            : address_{address},
              prefix_{prefix} {
            assert(prefix <= max_prefix && "an IPv4 prefix is at most 32 bits");
        }

        [[nodiscard]] constexpr Ipv4Address address() const noexcept {
            return address_;
        }

        [[nodiscard]] constexpr std::uint8_t prefix() const noexcept {
            return prefix_;
        }

        /// The prefix as a host-order mask: /24 is 0xFFFFFF00.
        [[nodiscard]] constexpr std::uint32_t mask() const noexcept {
            return prefix_ == 0 ? 0U : ~std::uint32_t{0} << (max_prefix - prefix_);
        }

        [[nodiscard]] constexpr bool contains(const Ipv4Address other) const noexcept {
            return ((other.to_uint32() ^ address_.to_uint32()) & mask()) == 0;
        }

        /// The all-zeros host: meaningful when `has_broadcast()`.
        [[nodiscard]] constexpr Ipv4Address network() const noexcept {
            return Ipv4Address::from_uint32(address_.to_uint32() & mask());
        }

        /// The all-ones host: meaningful when `has_broadcast()`.
        [[nodiscard]] constexpr Ipv4Address broadcast() const noexcept {
            return Ipv4Address::from_uint32(address_.to_uint32() | ~mask());
        }

        /// False for /31 and /32, where every address is a host.
        [[nodiscard]] constexpr bool has_broadcast() const noexcept {
            return prefix_ <= max_prefix - 2;
        }

        friend constexpr bool operator==(const Ipv4Subnet&, const Ipv4Subnet&) noexcept = default;

    private:
        Ipv4Address address_;
        std::uint8_t prefix_;
    };

}  // namespace aloe::wire
