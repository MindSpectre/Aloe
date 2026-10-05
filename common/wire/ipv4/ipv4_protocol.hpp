#pragma once

#include <cstdint>

namespace aloe::wire {

    /**
     * @brief An IPv4 protocol number: the `protocol` byte of the header.
     *
     * Any byte is a valid value, since a frame carries whatever it carries; the named ones are the
     * protocols Aloe handles. Compare with `==` and convert with `std::to_underlying`.
     */
    enum class Ipv4Protocol : std::uint8_t {
        Icmp = 1,
        Tcp  = 6,
        Udp  = 17,
    };

}  // namespace aloe::wire
