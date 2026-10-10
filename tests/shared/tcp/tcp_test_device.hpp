#pragma once

#include <aloe/device>
#include <aloe/fabric>
#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace aloe::testing {

    /**
     * @brief A fabric port seen through a device that can refuse transmits, fail allocations and
     * report another steering description: for the refusals the fabric never produces.
     *
     * Models IsDevice; every call delegates to the port unless a knob says otherwise. Used from the
     * port's queue thread only.
     */
    class TcpTestDevice {
    public:
        using Packet = fabric::Packet;

        explicit TcpTestDevice(fabric::Port& port) noexcept
            : port_{&port} {
        }

        bool refuse_transmit         = false;  ///< `transmit` accepts nothing while set.
        std::size_t fail_allocations = 0;      ///< That many `allocate` calls return nothing, then normal.
        std::optional<device::RssDescription> steering_override;

        [[nodiscard]] std::uint16_t queue_count() const noexcept {
            return port_->queue_count();
        }
        [[nodiscard]] wire::MacAddress mac() const noexcept {
            return port_->mac();
        }
        [[nodiscard]] std::uint16_t mtu() const noexcept {
            return port_->mtu();
        }
        [[nodiscard]] bool link_up() const noexcept {
            return port_->link_up();
        }
        [[nodiscard]] const device::Capabilities& capabilities() const noexcept {
            return port_->capabilities();
        }

        [[nodiscard]] const device::RssDescription& steering() const noexcept {
            return steering_override ? *steering_override : port_->steering();
        }

        [[nodiscard]] std::optional<Packet> allocate(const std::uint16_t queue) noexcept {
            if (fail_allocations > 0) {
                --fail_allocations;
                return std::nullopt;
            }
            return port_->allocate(queue);
        }

        [[nodiscard]] std::size_t receive(const std::uint16_t queue, std::span<Packet> out) const noexcept {
            return port_->receive(queue, out);
        }

        [[nodiscard]] std::size_t transmit(const std::uint16_t queue, std::span<Packet> in) const noexcept {
            return refuse_transmit ? 0 : port_->transmit(queue, in);
        }

        [[nodiscard]] device::QueueCounters counters(const std::uint16_t queue) const noexcept {
            return port_->counters(queue);
        }

        [[nodiscard]] fabric::Port& port() noexcept {
            return *port_;
        }

    private:
        fabric::Port* port_;
    };

    static_assert(device::IsDevice<TcpTestDevice>);

}  // namespace aloe::testing
