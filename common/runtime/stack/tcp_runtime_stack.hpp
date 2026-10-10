#pragma once

#include <aloe/core>
#include <cstddef>
#include <cstdint>
#include <span>

#include <arp_forward.hpp>
#include <datagram.hpp>
#include <device.hpp>
#include <ipv4.hpp>
#include <ipv4_address.hpp>
#include <ipv4_protocol.hpp>
#include <mac_address.hpp>
#include <shard_context.hpp>
#include <shard_queue.hpp>
#include <streams.hpp>
#include <tcp_config.hpp>
#include <tcp_stack.hpp>

namespace aloe::runtime {

    namespace detail {

        /**
         * The IP config a shard's brick is built with. On a multi-queue device every shard shares one address,
         * and the reply to a request a sibling sent lands on queue 0, so the brick there must learn replies it did
         * not solicit in order to forward them.
         */
        template <device::IsDevice Device>
        [[nodiscard]] net::Ipv4Config shard_ip_config(const loop::ShardQueue<Device>& queue,
                                                      const net::Ipv4Config& config) {
            net::Ipv4Config copy = config;
            if (queue.queue_count() > 1) {
                copy.accept_unsolicited_replies = true;
            }
            return copy;
        }

    }  // namespace detail

    /**
     * @brief The ready-made stack for a TCP shard: the IP brick, the TCP brick and the stream owner,
     * with the tick glue. Models IsStack with both hooks.
     *
     * `on_receive` runs IP then TCP at the step's stamp and forwards that pass's ARP resolutions to
     * every sibling once. `on_tick` runs an empty TCP process, which publishes retry hints, then the
     * wake pass. `on_flush` runs the wake pass again, for events timers and tasks raised, then TCP's
     * flush. On a device with more than one queue the IP brick accepts unsolicited ARP replies, so queue
     * 0 learns what its siblings asked for. A program that wants another composition writes its own
     * IsStack over the same bricks.
     */
    template <device::IsDevice Device>
    class TcpStack {
    public:
        using Packet      = typename Device::Packet;
        using Ip          = net::Ipv4<Device>;
        using Tcp         = tcp::Stack<Ip>;
        using StreamsType = Streams<Tcp>;
        using StreamType  = Stream<Tcp>;

        TcpStack(ShardContext& context,
                 loop::ShardQueue<Device>& queue,
                 const net::Ipv4Config& ip_config,
                 const tcp::TcpConfig& tcp_config)
            : context_{&context},
              ip_{queue, detail::shard_ip_config(queue, ip_config)},
              tcp_{ip_, context.timers(), tcp_config},
              streams_{tcp_, context} {
            context.set_arp_sink({.object = this, .learn = &TcpStack::learn});
        }

        TcpStack(const TcpStack&)            = delete;
        TcpStack& operator=(const TcpStack&) = delete;
        TcpStack(TcpStack&&)                 = delete;
        TcpStack& operator=(TcpStack&&)      = delete;

        /// Tears the sink down first, while every member is still alive; then Streams, TCP and IP, in that order.
        ~TcpStack() {
            context_->set_arp_sink({});
        }

        void on_receive(const std::span<Packet> burst) noexcept {
            const core::TimePoint now = context_->now();
            ip_.process(burst, now);
            tcp_.process(ip_.received(wire::Ipv4Protocol::Tcp), now);
            for (const net::ArpResolution& resolution : ip_.resolved()) {
                const detail::ForwardOutcome outcome =
                    detail::forward_resolution(context_->siblings(), resolution.address, resolution.mac);
                forwards_dropped_  += outcome.dropped;
                forwards_rejected_ += outcome.rejected;
            }
        }

        void on_tick(const core::TimePoint now) noexcept {
            tcp_.process({}, now);
            streams_.wake(now);
        }

        void on_flush(const core::TimePoint now) noexcept {
            streams_.wake(now);
            tcp_.flush(now);
        }

        [[nodiscard]] Ip& ip() noexcept {
            return ip_;
        }
        [[nodiscard]] Tcp& tcp() noexcept {
            return tcp_;
        }
        [[nodiscard]] StreamsType& streams() noexcept {
            return streams_;
        }
        /// Forwards that could not be allocated; read on the shard or after it stopped.
        [[nodiscard]] std::uint64_t forwards_dropped() const noexcept {
            return forwards_dropped_;
        }
        /// Forwards a finished sibling refused, freed by this shard; read on the shard or after it stopped.
        [[nodiscard]] std::uint64_t forwards_rejected() const noexcept {
            return forwards_rejected_;
        }

    private:
        static void learn(void* object,
                          const wire::Ipv4Address address,
                          const wire::MacAddress mac,
                          const core::TimePoint now) noexcept {
            static_cast<TcpStack*>(object)->ip_.learn(address, mac, now);  // reports nothing: no echo between shards
        }

        ShardContext* context_ = nullptr;
        Ip ip_;                ///< Declared first: destroyed last.
        Tcp tcp_;              ///< Over `ip_` and the context's wheel.
        StreamsType streams_;  ///< Over `tcp_`: destroyed first.
        std::uint64_t forwards_dropped_  = 0;
        std::uint64_t forwards_rejected_ = 0;
    };

}  // namespace aloe::runtime
