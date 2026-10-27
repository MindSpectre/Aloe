#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <packet.hpp>

namespace aloe::fabric {

    class Pool;
    class Port;

    namespace detail {

        /// One buffer of a Pool: `packet_headroom` plus the pool's data capacity, on its own heap allocation.
        struct Block {
            std::unique_ptr<std::byte[]> storage;
            std::size_t capacity = 0;
            bool lent            = false;  ///< A handle from lend() is alive.
            bool orphaned        = false;  ///< The lender is gone; the handle gives the block back.
        };

    }  // namespace detail

    /**
     * @brief The fabric's packet: a move-only handle over one Block of a Pool.
     *
     * Models IsPacket. The block is plain memory, so ASan sees every byte. Destroying the packet, or
     * assigning over it, returns the block to its pool, which must still be alive. A packet can lend a second
     * handle to the port and asks `lent()` whether the port is done; a lender dropped while lent orphans its
     * block, which goes back to the pool with the handle.
     */
    class Packet {
    public:
        Packet() noexcept = default;
        Packet(Packet&& other) noexcept;
        Packet& operator=(Packet&& other) noexcept;
        Packet(const Packet&)            = delete;
        Packet& operator=(const Packet&) = delete;
        ~Packet();

        [[nodiscard]] bool empty() const noexcept {
            return block_ == nullptr;
        }

        [[nodiscard]] std::span<std::byte> data() noexcept;
        [[nodiscard]] std::span<const std::byte> data() const noexcept;

        [[nodiscard]] std::size_t size() const noexcept {
            return end_ - begin_;
        }

        [[nodiscard]] std::size_t headroom() const noexcept {
            return begin_;
        }

        [[nodiscard]] std::size_t tailroom() const noexcept;

        /// Headroom, data and tailroom together.
        [[nodiscard]] std::size_t capacity() const noexcept;

        [[nodiscard]] std::optional<std::span<std::byte>> prepend(std::size_t count) noexcept;
        [[nodiscard]] std::optional<std::span<std::byte>> append(std::size_t count) noexcept;
        void trim_front(std::size_t count) noexcept;
        void trim_back(std::size_t count) noexcept;

        [[nodiscard]] device::RxMetadata rx() const noexcept {
            return rx_;
        }

        [[nodiscard]] device::TxMetadata tx() const noexcept {
            return tx_;
        }

        void set_tx(const device::TxMetadata& tx) noexcept {
            assert(!lent() && "a lent packet is not written");
            tx_ = tx;
        }

        /// A second handle over the same block, for the port to transmit: the loan. One at a time; not empty.
        [[nodiscard]] Packet lend() noexcept;
        /// True while a handle from `lend()` is alive: in a ring, or in the port's in-flight list.
        [[nodiscard]] bool lent() const noexcept {
            return block_ != nullptr && block_->lent;
        }
        /// True on the handle `lend()` returned, false on the lender.
        [[nodiscard]] bool borrowed() const noexcept {
            return borrowed_;
        }

    private:
        friend class Pool;
        friend class Port;

        Packet(Pool& pool, detail::Block& block) noexcept;

        void set_rx(const device::RxMetadata& rx) noexcept {
            rx_ = rx;
        }

        void release() noexcept;

        Pool* pool_           = nullptr;
        detail::Block* block_ = nullptr;
        std::size_t begin_    = 0;
        std::size_t end_      = 0;
        device::RxMetadata rx_;
        device::TxMetadata tx_;
        bool borrowed_ = false;
    };

    /**
     * @brief A fixed set of blocks that packets are allocated from and return to.
     *
     * One pool belongs to one queue and is touched only from that queue's thread. Every packet
     * allocated from a pool must be destroyed before the pool.
     */
    class Pool {
    public:
        /// `count` blocks, each with `packet_headroom` bytes of headroom and `data_capacity` bytes of room.
        Pool(std::size_t count, std::size_t data_capacity);
        Pool(const Pool&)            = delete;
        Pool& operator=(const Pool&) = delete;
        Pool(Pool&&)                 = delete;
        Pool& operator=(Pool&&)      = delete;
        ~Pool();

        /// An empty packet with full headroom, or nothing when every block is out.
        [[nodiscard]] std::optional<Packet> allocate() noexcept;

        [[nodiscard]] std::size_t available() const noexcept {
            return free_.size();
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return blocks_.size();
        }

        [[nodiscard]] std::size_t data_capacity() const noexcept {
            return data_capacity_;
        }

    private:
        friend class Packet;

        void give_back(detail::Block& block) noexcept;

        std::size_t data_capacity_;
        std::vector<detail::Block> blocks_;
        std::vector<detail::Block*> free_;
    };

}  // namespace aloe::fabric
