#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include <address.hpp>
#include <bytes.hpp>
#include <protocol.hpp>

namespace aloe {

    /// Which fields of an IPv4 packet the hash covers.
    struct RssHashTypes {
        bool ipv4     = false;  ///< Addresses only, for IPv4 packets no other type covers.
        bool ipv4_tcp = false;  ///< Addresses and ports of unfragmented TCP.
        bool ipv4_udp = false;  ///< Addresses and ports of unfragmented UDP.

        friend constexpr bool operator==(const RssHashTypes&, const RssHashTypes&) noexcept = default;
    };

    inline constexpr std::size_t rss_key_capacity = 52;

    /**
     * @brief The receive-side scaling a device has in effect.
     *
     * Every field describes what is programmed, not what was requested: a card may truncate the key
     * or use its own table size. `flow_hash` and `queue_for` evaluate it, so anyone can predict
     * which queue a flow lands on.
     */
    struct RssDescription {
        bool enabled = false;
        std::array<std::byte, rss_key_capacity> key{};
        std::uint8_t key_length = 0;  ///< Bytes of `key` in use, 40 or 52 on real cards.
        RssHashTypes types{};
        std::vector<std::uint16_t> table;  ///< Indirection table; a power-of-two size on real cards.
    };

    /// The IPv4 fields a hash is computed over.
    struct FlowTuple {
        Ipv4Address source;
        Ipv4Address destination;
        std::uint16_t source_port      = 0;
        std::uint16_t destination_port = 0;
        std::optional<Ipv4Protocol> protocol =
            std::nullopt;  ///< Absent for a fragment: no protocol or ports take part.
    };

    /**
     * @brief Aloe's fixed RSS key.
     *
     * The first 40 bytes are Microsoft's published default key, so the RSS verification vectors
     * apply. An IPv4 4-tuple is 12 bytes and Toeplitz reads at most 16 bytes of key for it, so a
     * card that takes 40 bytes and one that takes 52 compute the same hash.
     */
    inline constexpr std::array<std::byte, rss_key_capacity> aloe_rss_key = {
        std::byte{0x6d}, std::byte{0x5a}, std::byte{0x56}, std::byte{0xda}, std::byte{0x25}, std::byte{0x5b},
        std::byte{0x0e}, std::byte{0xc2}, std::byte{0x41}, std::byte{0x67}, std::byte{0x25}, std::byte{0x3d},
        std::byte{0x43}, std::byte{0xa3}, std::byte{0x8f}, std::byte{0xb0}, std::byte{0xd0}, std::byte{0xca},
        std::byte{0x2b}, std::byte{0xcb}, std::byte{0xae}, std::byte{0x7b}, std::byte{0x30}, std::byte{0xb4},
        std::byte{0x77}, std::byte{0xcb}, std::byte{0x2d}, std::byte{0xa3}, std::byte{0x80}, std::byte{0x30},
        std::byte{0xf2}, std::byte{0x0c}, std::byte{0x6a}, std::byte{0x42}, std::byte{0xb7}, std::byte{0x3b},
        std::byte{0xbe}, std::byte{0xac}, std::byte{0x01}, std::byte{0xfa}, std::byte{0x6d}, std::byte{0x5a},
        std::byte{0x56}, std::byte{0xda}, std::byte{0x25}, std::byte{0x5b}, std::byte{0x0e}, std::byte{0xc2},
        std::byte{0x41}, std::byte{0x67}, std::byte{0x25}, std::byte{0x3d},
    };

    /**
     * @brief The Toeplitz hash of `input` under `key`, as network cards compute RSS.
     *
     * Bits are consumed most significant first. The key must be at least four bytes longer than the
     * input; missing key bits count as zero.
     */
    [[nodiscard]] constexpr std::uint32_t toeplitz_hash(const std::span<const std::byte> key,
                                                        const std::span<const std::byte> input) noexcept {
        assert(key.size() >= 4);
        std::uint32_t result   = 0;
        std::uint32_t window   = load_be32(key.first(4));
        std::size_t next_bit   = 32;
        const std::size_t bits = key.size() * 8;
        for (const std::byte in : input) {
            for (int bit = 7; bit >= 0; --bit) {
                if (((std::to_integer<unsigned>(in) >> static_cast<unsigned>(bit)) & 1U) != 0) {
                    result ^= window;
                }
                unsigned next = 0;
                if (next_bit < bits) {
                    next = (std::to_integer<unsigned>(key[next_bit / 8]) >> (7U - next_bit % 8)) & 1U;
                }
                window = (window << 1U) | next;
                ++next_bit;
            }
        }
        return result;
    }

    /**
     * @brief The RSS hash of a flow under a description.
     *
     * The 4-tuple is hashed when the protocol is TCP or UDP and the matching type is enabled, else the
     * 2-tuple when `ipv4` is enabled, else the hash is zero. Fields are fed in network byte order:
     * source address, destination address, source port, destination port.
     */
    [[nodiscard]] constexpr std::uint32_t flow_hash(const RssDescription& rss, const FlowTuple& flow) noexcept {
        if (!rss.enabled) {
            return 0;
        }
        std::array<std::byte, 12> input{};
        for (std::size_t index = 0; index < Ipv4Address::size; ++index) {
            input[index]     = flow.source.bytes()[index];
            input[4 + index] = flow.destination.bytes()[index];
        }
        const std::span<const std::byte> key{rss.key.data(), std::min<std::size_t>(rss.key_length, rss_key_capacity)};
        const bool with_ports = (flow.protocol == Ipv4Protocol::Tcp && rss.types.ipv4_tcp) ||
                                (flow.protocol == Ipv4Protocol::Udp && rss.types.ipv4_udp);
        if (with_ports) {
            store_be16(std::span<std::byte>{input}.subspan(8, 2), flow.source_port);
            store_be16(std::span<std::byte>{input}.subspan(10, 2), flow.destination_port);
            return toeplitz_hash(key, input);
        }
        if (rss.types.ipv4) {
            return toeplitz_hash(key, std::span<const std::byte>{input}.first(8));
        }
        return 0;
    }

    /**
     * @brief The queue a flow lands on: the table entry its hash selects, or 0 when steering is off.
     *
     * The hash is masked with the table size less one, as cards index their table; the size is a
     * power of two on real cards.
     */
    [[nodiscard]] constexpr std::uint16_t queue_for(const RssDescription& rss, const FlowTuple& flow) noexcept {
        if (!rss.enabled || rss.table.empty()) {
            return 0;
        }
        return rss.table[flow_hash(rss, flow) & (rss.table.size() - 1)];
    }

    /**
     * @brief A description that spreads `queues` round robin over a table, under Aloe's key.
     *
     * The fabric programs its ports with this, and ethdev assumes it for a card that accepts RSS
     * but cannot report its table. Throws std::invalid_argument for zero queues or a key length
     * outside 4 to 52 bytes.
     */
    [[nodiscard]] inline RssDescription round_robin_rss(const std::uint16_t queues,
                                                        const std::uint16_t table_size = 128,
                                                        const std::uint8_t key_length  = rss_key_capacity) {
        if (queues == 0) {
            throw std::invalid_argument{"round_robin_rss needs at least one queue"};
        }
        if (key_length < 4 || key_length > rss_key_capacity) {
            throw std::invalid_argument{"round_robin_rss needs a key of 4 to 52 bytes"};
        }
        RssDescription rss;
        rss.enabled    = true;
        rss.key        = aloe_rss_key;
        rss.key_length = key_length;
        rss.types      = RssHashTypes{.ipv4 = true, .ipv4_tcp = true, .ipv4_udp = true};
        rss.table.resize(table_size);
        for (std::size_t index = 0; index < rss.table.size(); ++index) {
            rss.table[index] = static_cast<std::uint16_t>(index % queues);
        }
        return rss;
    }

}  // namespace aloe
