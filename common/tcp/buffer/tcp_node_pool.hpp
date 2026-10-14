#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <packet.hpp>

namespace aloe::tcp::detail {

    /// The index that is no node: the end of a chain, an empty free list.
    inline constexpr std::uint32_t no_node = std::numeric_limits<std::uint32_t>::max();

    /**
     * @brief The per-shard pool of nodes a connection chains its held segments on.
     *
     * A node is a packet, the offset of its unconsumed payload, that payload's length and the next
     * node. Fixed at construction, never grown; `acquire` moves the packet in only when a node is
     * free, so a refused packet stays with the caller.
     */
    template <device::IsPacket Packet>
    class TcpNodePool {
    public:
        struct Node {
            Packet packet{};
            std::uint16_t offset = 0;  ///< Of the unconsumed payload inside the packet's data.
            std::uint16_t length = 0;  ///< Unconsumed payload bytes.
            std::uint32_t next   = no_node;
        };

        /// Throws std::invalid_argument for zero nodes or more than the index can name.
        explicit TcpNodePool(const std::size_t count)
            : nodes_(validated(count)),
              available_{count} {
            for (std::size_t index = 0; index + 1 < count; ++index) {
                nodes_[index].next = static_cast<std::uint32_t>(index + 1);
            }
            free_head_ = 0;
        }

        [[nodiscard]] std::optional<std::uint32_t>
        acquire(Packet&& packet, const std::uint16_t offset, const std::uint16_t length) noexcept {
            if (free_head_ == no_node) {
                return std::nullopt;
            }
            const std::uint32_t index = free_head_;
            Node& node                = nodes_[index];
            free_head_                = node.next;
            node.packet               = std::move(packet);
            node.offset               = offset;
            node.length               = length;
            node.next                 = no_node;
            --available_;
            return index;
        }

        /// Returns the packet to its pool and the node to the free list.
        void release(const std::uint32_t index) noexcept {
            Node& node  = nodes_[index];
            node.packet = Packet{};
            node.offset = 0;
            node.length = 0;
            node.next   = free_head_;
            free_head_  = index;
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
                throw std::invalid_argument{"TcpNodePool needs between one node and 2^32 - 2"};
            }
            return count;
        }

        std::vector<Node> nodes_;
        std::uint32_t free_head_ = no_node;
        std::size_t available_   = 0;
    };

}  // namespace aloe::tcp::detail
