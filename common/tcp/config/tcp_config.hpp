#pragma once

#include <aloe/core>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

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
        TableFull,    ///< No free connection slot, or no retransmission-queue node for the SYN.
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
        std::size_t connections      = 1024;  ///< Slots per shard.
        std::size_t listeners        = 8;
        std::size_t receive_segments = 32;  ///< Packets one connection may hold, in order and out of order together;
                                            ///< with `mss()`, the default window.
        std::uint32_t receive_window =
            0;  ///< The byte window when set, capped at what the slots hold; zero is half of what they hold.
        std::size_t receive_pool      = 2048;  ///< Shared retained-packet nodes per shard.
        /// Hard cap on queued data segments per connection: the send cap. The FIN may take one entry beyond it.
        std::size_t send_segments     = 32;
        std::size_t send_pool         = 2048;   ///< Shared retransmission-queue nodes per shard.
        std::uint16_t ephemeral_first = 32768;  ///< `connect`'s local ports, inclusive.
        std::uint16_t ephemeral_last  = 60999;
        core::Duration rto_initial    = std::chrono::seconds{1};  ///< The RTO before the first round-trip sample.
        core::Duration rto_min        = std::chrono::milliseconds{200};  ///< Linux's TCP_RTO_MIN.
        core::Duration rto_max        = std::chrono::seconds{120};       ///< Linux's TCP_RTO_MAX.
        std::uint8_t retries          = 5;  ///< SYN and SYN-ACK retransmits before TimedOut.
        std::uint8_t data_retries =
            15;  ///< Data and FIN retransmits, and unanswered zero-window probes, before TimedOut.
        core::Duration user_timeout =
            core::Duration::zero();  ///< RFC 5482: unacknowledged this long ends the connection; zero is off.
        bool timestamps     = true;  ///< Offer and use RFC 7323 timestamps.
        bool sack           = true;  ///< Offer and use SACK-permitted: report what is held, and what arrived twice.
        bool window_scaling = true;  ///< Offer and use RFC 7323 window scaling.
        core::Duration delayed_ack      = core::Duration::zero();  ///< How long an ACK for in-order data may wait; zero
                                                                   ///< sends it at the flush of the same tick.
        std::uint8_t retransmit_defers  = 1;  ///< Deferred resends of one segment before the copy fallback.
        core::Duration unresolved_retry = std::chrono::milliseconds{10};  ///< A SYN waiting for ARP; not a try.
        std::uint16_t unresolved_polls  = 300;  ///< Polls before Unresolved: 3 s by default, Linux's ARP bound.

        /// Throws std::invalid_argument naming the first rule broken; `max_l4_size` is the IP brick's and `resolution`
        /// the timer wheel's. Returns the config, so a constructor validates it before sizing anything from it.
        [[nodiscard]] const TcpConfig& validate(const std::uint16_t max_l4_size,
                                                const core::Duration resolution) const {
            if (connections == 0 || listeners == 0 || receive_segments == 0 || receive_pool == 0) {
                throw std::invalid_argument{
                    "TcpConfig: connections, listeners, receive_segments and receive_pool must be positive"};
            }
            if (receive_segments > receive_pool) {
                throw std::invalid_argument{"TcpConfig: receive_segments must not exceed receive_pool"};
            }
            if (receive_window > (65535U << 14U)) {
                throw std::invalid_argument{"TcpConfig: receive_window must not exceed 65535 << 14"};
            }
            if (delayed_ack < core::Duration::zero() || delayed_ack > std::chrono::milliseconds{500} ||
                (delayed_ack > core::Duration::zero() && delayed_ack < resolution)) {
                throw std::invalid_argument{"TcpConfig: delayed_ack must be zero, or between the timer wheel's "
                                            "resolution and 500 ms"};
            }
            if (send_segments == 0 || send_pool == 0) {
                throw std::invalid_argument{"TcpConfig: send_segments and send_pool must be positive"};
            }
            if (connections > (std::size_t{1} << 30U)) {
                throw std::invalid_argument{"TcpConfig: too many connections for a 32-bit index"};
            }
            if (ephemeral_first == 0 || ephemeral_first > ephemeral_last) {
                throw std::invalid_argument{"TcpConfig: the ephemeral range must be non-empty and exclude port 0"};
            }
            if (rto_initial <= core::Duration::zero() || unresolved_retry <= core::Duration::zero()) {
                throw std::invalid_argument{"TcpConfig: rto_initial and unresolved_retry must be positive"};
            }
            if (rto_min <= core::Duration::zero() || rto_min < resolution) {
                throw std::invalid_argument{
                    "TcpConfig: rto_min must be positive and at least the timer wheel's resolution"};
            }
            if (rto_min > rto_initial || rto_initial > rto_max) {
                throw std::invalid_argument{"TcpConfig: rto_min <= rto_initial <= rto_max must hold"};
            }
            if (rto_max.count() > core::Duration::max().count() / 4) {
                throw std::invalid_argument{"TcpConfig: rto_max must fit a quarter of core::Duration"};
            }
            if (user_timeout < core::Duration::zero() || user_timeout > core::Duration::max() / 4) {
                throw std::invalid_argument{
                    "TcpConfig: user_timeout must not be negative and must fit a quarter of core::Duration"};
            }
            if (retries == 0 || data_retries == 0) {
                throw std::invalid_argument{"TcpConfig: retries and data_retries must be positive"};
            }
            if (data_retries >= 63) {
                throw std::invalid_argument{"TcpConfig: data_retries must be below 63"};
            }
            if (unresolved_polls == 0) {
                throw std::invalid_argument{"TcpConfig: unresolved_polls must be positive"};
            }
            // Bounds the longest handshake delay, rto_initial * 2^retries, and the whole schedule, rto_initial *
            // (2^(retries+1) - 1), to a quarter of core::Duration's range, so no deadline now + delay can overflow;
            // the doubling itself stops at rto_max.
            if (retries >= 63 || rto_initial.count() > (core::Duration::max().count() / 4) >> retries) {
                throw std::invalid_argument{"TcpConfig: rto_initial doubled retries times must fit core::Duration"};
            }
            if (max_l4_size <= wire::TcpHeader::size) {
                throw std::invalid_argument{"TcpConfig: the MTU leaves no room for a TCP segment"};
            }
            return *this;
        }
    };

}  // namespace aloe::tcp
