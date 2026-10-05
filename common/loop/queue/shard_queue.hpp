#pragma once

#include <algorithm>
#include <aloe/core>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <counters.hpp>
#include <device.hpp>
#include <mac_address.hpp>
#include <rss.hpp>


namespace aloe::loop {

    /**
     * @brief One queue of a device as the stack sees it: allocate, transmit, and the device's facts.
     *
     * Owns a bounded transmit ring. `transmit` appends; when the ring is full it flushes to the
     * device first and retries once; if the device still refuses it returns false and the caller
     * keeps the packet. That false is the backpressure signal TCP treats as loss. The shard flushes
     * the ring at the end of every tick. Nothing here blocks, throws or allocates after construction.
     */
    template <device::IsDevice Device>
    class ShardQueue {
    public:
        using Packet = typename Device::Packet;

        ShardQueue(Device& owner, const std::uint16_t index, const std::size_t ring_capacity, ShardCounters& counters)
            : device_{&owner},
              queue_{index},
              counters_{&counters},
              ring_(ring_capacity) {
        }

        ShardQueue(const ShardQueue&)            = delete;
        ShardQueue& operator=(const ShardQueue&) = delete;
        ShardQueue(ShardQueue&&)                 = delete;
        ShardQueue& operator=(ShardQueue&&)      = delete;
        ~ShardQueue()                            = default;

        [[nodiscard]] std::optional<Packet> allocate() noexcept {
            core::force_non_const(this);  // the stack's mutable view of the device, though only the pointee is written
            return device_->allocate(queue_);
        }

        /// Fills `out` from the front and returns how many; the slots must hold empty packets.
        [[nodiscard]] std::size_t receive(std::span<Packet> out) noexcept {
            core::force_non_const(this);  // the stack's mutable view of the device, though only the pointee is written
            return device_->receive(queue_, out);
        }

        /// False when the ring is full and the device takes nothing; the packet then stays with the caller.
        [[nodiscard]] bool transmit(Packet&& packet) noexcept {
            if (size_ == ring_.size()) {
                flush();
                if (size_ == ring_.size()) {
                    ++counters_->transmit_refused;
                    return false;
                }
            }
            ring_[(head_ + size_) % ring_.size()] = std::move(packet);
            ++size_;
            return true;
        }

        /// Hands the ring's front to the device, at most two calls because the ring wraps; returns how many it
        /// accepted. A partial accept ends the flush. Ethdev takes at most `Port::max_burst` per call, so a ring
        /// holding more than that drains over several ticks; harmless for the echo, noted in the spec's "Open
        /// questions" for TCP.
        std::size_t flush() noexcept {
            std::size_t accepted_total = 0;
            while (size_ > 0) {
                const std::size_t contiguous = std::min(size_, ring_.size() - head_);
                const std::size_t accepted =
                    device_->transmit(queue_, std::span<Packet>{ring_}.subspan(head_, contiguous));
                accepted_total += accepted;
                head_           = (head_ + accepted) % ring_.size();
                size_          -= accepted;
                if (accepted < contiguous) {
                    break;
                }
            }
            counters_->frames_transmitted += accepted_total;
            return accepted_total;
        }

        /// Drops whatever the device would not take, counting it as refused. The shard calls it once at drain.
        void discard() noexcept {
            while (size_ > 0) {
                ring_[head_] = Packet{};
                head_        = (head_ + 1) % ring_.size();
                --size_;
                ++counters_->transmit_refused;
            }
        }

        [[nodiscard]] std::size_t pending() const noexcept {
            return size_;
        }

        [[nodiscard]] std::size_t capacity() const noexcept {
            return ring_.size();
        }

        [[nodiscard]] std::uint16_t index() const noexcept {
            return queue_;
        }

        [[nodiscard]] wire::MacAddress mac() const noexcept {
            return device_->mac();
        }

        [[nodiscard]] std::uint16_t mtu() const noexcept {
            return device_->mtu();
        }

        [[nodiscard]] const device::Capabilities& capabilities() const noexcept {
            return device_->capabilities();
        }

        [[nodiscard]] const device::RssDescription& steering() const noexcept {
            return device_->steering();
        }

        [[nodiscard]] std::uint16_t queue_count() const noexcept {
            return device_->queue_count();
        }

        [[nodiscard]] const Device& device() const noexcept {
            return *device_;
        }

    private:
        Device* device_;
        std::uint16_t queue_;
        ShardCounters* counters_;
        std::vector<Packet> ring_;
        std::size_t head_ = 0;
        std::size_t size_ = 0;
    };

}  // namespace aloe::loop
