#pragma once

#include <aloe/core>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include <packet.hpp>
#include <tcp_header.hpp>
#include <tcp_node_pool.hpp>
#include <tcp_sequence.hpp>

namespace aloe::tcp::detail {

    /**
     * @brief The per-shard pool of retransmission-queue nodes; a connection chains its sent segments on them, oldest
     * first.
     *
     * A node is one segment that occupies sequence space, SYN, SYN-ACK, data or FIN, from its first transmission until
     * an ACK covers it: the packet with the whole frame as it last left, and what a resend and a rate sample read.
     * Fixed at construction, never grown. Every node may keep a packet of the device's pool, and a second one while
     * the copy fallback has one out, so that pool covers the receive descriptors, `receive_pool` held packets,
     * `send_pool` kept packets and the copies out, the transmit ring, what the card holds and the open preparations.
     */
    template <device::IsPacket Packet>
    class SentSegmentPool {
    public:
        struct Node {
            Packet packet{};  ///< The frame as it last left, headers included; lent while the device reads it.
            Packet copy{};    ///< The copy fallback's second packet while both are out; empty otherwise.
            wire::TcpSequence first{};  ///< The segment's first sequence number.
            std::uint32_t length  = 0;  ///< Sequence space: payload plus SYN and FIN.
            std::uint16_t payload = 0;  ///< Payload bytes in the packet.
            wire::TcpFlags flags{};     ///< As the segment carries them.
            core::TimePoint sent{};     ///< The last transmission; the epoch while never transmitted.
            core::TimePoint first_sent{};
            wire::TcpSequence copy_first{};  ///< The copy's first sequence number while a copy is out.
            std::uint16_t copy_payload = 0;  ///< The copy's payload bytes: the part not acknowledged when it left.
            std::uint8_t deferrals     = 0;  ///< Resends deferred since the last one that left.
            core::TimePoint deferred_at{};   ///< The last deferral: a later resend at the same stamp is the same one.
            std::uint8_t transmits          = 0;
            bool framed                     = false;  ///< The packet holds the frame headers in front of the payload.
            std::uint64_t delivered_at_send = 0;
            bool app_limited                = false;
            std::uint32_t next              = no_node;
        };

        /// Throws std::invalid_argument for zero nodes or more than the index can name.
        explicit SentSegmentPool(const std::size_t count)
            : nodes_(validated(count)),
              available_{count} {
            for (std::size_t index = 0; index + 1 < count; ++index) {
                nodes_[index].next = static_cast<std::uint32_t>(index + 1);
            }
            free_head_ = 0;
        }

        /// A blank node, or nothing when every node is taken.
        [[nodiscard]] std::optional<std::uint32_t> acquire() noexcept {
            if (free_head_ == no_node) {
                return std::nullopt;
            }
            const std::uint32_t index = free_head_;
            Node& node                = nodes_[index];
            free_head_                = node.next;
            node.next                 = no_node;
            --available_;
            return index;
        }

        /// Drops both packets, which go back to their pool now or when the device returns them, and blanks the node.
        void release(const std::uint32_t index) noexcept {
            Node& node = nodes_[index];
            node       = Node{};
            node.next  = free_head_;
            free_head_ = index;
            ++available_;
        }

        [[nodiscard]] Node& node(const std::uint32_t index) noexcept {
            return nodes_[index];
        }

        [[nodiscard]] const Node& node(const std::uint32_t index) const noexcept {
            return nodes_[index];
        }

        [[nodiscard]] std::size_t available() const noexcept {
            return available_;
        }

        [[nodiscard]] std::size_t capacity() const noexcept {
            return nodes_.size();
        }

    private:
        [[nodiscard]] static std::size_t validated(const std::size_t count) {
            if (count == 0 || count >= no_node) {
                throw std::invalid_argument{"SentSegmentPool needs between one node and 2^32 - 2"};
            }
            return count;
        }

        std::vector<Node> nodes_;
        std::uint32_t free_head_ = no_node;
        std::size_t available_   = 0;
    };

}  // namespace aloe::tcp::detail
