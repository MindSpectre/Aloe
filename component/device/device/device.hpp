#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <address.hpp>
#include <packet.hpp>
#include <rss.hpp>

namespace aloe {

    /**
     * @brief What a device can do, fixed after construction.
     *
     * Anything absent gets its software fallback in the stack above, never inside the device.
     */
    struct Capabilities {
        std::uint16_t max_rx_queues  = 0;
        std::uint16_t max_tx_queues  = 0;
        std::uint16_t min_mtu        = 0;
        std::uint16_t max_mtu        = 0;
        bool rss                     = false;  ///< Hashes and steers by its RssDescription.
        std::uint8_t rss_key_size    = 0;      ///< Bytes of key the card takes.
        std::uint16_t rss_table_size = 0;      ///< Entries in the indirection table.
        RssHashTypes rss_types{};              ///< Which fields the card can hash.
        bool rx_ipv4_checksum = false;         ///< Verifies IPv4 header checksums on receive.
        bool rx_l4_checksum   = false;         ///< Verifies TCP and UDP checksums on receive.
        bool tx_ipv4_checksum = false;         ///< Fills IPv4 header checksums on transmit.
        bool tx_l4_checksum   = false;         ///< Completes TCP and UDP checksums on transmit.

        friend constexpr bool operator==(const Capabilities&, const Capabilities&) noexcept = default;
    };

    /// Per-queue counters, all monotonic.
    struct QueueCounters {
        std::uint64_t received    = 0;
        std::uint64_t transmitted = 0;
        std::uint64_t dropped     = 0;  ///< Arrived with no room in the receive queue.
        std::uint64_t oversized   = 0;  ///< Longer than the MTU allows, or shorter than a header.

        friend constexpr bool operator==(const QueueCounters&, const QueueCounters&) noexcept = default;
    };

    /**
     * @brief One port with N queues, queue i belonging to shard i.
     *
     * Contracts every backend keeps and every caller honours:
     * - Queue i is used from one thread at a time. A packet is allocated, transmitted and freed on the
     *   queue it came from; a received packet is freed on the queue it arrived on.
     * - A packet's data is a whole Ethernet frame from the destination address to the end of the
     *   payload, without the frame check sequence.
     * - Frames longer than `mtu() + 14`, or shorter than an Ethernet header, are never transmitted by
     *   the stack. A backend that is handed one drops it and counts it as oversized: on the
     *   transmitting queue when the transmitter refuses to send it, on the receiving queue when the
     *   receiver drops it. `transmit` counts such a packet as accepted.
     * - `receive` fills `out` from the front and returns how many; the slots it fills must hold empty
     *   packets. `transmit` takes packets from the front of `in` and returns how many it accepted.
     *   Accepted packets are moved from; the rest stay untouched with the caller, who retries or
     *   drops them. Empty slots are skipped and count as accepted.
     * - The hot-path operations never throw and never allocate anything but packets. Construction
     *   and destruction are cold paths.
     */
    template <typename D>
    concept IsDevice =
        requires(D device, const D const_device, std::uint16_t queue, std::span<typename D::Packet> packets) {
            typename D::Packet;
            requires IsPacket<typename D::Packet>;
            { const_device.queue_count() } -> std::same_as<std::uint16_t>;
            { const_device.mac() } -> std::same_as<MacAddress>;
            { const_device.mtu() } -> std::same_as<std::uint16_t>;
            { const_device.link_up() } -> std::same_as<bool>;
            { const_device.capabilities() } -> std::same_as<const Capabilities&>;
            { const_device.steering() } -> std::same_as<const RssDescription&>;
            { device.allocate(queue) } -> std::same_as<std::optional<typename D::Packet>>;
            { device.receive(queue, packets) } -> std::same_as<std::size_t>;
            { device.transmit(queue, packets) } -> std::same_as<std::size_t>;
            { const_device.counters(queue) } -> std::same_as<QueueCounters>;
        };

    /// Ethernet header length, the `14` in `mtu() + 14`.
    inline constexpr std::size_t ethernet_header_size = 14;

}  // namespace aloe
