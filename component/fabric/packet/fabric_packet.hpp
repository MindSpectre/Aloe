#pragma once

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
        };

    }  // namespace detail

    /**
     * @brief The fabric's packet: a move-only handle over one Block of a Pool.
     *
     * Models IsPacket. The block is plain memory, so ASan sees every byte. Destroying the packet, or
     * assigning over it, returns the block to its pool, which must still be alive.
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
            tx_ = tx;
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
