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
          begin_{packet_headroom},
          end_{packet_headroom} {
    }

    Packet::Packet(Packet&& other) noexcept
        : pool_{std::exchange(other.pool_, nullptr)},
          block_{std::exchange(other.block_, nullptr)},
          begin_{std::exchange(other.begin_, 0)},
          end_{std::exchange(other.end_, 0)},
          rx_{std::exchange(other.rx_, RxMetadata{})},
          tx_{std::exchange(other.tx_, TxMetadata{})} {
    }

    Packet& Packet::operator=(Packet&& other) noexcept {
        if (this != &other) {
            release();
            pool_  = std::exchange(other.pool_, nullptr);
            block_ = std::exchange(other.block_, nullptr);
            begin_ = std::exchange(other.begin_, 0);
            end_   = std::exchange(other.end_, 0);
            rx_    = std::exchange(other.rx_, RxMetadata{});
            tx_    = std::exchange(other.tx_, TxMetadata{});
        }
        return *this;
    }

    Packet::~Packet() {
        release();
    }

    void Packet::release() noexcept {
        if (block_ != nullptr) {
            pool_->give_back(*block_);
            pool_  = nullptr;
            block_ = nullptr;
            begin_ = 0;
            end_   = 0;
        }
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

    std::optional<std::span<std::byte>> Packet::prepend(std::size_t count) noexcept {
        if (block_ == nullptr || count > headroom()) {
            return std::nullopt;
        }
        begin_ -= count;
        return data().first(count);
    }

    std::optional<std::span<std::byte>> Packet::append(std::size_t count) noexcept {
        if (block_ == nullptr || count > tailroom()) {
            return std::nullopt;
        }
        end_ += count;
        return data().last(count);
    }

    void Packet::trim_front(std::size_t count) noexcept {
        assert(count <= size());
        begin_ += std::min(count, size());
    }

    void Packet::trim_back(std::size_t count) noexcept {
        assert(count <= size());
        end_ -= std::min(count, size());
    }

    Pool::Pool(std::size_t count, std::size_t data_capacity)
        : data_capacity_{data_capacity} {
        blocks_.reserve(count);
        free_.reserve(count);
        const std::size_t capacity = packet_headroom + data_capacity;
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
        free_.push_back(&block);
    }

}  // namespace aloe::fabric
