#include <algorithm>
#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include <fabric_packet.hpp>

namespace aloe::fabric {

    Packet::Packet(Pool& pool, detail::Block& block) noexcept
        : pool_{&pool},
          block_{&block},
          begin_{device::packet_headroom},
          end_{device::packet_headroom} {
    }

    Packet::Packet(Packet&& other) noexcept
        : pool_{std::exchange(other.pool_, nullptr)},
          block_{std::exchange(other.block_, nullptr)},
          begin_{std::exchange(other.begin_, 0)},
          end_{std::exchange(other.end_, 0)},
          rx_{std::exchange(other.rx_, device::RxMetadata{})},
          tx_{std::exchange(other.tx_, device::TxMetadata{})},
          borrowed_{std::exchange(other.borrowed_, false)} {
    }

    Packet& Packet::operator=(Packet&& other) noexcept {
        if (this != &other) {
            release();
            pool_     = std::exchange(other.pool_, nullptr);
            block_    = std::exchange(other.block_, nullptr);
            begin_    = std::exchange(other.begin_, 0);
            end_      = std::exchange(other.end_, 0);
            rx_       = std::exchange(other.rx_, device::RxMetadata{});
            tx_       = std::exchange(other.tx_, device::TxMetadata{});
            borrowed_ = std::exchange(other.borrowed_, false);
        }
        return *this;
    }

    Packet::~Packet() {
        release();
    }

    Packet Packet::lend() noexcept {
        assert(block_ != nullptr && "lend of an empty packet");
        assert(!block_->lent && "one loan at a time");
        block_->lent = true;
        Packet handle{*pool_, *block_};
        handle.begin_    = begin_;
        handle.end_      = end_;
        handle.tx_       = tx_;
        handle.borrowed_ = true;
        return handle;
    }

    void Packet::release() noexcept {
        if (block_ == nullptr) {
            return;
        }
        if (borrowed_) {
            block_->lent = false;
            if (block_->orphaned) {
                block_->orphaned = false;
                pool_->give_back(*block_);
            }
        } else if (block_->lent) {
            block_->orphaned = true;  // the handle gives the block back when it goes
        } else {
            pool_->give_back(*block_);
        }
        pool_     = nullptr;
        block_    = nullptr;
        begin_    = 0;
        end_      = 0;
        borrowed_ = false;
    }

    std::span<std::byte> Packet::data() noexcept {
        if (block_ == nullptr) {
            return {};
        }
        return std::span<std::byte>{block_->storage.get() + begin_, end_ - begin_};
    }

    std::span<const std::byte> Packet::data() const noexcept {
        if (block_ == nullptr) {
            return {};
        }
        return std::span<const std::byte>{block_->storage.get() + begin_, end_ - begin_};
    }

    std::size_t Packet::capacity() const noexcept {
        return block_ == nullptr ? 0 : block_->capacity;
    }

    std::size_t Packet::tailroom() const noexcept {
        return capacity() - end_;
    }

    std::optional<std::span<std::byte>> Packet::prepend(const std::size_t count) noexcept {
        assert(!lent() && "a lent packet is not written");
        if (block_ == nullptr || count > headroom()) {
            return std::nullopt;
        }
        begin_ -= count;
        return data().first(count);
    }

    std::optional<std::span<std::byte>> Packet::append(const std::size_t count) noexcept {
        assert(!lent() && "a lent packet is not written");
        if (block_ == nullptr || count > tailroom()) {
            return std::nullopt;
        }
        end_ += count;
        return data().last(count);
    }

    void Packet::trim_front(const std::size_t count) noexcept {
        assert(!lent() && "a lent packet is not written");
        assert(count <= size());
        begin_ += std::min(count, size());
    }

    void Packet::trim_back(const std::size_t count) noexcept {
        assert(!lent() && "a lent packet is not written");
        assert(count <= size());
        end_ -= std::min(count, size());
    }

    Pool::Pool(const std::size_t count, const std::size_t data_capacity)
        : data_capacity_{data_capacity} {
        blocks_.reserve(count);
        free_.reserve(count);
        const std::size_t capacity = device::packet_headroom + data_capacity;
        for (std::size_t index = 0; index < count; ++index) {
            blocks_.push_back(detail::Block{.storage = std::make_unique<std::byte[]>(capacity), .capacity = capacity});
        }
        for (detail::Block& block : blocks_) {
            free_.push_back(&block);
        }
    }

    Pool::~Pool() {
        assert(free_.size() == blocks_.size() && "every packet must be destroyed before its pool");
    }

    std::optional<Packet> Pool::allocate() noexcept {
        if (free_.empty()) {
            return std::nullopt;
        }
        detail::Block* block = free_.back();
        free_.pop_back();
        return Packet{*this, *block};
    }

    void Pool::give_back(detail::Block& block) noexcept {
        assert(free_.size() < blocks_.size());
        assert(!block.lent && !block.orphaned && "a block comes back with no loan outstanding");
        free_.push_back(&block);
    }

}  // namespace aloe::fabric
