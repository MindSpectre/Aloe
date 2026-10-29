#pragma once

#include <aloe/core>
#include <cstdint>

#include <prebuilt.hpp>

namespace aloe::tcp::detail {

    /**
     * @brief A connection's link and network headers from `Ipv4::prebuild`, with when they stop being valid.
     *
     * The TCP header is not part of it: it is written per segment by `TcpHeader::write`, which costs the same as a copy
     * and a patch and keeps one path for every segment kind; the option block per kind is the stack's.
     */
    struct FrameTemplate {
        net::Prebuilt headers{};
        bool built = false;

        /// Rebuild before the next send: never built, past its validity, or the IP brick's generation moved.
        [[nodiscard]] constexpr bool stale(const core::TimePoint now, const std::uint64_t generation) const noexcept {
            return !built || now >= headers.valid_until || generation != headers.generation;
        }
    };

}  // namespace aloe::tcp::detail
