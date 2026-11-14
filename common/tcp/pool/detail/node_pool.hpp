#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace aloe::tcp::detail {

    /// The index that is no node: the end of a chain, an empty free list.
    inline constexpr std::uint32_t no_node = std::numeric_limits<std::uint32_t>::max();

    /// A node a `NodePool` can hold: blank when value-initialised, with the index of the next node in its chain.
    template <typename N>
    concept IsPoolNode = std::default_initializable<N> && std::movable<N> && requires(N& node) {
        { node.next } -> std::same_as<std::uint32_t&>;
    };

    /// A singly linked chain of one pool's nodes, oldest first: its two ends and its length. The links live in the
    /// nodes; the pool's chain members keep the three in step with them.
    struct NodeChain {
        std::uint32_t head  = no_node;
        std::uint32_t tail  = no_node;
        std::uint32_t count = 0;

        [[nodiscard]] constexpr bool empty() const noexcept {
            return head == no_node;
        }
    };

    /**
     * @brief A fixed array of nodes with an intrusive free list: the storage behind the per-shard pools.
     *
     * Fixed at construction, never grown. A node's `next` links the free list while the node is free and its chain
     * while it is taken. `acquire` hands out a blank node; `release` blanks it again, so whatever it held, a packet
     * among them, goes back now. A free node may be set aside (`reserve`): `acquire` leaves it alone and only
     * `acquire_reserved` takes it, so an owner can count on a node whatever the others hold.
     */
    template <IsPoolNode N>
    class NodePool {
    public:
        using Node = N;

        /// Throws std::invalid_argument for zero nodes or more than the index can name, before it allocates.
        explicit NodePool(const std::size_t count) {
            if (count == 0 || count >= no_node) {
                throw std::invalid_argument{"NodePool needs between one node and 2^32 - 2"};
            }
            nodes_.resize(count);
            for (std::size_t index = 0; index + 1 < count; ++index) {
                nodes_[index].next = static_cast<std::uint32_t>(index + 1);
            }
            free_head_ = 0;
            available_ = count;
        }

        /// A blank node, or nothing when every node is taken or set aside.
        [[nodiscard]] std::optional<std::uint32_t> acquire() noexcept {
            if (available_ == reserved_) {
                return std::nullopt;
            }
            return take();
        }

        /// Sets one free node aside: `acquire` cannot take it, `acquire_reserved` can. False when none is free to set
        /// aside.
        [[nodiscard]] bool reserve() noexcept {
            if (available_ == reserved_) {
                return false;
            }
            ++reserved_;
            return true;
        }

        /// A reservation given back unused: the node is any node's again.
        void unreserve() noexcept {
            assert(reserved_ > 0 && "an unreserve without a reservation");
            --reserved_;
        }

        /// A blank node from the ones set aside, spending one reservation; nothing when none is set aside.
        [[nodiscard]] std::optional<std::uint32_t> acquire_reserved() noexcept {
            if (reserved_ == 0) {
                return std::nullopt;
            }
            --reserved_;
            return take();
        }

        /// Blanks the node, which drops what it held, and puts it back on the free list.
        void release(const std::uint32_t index) noexcept {
            Node& node = nodes_[index];
            node       = Node{};
            node.next  = free_head_;
            free_head_ = index;
            ++available_;
        }

        /// Links the taken node `index` at the tail of `chain`.
        void push_back(NodeChain& chain, const std::uint32_t index) noexcept {
            assert(nodes_[index].next == no_node && "a node joins a chain unlinked");
            if (chain.tail == no_node) {
                chain.head = index;
            } else {
                nodes_[chain.tail].next = index;
            }
            chain.tail = index;
            ++chain.count;
        }

        /// Links the taken node `index` after `previous` in `chain`; `no_node` links it at the head.
        void insert_after(NodeChain& chain, const std::uint32_t previous, const std::uint32_t index) noexcept {
            assert(nodes_[index].next == no_node && "a node joins a chain unlinked");
            if (previous == no_node) {
                nodes_[index].next = chain.head;
                chain.head         = index;
            } else {
                nodes_[index].next    = nodes_[previous].next;
                nodes_[previous].next = index;
            }
            if (chain.tail == previous) {
                chain.tail = index;
            }
            ++chain.count;
        }

        /// Unlinks the node after `previous`, the head for `no_node`, and returns it: still taken, linked to nothing.
        [[nodiscard]] std::uint32_t unlink_after(NodeChain& chain, const std::uint32_t previous) noexcept {
            const std::uint32_t index = previous == no_node ? chain.head : nodes_[previous].next;
            assert(index != no_node && "an unlink past the tail");
            if (previous == no_node) {
                chain.head = nodes_[index].next;
            } else {
                nodes_[previous].next = nodes_[index].next;
            }
            if (chain.tail == index) {
                chain.tail = previous;
            }
            nodes_[index].next = no_node;
            --chain.count;
            return index;
        }

        /// Links every node of `run` behind `chain`'s tail, in order, and leaves `run` empty.
        void splice_back(NodeChain& chain, NodeChain& run) noexcept {
            if (run.empty()) {
                return;
            }
            if (chain.tail == no_node) {
                chain.head = run.head;
            } else {
                nodes_[chain.tail].next = run.head;
            }
            chain.tail   = run.tail;
            chain.count += run.count;
            run          = NodeChain{};
        }

        /// Releases the head of a chain that is not empty.
        void release_front(NodeChain& chain) noexcept {
            assert(!chain.empty() && "a release from an empty chain");
            const std::uint32_t head = chain.head;
            chain.head               = nodes_[head].next;
            if (chain.head == no_node) {
                chain.tail = no_node;
            }
            --chain.count;
            release(head);
        }

        /// Releases the tail of a chain that is not empty; `previous` is the node before it, `no_node` when the tail
        /// is the only node. The chain is singly linked, so the caller remembers it.
        void release_back(NodeChain& chain, const std::uint32_t previous) noexcept {
            assert(!chain.empty() && "a release from an empty chain");
            assert((previous == no_node ? chain.head == chain.tail : nodes_[previous].next == chain.tail) &&
                   "previous is the node before the tail");
            const std::uint32_t tail = chain.tail;
            chain.tail               = previous;
            if (previous == no_node) {
                chain.head = no_node;
            } else {
                nodes_[previous].next = no_node;
            }
            --chain.count;
            release(tail);
        }

        /// Releases every node of `chain`, head first, and empties it.
        void release_all(NodeChain& chain) noexcept {
            for (std::uint32_t index = chain.head; index != no_node;) {
                const std::uint32_t next = nodes_[index].next;
                release(index);
                index = next;
            }
            chain = NodeChain{};
        }

        [[nodiscard]] Node& node(const std::uint32_t index) noexcept {
            return nodes_[index];
        }

        [[nodiscard]] const Node& node(const std::uint32_t index) const noexcept {
            return nodes_[index];
        }

        /// The nodes an `acquire` may take: the free ones less those set aside.
        [[nodiscard]] std::size_t available() const noexcept {
            return available_ - reserved_;
        }

        [[nodiscard]] std::size_t reserved() const noexcept {
            return reserved_;
        }

        [[nodiscard]] std::size_t capacity() const noexcept {
            return nodes_.size();
        }

    private:
        /// The free head, which exists while `available_ > 0`.
        [[nodiscard]] std::uint32_t take() noexcept {
            assert(free_head_ != no_node && "a take from an empty pool");
            const std::uint32_t index = free_head_;
            Node& node                = nodes_[index];
            free_head_                = node.next;
            node.next                 = no_node;
            --available_;
            return index;
        }

        std::vector<Node> nodes_;
        std::uint32_t free_head_ = no_node;
        std::size_t available_   = 0;  ///< Free nodes, the ones set aside included.
        std::size_t reserved_    = 0;  ///< Free nodes set aside; never more than `available_`.
    };

}  // namespace aloe::tcp::detail
