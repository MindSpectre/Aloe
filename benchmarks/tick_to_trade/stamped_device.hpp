#pragma once

#include <aloe/device>
#include <aloe/wire>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <tsc_clock.hpp>

namespace aloe::benchmarks {

    /**
     * @brief A device seen through a wrapper that stamps the transmit the application armed.
     *
     * Models IsDevice and forwards every call. `arm(round)` says the next transmit the device accepts carries that
     * round's critical frame; once the device has taken it, `transmit` writes `TscClock::get_ticks()` into
     * `stops[round]` and disarms. An unarmed transmit, a pure ACK leaving in an early flush say, stamps nothing. The
     * one stop point of the floor, level 0 and level 1, at the cost of one register read per transmit everywhere.
     */
    template <device::IsDevice Device>
    class StampedDevice {
    public:
        using Packet = typename Device::Packet;

        StampedDevice(Device& owner, std::span<std::uint64_t> stops) noexcept
            : device_{&owner},
              stops_{stops} {
        }

        void arm(const std::uint64_t round) noexcept {
            assert(round < stops_.size() && "a round beyond the stamps");
            armed_ = true;
            round_ = round;
        }

        [[nodiscard]] bool armed() const noexcept {
            return armed_;
        }

        [[nodiscard]] std::uint64_t stamps_taken() const noexcept {
            return taken_;
        }

        [[nodiscard]] Device& device() noexcept {
            return *device_;
        }

        [[nodiscard]] std::uint16_t queue_count() const noexcept {
            return device_->queue_count();
        }

        [[nodiscard]] wire::MacAddress mac() const noexcept {
            return device_->mac();
        }

        [[nodiscard]] std::uint16_t mtu() const noexcept {
            return device_->mtu();
        }

        [[nodiscard]] bool link_up() const noexcept {
            return device_->link_up();
        }

        [[nodiscard]] const device::Capabilities& capabilities() const noexcept {
            return device_->capabilities();
        }

        [[nodiscard]] const device::RssDescription& steering() const noexcept {
            return device_->steering();
        }

        [[nodiscard]] std::optional<Packet> allocate(const std::uint16_t queue) noexcept {
            return device_->allocate(queue);
        }

        [[nodiscard]] std::size_t receive(const std::uint16_t queue, std::span<Packet> out) noexcept {
            return device_->receive(queue, out);
        }

        /// Forwards, then stamps when armed and the device took at least one packet.
        [[nodiscard]] std::size_t transmit(const std::uint16_t queue, std::span<Packet> in) noexcept {
            const std::size_t accepted = device_->transmit(queue, in);
            if (accepted > 0 && armed_) {
                stops_[round_] = core::TscClock::get_ticks();
                armed_         = false;
                ++taken_;
            }
            return accepted;
        }

        [[nodiscard]] device::QueueCounters counters(const std::uint16_t queue) const noexcept {
            return device_->counters(queue);
        }

    private:
        Device* device_;
        std::span<std::uint64_t> stops_;
        std::uint64_t round_ = 0;
        std::uint64_t taken_ = 0;
        bool armed_          = false;
    };

}  // namespace aloe::benchmarks
