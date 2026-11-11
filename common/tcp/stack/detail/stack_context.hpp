#pragma once

#include <algorithm>
#include <aloe/core>
#include <aloe/loop>
#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>

#include <detail/control_block.hpp>
#include <detail/held_segment_pool.hpp>
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

        /// `validated` is the config after `TcpConfig::validate`; one block per slot is built here over `owner`.
        StackContext(Ip& ip_brick,
                     const TcpConfig& validated,
                     Stack<Ip, CongestionControl>& owner,
                     const loop::Timer::Function fire)
            : ip{ip_brick},
              config{validated},
              mss{static_cast<std::uint16_t>(ip_brick.max_l4_size() - wire::TcpHeader::size)},
              byte_budget{static_cast<std::uint32_t>(std::min<std::uint64_t>(
                  std::uint64_t{config.receive_segments} * mss, std::numeric_limits<std::uint16_t>::max()))},
              pool{config.receive_pool} {
            for (std::size_t index = 0; index < config.connections; ++index) {
                blocks.emplace_back(owner, fire, static_cast<std::uint32_t>(index), mss);
            }
        }

        StackContext(const StackContext&)            = delete;
        StackContext& operator=(const StackContext&) = delete;
        StackContext(StackContext&&)                 = delete;
        StackContext& operator=(StackContext&&)      = delete;
        ~StackContext()                              = default;

        Ip& ip;
        TcpConfig config{};
        std::uint16_t mss         = 0;  ///< Ours: `max_l4_size() - 20`.
        /// `min(receive_segments * mss, 65535)`: the receive credit a connection starts with.
        std::uint32_t byte_budget = 0;
        HeldSegmentPool<Packet> pool;
        std::deque<ControlBlock> blocks{};
        TcpCounters counters{};
        core::TimePoint now{};
    };

}  // namespace aloe::tcp::detail
