#pragma once

#include <aloe/device>
#include <aloe/loop>
#include <cstdint>
#include <utility>

namespace aloe::net::detail {

    /// Transmits a frame a part built in `packet`: counts it under `sent`, or under `refused` when the queue and
    /// the device are both full, in which case the packet goes back to its pool.
    template <device::IsDevice Device>
    void transmit_built(loop::ShardQueue<Device>& queue,
                        typename Device::Packet&& packet,
                        std::uint64_t& sent,
                        std::uint64_t& refused) noexcept {
        if (queue.transmit(std::move(packet))) {
            ++sent;
        } else {
            ++refused;
        }
    }

}  // namespace aloe::net::detail
