#pragma once

#include <aloe/core>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include <ipv4_address.hpp>

namespace aloe::tcp {

    struct Endpoint {
        wire::Ipv4Address address{};
        std::uint16_t port = 0;

        friend constexpr bool operator==(const Endpoint&, const Endpoint&) noexcept = default;
    };

    /// The RFC 793 states minimal TCP has; no TIME_WAIT until phase 2.
    enum class State : std::uint8_t {
        Closed,
        SynSent,
        SynReceived,
        Established,
        FinWait1,
        FinWait2,
        CloseWait,
        Closing,
        LastAck,
    };

    enum class ConnectError : std::uint8_t {
        TableFull,    ///< No free connection slot.
        NoPort,       ///< The ephemeral range has no free port that lands on this queue.
        Unplaceable,  ///< The card hashes addresses only, or steers nothing, and this is not the queue it picks.
        NoRoute,      ///< Off the subnet with no gateway.
    };

    enum class ListenError : std::uint8_t {
        InUse,      ///< Already listening on that port.
        TableFull,  ///< Every listener slot is taken.
    };

    /// The peer's MSS when its SYN carried no option: RFC 1122's default.
    inline constexpr std::uint16_t default_peer_mss = 536;

    struct TcpConfig {
        std::size_t connections         = 1024;  ///< Slots per shard.
        std::size_t listeners           = 8;
        std::size_t receive_segments    = 32;  ///< Hard cap on held packets per connection; sizes the byte budget too.
        std::size_t receive_pool        = 2048;   ///< Shared retained-packet nodes per shard.
        std::uint16_t ephemeral_first   = 32768;  ///< `connect`'s local ports, inclusive.
        std::uint16_t ephemeral_last    = 60999;
        core::Duration retry_initial    = std::chrono::seconds{1};        ///< SYN, SYN-ACK, FIN: doubles per try.
        std::uint8_t retries            = 5;                              ///< Retransmissions before TimedOut.
        core::Duration unresolved_retry = std::chrono::milliseconds{10};  ///< A SYN waiting for ARP; not a try.
    };

}  // namespace aloe::tcp
