#pragma once

#include <aloe/core>
#include <aloe/wire>
#include <array>
#include <cstddef>
#include <cstdint>

namespace aloe::net {

    /// The link and network headers of a frame to one destination, built once and patched per datagram by
    /// `Ipv4::send_frame`.
    struct Prebuilt {
        static constexpr std::size_t size = wire::EthernetHeader::size + wire::Ipv4Header::size;  // 34

        std::array<std::byte, size> headers{};  ///< Ethernet then IPv4; total length, identification and checksum zero.
        core::TimePoint valid_until{};          ///< Rebuild at or after this stamp: the entry wants a refresh.
        std::uint64_t generation = 0;           ///< The brick's generation when built; rebuild when it moved.

        friend constexpr bool operator==(const Prebuilt&, const Prebuilt&) noexcept = default;
    };

}  // namespace aloe::net
