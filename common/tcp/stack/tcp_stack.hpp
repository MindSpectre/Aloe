#pragma once

#include <aloe/core>
#include <aloe/device>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/stream>
#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <span>

#include <congestion_control.hpp>
#include <detail/ack_processor.hpp>
#include <detail/control_block.hpp>
#include <detail/lifecycle.hpp>
#include <detail/receiver.hpp>
#include <detail/sent_segment_pool.hpp>
#include <detail/stack_context.hpp>
#include <detail/transmitter.hpp>
#include <read_view.hpp>
#include <tcp_config.hpp>
#include <tcp_connection.hpp>
#include <tcp_counters.hpp>
#include <tcp_fwd.hpp>

namespace aloe::tcp {

    /**
     * @brief The minimal TCP brick: one per shard, over the IP brick and the shard's wheel.
     *
     * Four verbs: `process` the segments IP sorted out, `poll_event` to drain notifications, the
     * connections' non-blocking operations, `flush` for the resends and ACKs that waited. Every segment
     * that occupies sequence space is kept in its connection's retransmission queue, as the packet it
     * left in, until an ACK covers it. The stack keeps the
     * stamp of the last `process`, `flush` or `connect` and uses it for everything a connection
     * does in between. Everything is allocated at construction; nothing here logs, blocks or
     * names a clock.
     *
     * The stack is a façade over four parts, each a class of its own in `tcp::detail`: the Transmitter (every
     * segment that leaves, the retransmission queue, the send side and its timers), the Lifecycle (the connection
     * table and every change of a connection's state), the AckProcessor (what an ACK does to the send side) and the
     * Receiver (what a datagram does to a connection, and `consume`). The dependency runs one way: the Receiver
     * calls the other three, the Lifecycle and the AckProcessor call the Transmitter and not each other, and the
     * Transmitter calls none. They share the `detail::StackContext` (the IP brick, the pool, the counters, the
     * stamp) and work on the connections' `detail::ControlBlock`s, which the `Connection` handles name.
     */
    template <typename Ip, typename CongestionControl>
    class Stack {
        static_assert(IsIp<Ip>, "tcp::Stack needs net::Ipv4<Device>, or an IP brick with the same members");
        static_assert(IsCongestionControl<CongestionControl>,
                      "tcp::Stack needs a congestion control: a State and the reset, window and on_* hooks");
        static_assert(net::Prebuilt::size + wire::TcpHeader::max_size <= device::packet_headroom,
                      "every frame's headers, with any option block, fit in front of the payload of a packet from "
                      "allocate()");
        // The connection and its view are templates over the IP brick: checked here, where they first have one.
        static_assert(stream::IsStream<Connection<Ip, CongestionControl>>);
        static_assert(stream::IsReadView<ReadView<typename Ip::Packet>>);

    public:
        using Device         = Ip::Device;
        using Packet         = Ip::Packet;
        using Policy         = CongestionControl;
        using ConnectionType = Connection<Ip, CongestionControl>;
        using EventType      = ConnectionEvent<Ip, CongestionControl>;
        using Sequence       = wire::TcpSequence;

        /// Validates and throws std::invalid_argument; allocates everything; sends nothing.
        Stack(Ip& ip, loop::TimerWheel& wheel, const TcpConfig& config)
            : context_{ip, config.validate(ip.max_l4_size(), wheel.resolution()), *this, &Stack::on_timer_fired},
              transmitter_{context_, wheel},
              lifecycle_{context_, transmitter_},
              acks_{context_, transmitter_},
              receiver_{context_, lifecycle_, acks_, transmitter_} {
            for (ControlBlock& block : context_.blocks) {
                connections_.emplace_back(typename ConnectionType::PrivateTag{}, block);
            }
        }

        Stack(const Stack&)            = delete;
        Stack& operator=(const Stack&) = delete;
        Stack(Stack&&)                 = delete;
        Stack& operator=(Stack&&)      = delete;
        /// Cancels every timer, returns every held packet and drops every queued one, which goes back to its pool
        /// now or when the device returns it; the IP brick and the wheel outlive it.
        ~Stack() {
            for (ControlBlock& block : context_.blocks) {
                transmitter_.disarm(block);
                block.prepared.reset();
                block.receive.release(context_.pool);
                transmitter_.drop_queue(block);
            }
        }

        /// Records the stamp, publishes deferred retry hints, then processes each datagram. Every packet is moved
        /// out or released. An empty span is valid.
        void process(const std::span<net::Datagram<Packet>> segments, const core::TimePoint now) noexcept {
            context_.now = now;
            for (ControlBlock* block = transmitter_.pop_retry(); block != nullptr; block = transmitter_.pop_retry()) {
                // The deferred retry hint, at most one per marked connection.
                lifecycle_.raise(*block, stream::Event::Writable);
            }
            for (Datagram& datagram : segments) {
                if (datagram.packet.empty()) {
                    continue;
                }
                receiver_.on_datagram(datagram);
                if (!datagram.packet.empty()) {
                    datagram.packet = Packet{};  // not held: back to the pool now, not at IP's next process
                }
            }
        }

        /// Removes one pending notification and returns its snapshot; nothing when none is pending.
        [[nodiscard]] std::optional<EventType> poll_event() noexcept {
            ControlBlock* block = lifecycle_.pop_event();
            if (block == nullptr) {
                return std::nullopt;
            }
            const stream::Events snapshot = block->events;
            block->events                 = {};
            return EventType{.connection = &connections_[block->index], .events = snapshot};
        }

        /// Retries the resends that found their packet lent, which leave once it is back or go by the busy rule, then
        /// sends the ordinary ACKs and window updates that waited, one pure ACK per connection.
        void flush(const core::TimePoint now) noexcept {
            context_.now = now;
            transmitter_.flush_deferred();
            for (ControlBlock* block = transmitter_.ack_front(); block != nullptr;) {
                ControlBlock* next = Transmitter::ack_next(*block);  // a successful send unlinks the block
                if (transmitter_.send_pure_ack(*block)) {
                    lifecycle_.maybe_finish(*block);
                }
                block = next;
            }
        }

        [[nodiscard]] std::expected<void, ListenError> listen(const std::uint16_t port) noexcept {
            return lifecycle_.listen(port);
        }

        /// Connections already open stay open.
        void unlisten(const std::uint16_t port) noexcept {
            lifecycle_.unlisten(port);
        }

        /// A SynSent connection the caller owns; its SYN is queued, or waits for ARP on the timer.
        [[nodiscard]] std::expected<ConnectionType*, ConnectError> connect(const Endpoint peer,
                                                                           const core::TimePoint now) noexcept {
            context_.now                                            = now;
            const std::expected<ControlBlock*, ConnectError> opened = lifecycle_.connect(peer);
            if (!opened) {
                return std::unexpected{opened.error()};
            }
            return &connections_[(*opened)->index];
        }

        [[nodiscard]] ConnectionType& connection(const std::uint32_t index) noexcept {
            return connections_[index];
        }
        [[nodiscard]] const ConnectionType& connection(const std::uint32_t index) const noexcept {
            return connections_[index];
        }
        [[nodiscard]] std::size_t capacity() const noexcept {
            return connections_.size();
        }
        /// Ours: `max_l4_size() - 20`.
        [[nodiscard]] std::uint16_t mss() const noexcept {
            return context_.mss;
        }
        /// The largest budget the configuration allows, under the stack's window shift; a connection's own is
        /// `Connection::receive_budget()`.
        [[nodiscard]] std::uint32_t byte_budget() const noexcept {
            return context_.largest_budget;
        }
        [[nodiscard]] const TcpCounters& counters() const noexcept {
            return context_.counters;
        }
        [[nodiscard]] const TcpConfig& config() const noexcept {
            return context_.config;
        }
        [[nodiscard]] bool listening(const std::uint16_t port) const noexcept {
            return lifecycle_.listening(port);
        }
        [[nodiscard]] std::size_t pending_events() const noexcept {
            return lifecycle_.pending_events();
        }
        [[nodiscard]] core::TimePoint now() const noexcept {
            return context_.now;
        }
        [[nodiscard]] std::size_t nodes_available() const noexcept {
            return context_.pool.available();
        }
        [[nodiscard]] std::size_t table_size() const noexcept {
            return lifecycle_.table_size();
        }
        /// For tests and monitoring: the entry at `position` of the connection's retransmission queue, 0 the head.
        [[nodiscard]] const detail::SentSegment<Packet>& sent_segment(const ConnectionType& conn,
                                                                      const std::size_t position) const noexcept {
            return transmitter_.sent_segment(conn.block_, position);
        }
        [[nodiscard]] std::size_t send_nodes_available() const noexcept {
            return transmitter_.nodes_available();
        }

        /// For tests: the next connection opened takes this initial sequence number instead of a drawn one.
        void force_next_isn(const wire::TcpSequence isn) noexcept {
            lifecycle_.force_next_isn(isn);
        }

        /// For tests: the connection's RFC 6582 `recover` takes this value, as if an episode had recorded it.
        void force_recover(ConnectionType& conn, const wire::TcpSequence recover) noexcept {
            core::force_non_static(this);
            conn.block_.recovery.force_recover(recover);
        }

    private:
        friend class Connection<Ip, CongestionControl>;

        using ControlBlock = detail::ControlBlock<Ip, CongestionControl>;
        using Datagram     = net::Datagram<Packet>;
        using Transmitter  = detail::Transmitter<Ip, CongestionControl>;

        /// The wheel's callback: the node's block, then the Lifecycle's timer handling.
        static void on_timer_fired(loop::Timer& timer) noexcept {
            ControlBlock& block = static_cast<ControlBlock::TimerNode&>(timer).block;
            block.stack.lifecycle_.on_timer(block);
        }

        // The handle's verbs: the Receiver's consume, the Transmitter's send side and the Lifecycle's close, abort
        // and release.
        [[nodiscard]] ConnectionType::View unread(const ControlBlock& block) noexcept {
            return block.receive.view(context_.pool);
        }
        void consume(ControlBlock& block, const std::size_t count) noexcept {
            receiver_.consume(block, count);
        }
        [[nodiscard]] std::size_t writable(const ControlBlock& block) const noexcept {
            return transmitter_.writable(block);
        }
        [[nodiscard]] std::optional<std::span<std::byte>> prepare(ControlBlock& block,
                                                                  const std::size_t count) noexcept {
            return transmitter_.prepare(block, count);
        }
        [[nodiscard]] bool commit(ControlBlock& block, const std::size_t count) noexcept {
            return transmitter_.commit(block, count);
        }
        void close(ControlBlock& block) noexcept {
            lifecycle_.close(block);
        }
        void abort(ControlBlock& block) noexcept {
            lifecycle_.abort(block);
        }
        void release(ControlBlock& block) noexcept {
            lifecycle_.release(block);
        }

        detail::StackContext<Ip, CongestionControl> context_;
        detail::Transmitter<Ip, CongestionControl> transmitter_;
        detail::Lifecycle<Ip, CongestionControl> lifecycle_;
        detail::AckProcessor<Ip, CongestionControl> acks_;
        detail::Receiver<Ip, CongestionControl> receiver_;
        std::deque<ConnectionType> connections_ = {};
    };

}  // namespace aloe::tcp
