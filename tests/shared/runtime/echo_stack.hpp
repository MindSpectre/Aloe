#pragma once

#include <algorithm>
#include <aloe/device>
#include <aloe/loop>
#include <aloe/runtime>
#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace aloe::testing {

    /**
     * @brief The simplest stack: swap the Ethernet addresses, stamp the shard index into the last
     * two bytes of the payload, send the frame back on the queue it came from.
     *
     * Only frames addressed to the port's own MAC are answered. Anything else, and anything shorter
     * than an Ethernet header plus two bytes, is counted dropped and left in the burst for the shard
     * to free. That is what lets an echo on a loopback device terminate: the reply comes back
     * addressed to the peer and is dropped. The stamp is how a test learns which shard answered.
     */
    template <device::IsDevice Device>
    class EchoStack {
    public:
        using Packet = typename Device::Packet;

        EchoStack(runtime::ShardContext& context, loop::ShardQueue<Device>& queue) noexcept
            : context_{&context},
              queue_{&queue} {
        }

        void on_receive(std::span<Packet> burst) noexcept {
            for (Packet& packet : burst) {
                const std::span<std::byte> data = packet.data();
                if (data.size() < wire::ethernet_header_size + 2 || !addressed_to_me(data)) {
                    ++dropped_;
                    continue;
                }
                std::swap_ranges(data.begin(), data.begin() + 6, data.begin() + 6);
                wire::store_be16(data.last(2), context_->index());
                if (queue_->transmit(std::move(packet))) {
                    ++echoed_;
                } else {
                    ++refused_;
                }
            }
        }

        [[nodiscard]] std::uint64_t echoed() const noexcept {
            return echoed_;
        }

        [[nodiscard]] std::uint64_t dropped() const noexcept {
            return dropped_;
        }

        [[nodiscard]] std::uint64_t refused() const noexcept {
            return refused_;
        }

    private:
        [[nodiscard]] bool addressed_to_me(const std::span<const std::byte> frame) const noexcept {
            wire::MacAddress::Bytes destination{};
            std::ranges::copy(frame.first(wire::MacAddress::size), destination.begin());
            return wire::MacAddress{destination} == queue_->mac();
        }

        runtime::ShardContext* context_;
        loop::ShardQueue<Device>* queue_;
        std::uint64_t echoed_  = 0;
        std::uint64_t dropped_ = 0;
        std::uint64_t refused_ = 0;
    };

    /// The shard index an echo stamped into a frame.
    [[nodiscard]] inline std::uint16_t stamp_of(const std::span<const std::byte> frame) {
        return wire::load_be16(frame.last(2));
    }

}  // namespace aloe::testing
