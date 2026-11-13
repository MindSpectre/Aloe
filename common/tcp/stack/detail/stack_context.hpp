#pragma once

#include <algorithm>
#include <aloe/core>
#include <aloe/loop>
#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <deque>

#include <detail/control_block.hpp>
#include <detail/held_segment_pool.hpp>
#include <detail/receive_window.hpp>
#include <tcp_config.hpp>
#include <tcp_counters.hpp>
#include <tcp_fwd.hpp>

namespace aloe::tcp::detail {

    /**
     * @brief What every part of the stack reads: the IP brick, the validated config and what is derived from it,
     * the pool of held segments, the blocks, the counters and the stamp of the last `process`, `flush` or
     * `connect`. One per shard, owned by the stack, held by reference by its parts.
     */
    template <typename Ip, typename CongestionControl>
    struct StackContext {
        using Packet       = Ip::Packet;
        using ControlBlock = detail::ControlBlock<Ip, CongestionControl>;

        /// `validated` is the config after `TcpConfig::validate`; one block per slot is built here over `owner`, with
        /// the trampolines of its two timers.
        StackContext(Ip& ip_brick,
                     const TcpConfig& validated,
                     Stack<Ip, CongestionControl>& owner,
                     const loop::Timer::Function fire,
                     const loop::Timer::Function fire_ack)
            : ip{ip_brick},
              config{validated},
              mss{static_cast<std::uint16_t>(ip_brick.max_l4_size() - wire::TcpHeader::size)},
              window_shift{
                  config.window_scaling
                      ? ReceiveWindow::shift_for(ReceiveWindow::budget_of(
                            config.receive_segments, mss, config.receive_window, wire::TcpOptions::max_window_scale))
                      : std::uint8_t{0}},
              largest_budget{
                  ReceiveWindow::budget_of(config.receive_segments, mss, config.receive_window, window_shift)},
              syn_window{static_cast<std::uint16_t>(std::min(largest_budget, ReceiveWindow::unscaled_max))},
              pool{config.receive_pool} {
            for (std::size_t index = 0; index < config.connections; ++index) {
                blocks.emplace_back(owner, fire, fire_ack, static_cast<std::uint32_t>(index), mss);
            }
        }

        StackContext(const StackContext&)            = delete;
        StackContext& operator=(const StackContext&) = delete;
        StackContext(StackContext&&)                 = delete;
        StackContext& operator=(StackContext&&)      = delete;
        ~StackContext()                              = default;

        Ip& ip;
        TcpConfig config{};
        std::uint16_t mss            = 0;  ///< Ours: `max_l4_size() - 20`.
        /// Ours, offered on every SYN and SYN-ACK when window scaling is on: the smallest shift under which the field
        /// states the largest budget the configuration allows, counted in our MSS; zero with scaling off.
        std::uint8_t window_shift    = 0;
        /// `ReceiveWindow::budget_of(receive_segments, mss, receive_window, window_shift)`.
        std::uint32_t largest_budget = 0;
        /// `min(largest_budget, 65535)`: what a SYN states, before the connection has a budget.
        std::uint16_t syn_window     = 0;
        HeldSegmentPool<Packet> pool;
        std::deque<ControlBlock> blocks{};
        TcpCounters counters{};
        core::TimePoint now{};
    };

}  // namespace aloe::tcp::detail
