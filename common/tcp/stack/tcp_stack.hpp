#pragma once

#include <algorithm>
#include <aloe/core>
#include <aloe/device>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/stream>
#include <aloe/wire>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <tuple>
#include <utility>

#include <congestion_control.hpp>
#include <detail/connection_table.hpp>
#include <detail/control_block.hpp>
#include <detail/held_segment_pool.hpp>
#include <detail/isn_generator.hpp>
#include <detail/loss_recovery.hpp>
#include <detail/retransmission_queue.hpp>
#include <detail/stack_context.hpp>
#include <detail/transmitter.hpp>
#include <flow_table.hpp>
#include <newreno.hpp>
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
     * The stack is the protocol's state machine and the shard's lists; each concern it drives is a part of its own:
     * the connection directory (`detail::ConnectionTable`), the segment writer over IP (`detail::SegmentWriter`), and
     * per connection the receive queue, the ACK policy, the send sequence, the retransmission queue, loss recovery,
     * the RTT estimator, the zero-window probe and the timestamps.
     */
    template <typename Ip, typename CongestionControl>
    class Stack {
        static_assert(IsIp<Ip>, "tcp::Stack needs net::Ipv4<Device>, or an IP brick with the same members");
        static_assert(IsCongestionControl<CongestionControl>,
                      "tcp::Stack needs a congestion control: a State and the reset, window and on_* hooks");
        static_assert(net::Prebuilt::size + wire::TcpHeader::size + wire::TcpOptions::mss_size +
                              wire::TcpOptions::timestamps_size <=
                          device::packet_headroom,
                      "every frame's headers fit in front of the payload of a packet from allocate()");
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
              table_{context_.config, ip.queue().steering()} {
            for (Block& block : context_.blocks) {
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
            for (Block& block : context_.blocks) {
                transmitter_.disarm(block);
                block.prepared.reset();
                return_chain(block);
                transmitter_.drop_queue(block);
            }
        }

        /// Records the stamp, publishes deferred retry hints, then processes each datagram. Every packet is moved
        /// out or released. An empty span is valid.
        void process(const std::span<net::Datagram<Packet>> segments, const core::TimePoint now) noexcept {
            context_.now = now;
            for (Block* block = transmitter_.pop_retry(); block != nullptr; block = transmitter_.pop_retry()) {
                raise(*block, stream::Event::Writable);  // the deferred retry hint, at most one per marked connection
            }
            for (Datagram& datagram : segments) {
                if (datagram.packet.empty()) {
                    continue;
                }
                on_datagram(datagram);
                if (!datagram.packet.empty()) {
                    datagram.packet = Packet{};  // not held: back to the pool now, not at IP's next process
                }
            }
        }

        /// Removes one pending notification and returns its snapshot; nothing when none is pending.
        [[nodiscard]] std::optional<EventType> poll_event() noexcept {
            Block* block = events_.pop_front();
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
            for (Block* block = transmitter_.ack_front(); block != nullptr;) {
                Block* next = Transmitter::ack_next(*block);  // a successful send unlinks the block
                if (transmitter_.send_pure_ack(*block)) {
                    maybe_finish(*block);
                }
                block = next;
            }
        }

        [[nodiscard]] std::expected<void, ListenError> listen(const std::uint16_t port) noexcept {
            return table_.listen(port);
        }

        /// Connections already open stay open.
        void unlisten(const std::uint16_t port) noexcept {
            table_.unlisten(port);
        }

        /// A SynSent connection the caller owns; its SYN is queued, or waits for ARP on the timer.
        [[nodiscard]] std::expected<ConnectionType*, ConnectError> connect(const Endpoint peer,
                                                                           const core::TimePoint now) noexcept {
            context_.now = now;
            if (!context_.ip.next_hop(peer.address)) {
                return std::unexpected{ConnectError::NoRoute};
            }
            if (transmitter_.nodes_available() == 0) {
                return std::unexpected{ConnectError::TableFull};  // no retransmission-queue node for the SYN
            }
            const std::expected<detail::Placement, ConnectError> placed =
                table_.place(peer, context_.ip.address(), context_.ip.queue().steering(), context_.ip.queue().index());
            if (!placed) {
                return std::unexpected{placed.error()};
            }
            const std::optional<std::uint32_t> slot = table_.take_slot();
            if (!slot) {
                return std::unexpected{ConnectError::TableFull};
            }
            Block& block = context_.blocks[*slot];
            bind_flow(block, Endpoint{.address = context_.ip.address(), .port = placed->port}, peer, placed->base_hash);
            block.state = State::SynSent;
            block.owned = true;
            open(block);
            block.timestamps.offer(context_.config.timestamps, isn_.offset());  // the SYN offers; the SYN-ACK decides
            // Arms the timer on a send or an ARP wait; a refused send leaves the SYN to the RTO.
            std::ignore = transmitter_.queue_control(block, wire::TcpFlags{wire::TcpFlag::Syn}, block.send.iss);
            if (!block.timer.armed()) {
                transmitter_.arm_retransmit(block);
            }
            return &connections_[block.index];
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
        /// `min(receive_segments * mss, 65535)`: the receive credit a connection starts with.
        [[nodiscard]] std::uint32_t byte_budget() const noexcept {
            return context_.byte_budget;
        }
        [[nodiscard]] const TcpCounters& counters() const noexcept {
            return context_.counters;
        }
        [[nodiscard]] const TcpConfig& config() const noexcept {
            return context_.config;
        }
        [[nodiscard]] bool listening(const std::uint16_t port) const noexcept {
            return table_.listening(port);
        }
        [[nodiscard]] std::size_t pending_events() const noexcept {
            return events_.size();
        }
        [[nodiscard]] core::TimePoint now() const noexcept {
            return context_.now;
        }
        [[nodiscard]] std::size_t nodes_available() const noexcept {
            return context_.pool.available();
        }
        [[nodiscard]] std::size_t table_size() const noexcept {
            return table_.size();
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
            isn_.force(isn);
        }

        /// For tests: the connection's RFC 6582 `recover` takes this value, as if an episode had recorded it.
        void force_recover(ConnectionType& conn, const wire::TcpSequence recover) noexcept {
            core::force_non_static(this);
            conn.block_.recovery.force_recover(recover);
        }

    private:
        friend class Connection<Ip, CongestionControl>;

        using Block       = detail::ControlBlock<Ip, CongestionControl>;
        using Datagram    = net::Datagram<Packet>;
        using EventList   = detail::ConnectionList<Block, &Block::event_link>;
        using Transmitter = detail::Transmitter<Ip, CongestionControl>;

        /// What the receive path parsed from one datagram.
        struct Segment {
            wire::TcpHeader header{};
            std::uint16_t payload_offset = 0;  ///< From the start of `l4()`: the data offset.
            std::uint16_t payload_length = 0;
            std::optional<std::uint16_t> mss;  ///< From the options of a SYN.
            std::uint32_t base_hash = 0;
            std::optional<std::uint32_t> ts_value{};  ///< RFC 7323 TSval, when the segment carries the option.
            std::optional<std::uint32_t> ts_echo{};   ///< RFC 7323 TSecr, with it.

            /// Sequence space the segment occupies: payload, SYN and FIN.
            [[nodiscard]] std::uint32_t length() const noexcept {
                return payload_length + (header.flags.has(wire::TcpFlag::Syn) ? 1U : 0U) +
                       (header.flags.has(wire::TcpFlag::Fin) ? 1U : 0U);
            }
        };

        // The receive path: header, checksum, lookup, the handshake, the synchronized states.

        void on_datagram(Datagram& datagram) noexcept {
            ++context_.counters.segments_received;
            const std::span<std::byte> l4               = datagram.l4();
            const std::optional<wire::TcpHeader> header = wire::TcpHeader::parse(l4);
            if (!header) {
                ++context_.counters.dropped_bad_header;
                return;
            }
            Segment segment{.header         = *header,
                            .payload_offset = header->data_offset,
                            .payload_length = static_cast<std::uint16_t>(l4.size() - header->data_offset),
                            .mss            = std::nullopt,
                            .base_hash      = 0,
                            .ts_value       = std::nullopt,
                            .ts_echo        = std::nullopt};
            if (header->data_offset > wire::TcpHeader::size) {
                const auto options = wire::TcpOptions::parse(std::span<const std::byte>{l4}.subspan(
                    wire::TcpHeader::size, header->data_offset - wire::TcpHeader::size));
                if (!options) {
                    ++context_.counters.dropped_bad_header;
                    return;
                }
                if (header->flags.has(wire::TcpFlag::Syn)) {
                    segment.mss = options->mss;  // RFC 9293: the option means something only on a SYN
                }
                segment.ts_value = options->timestamp_value;
                segment.ts_echo  = options->timestamp_echo;
            }
            if (!checksum_ok(datagram)) {
                ++context_.counters.dropped_bad_checksum;
                return;
            }
            const FlowKey key{
                .remote = datagram.source, .remote_port = header->source_port, .local_port = header->destination_port};
            segment.base_hash = table_.base_hash(context_.ip.queue().steering(),
                                                 device::FlowTuple{.source           = datagram.source,
                                                                   .destination      = datagram.destination,
                                                                   .source_port      = header->source_port,
                                                                   .destination_port = header->destination_port,
                                                                   .protocol         = wire::Ipv4Protocol::Tcp},
                                                 datagram.packet.rx().rss_hash);
            const std::optional<std::uint32_t> found = table_.find(key, segment.base_hash);
            if (!found) {
                const bool opening = header->flags.has(wire::TcpFlag::Syn) && !header->flags.has(wire::TcpFlag::Ack) &&
                                     !header->flags.has(wire::TcpFlag::Rst);
                if (opening && table_.listening(header->destination_port)) {
                    passive_open(datagram, segment);
                    return;
                }
                ++context_.counters.dropped_no_connection;
                if (!header->flags.has(wire::TcpFlag::Rst)) {
                    transmitter_.reply_reset(datagram.source, segment.header, segment.length());
                }
                return;
            }
            switch (Block& block = context_.blocks[*found]; block.state) {
                case State::Closed:  // still indexed because not released
                    ++context_.counters.dropped_closed;
                    if (!header->flags.has(wire::TcpFlag::Rst)) {
                        transmitter_.reply_reset(datagram.source, segment.header, segment.length());
                    }
                    return;
                case State::SynSent:
                    receive_syn_sent(block, datagram, segment);
                    return;
                default:
                    receive_synchronized(block, datagram, segment);
                    return;
            }
        }

        [[nodiscard]] static bool checksum_ok(Datagram& datagram) noexcept {
            switch (datagram.l4_checksum) {
                case device::ChecksumVerdict::Good:
                    return true;
                case device::ChecksumVerdict::Bad:
                    return false;
                default:
                    break;
            }
            const std::span<const std::byte> l4 = datagram.l4();
            const std::uint32_t pseudo          = wire::ipv4_pseudo_header_sum(
                datagram.source, datagram.destination, wire::Ipv4Protocol::Tcp, static_cast<std::uint16_t>(l4.size()));
            return wire::checksum_finish(wire::checksum_add(pseudo, l4)) == 0;
        }

        void passive_open(Datagram& datagram, const Segment& segment) noexcept {
            if (transmitter_.nodes_available() == 0) {
                ++context_.counters.dropped_no_node;  // no retransmission-queue node for the SYN-ACK
                return;
            }
            const std::optional<std::uint32_t> slot = table_.take_slot();
            if (!slot) {
                ++context_.counters.dropped_table_full;
                return;
            }
            Block& block = context_.blocks[*slot];
            bind_flow(block,
                      Endpoint{.address = context_.ip.address(), .port = segment.header.destination_port},
                      Endpoint{.address = datagram.source, .port = segment.header.source_port},
                      segment.base_hash);
            block.state = State::SynReceived;
            block.owned = false;
            open(block);
            block.receive.open(segment.header.sequence + 1,
                               context_.byte_budget);  // the initial offer, carried by the SYN-ACK
            block.send.peer_mss = segment.mss.value_or(default_peer_mss);
            block.send.wnd      = segment.header.window;
            block.send.wl1      = segment.header.sequence;
            block.send.wl2      = Sequence{};
            block.timestamps.offer(context_.config.timestamps, isn_.offset());
            block.timestamps.answer(segment.ts_value, context_.now);
            block.timestamps.sent_ack(block.receive.next());  // what the SYN-ACK acknowledges, as Linux's rcv_wup
            // Arms the timer on a send or an ARP wait; a refused send leaves the SYN-ACK to the RTO.
            std::ignore = transmitter_.queue_control(
                block, wire::TcpFlags{wire::TcpFlag::Syn, wire::TcpFlag::Ack}, block.send.iss);
            if (!block.timer.armed()) {
                transmitter_.arm_retransmit(block);
            }
        }

        void receive_syn_sent(Block& block, Datagram& datagram, Segment segment) noexcept {
            const wire::TcpHeader& header = segment.header;
            if (header.flags.has(wire::TcpFlag::Ack) && header.acknowledgement != block.send.nxt) {
                ++context_.counters.dropped_unexpected;  // not an answer to our SYN
                return;
            }
            if (header.flags.has(wire::TcpFlag::Rst)) {
                if (!header.flags.has(wire::TcpFlag::Ack)) {
                    ++context_.counters.dropped_unexpected;
                    return;
                }
                ++context_.counters.connections_reset;
                set_closed(block);
                raise(block, stream::Event::Reset);
                return;
            }
            if (!header.flags.has(wire::TcpFlag::Syn) || !header.flags.has(wire::TcpFlag::Ack)) {
                ++context_.counters.dropped_unexpected;  // a simultaneous open is out of scope
                return;
            }
            block.timestamps.answer(segment.ts_value, context_.now);  // our offer, answered
            const typename Transmitter::Freed freed =
                transmitter_.acknowledge(block, header.acknowledgement);  // frees the SYN's entry
            block.send.una = header.acknowledgement;
            on_acknowledged(block, freed, segment.ts_echo);  // a sample, and the timer stopped with the queue empty
            block.receive.open(header.sequence + 1,
                               context_.byte_budget);         // the initial offer, carried by our first ACK
            block.timestamps.sent_ack(block.receive.next());  // as Linux's rcv_wup: data on the SYN-ACK may update it
            block.send.peer_mss = segment.mss.value_or(default_peer_mss);
            block.send.synchronize(header);
            block.state = State::Established;
            transmitter_.reserve_fin_node(block);
            block.recovery.establish(block.mss());
            update_probing(block);  // a SYN-ACK offering no window starts probing as any ACK does
            ++context_.counters.connections_opened;
            transmitter_.queue_ack(
                block);  // deferred: a commit on the Connected event completes the handshake with data
            raise(block, stream::Event::Connected);
            if (segment.payload_length > 0 || header.flags.has(wire::TcpFlag::Fin)) {
                segment.header.sequence = block.receive.next();
                segment.header.flags    = wire::TcpFlags::from_raw(
                    static_cast<std::uint8_t>(header.flags.raw() & ~std::to_underlying(wire::TcpFlag::Syn)));
                receive_synchronized(block, datagram, segment);
            }
        }

        /// A segment on a synchronized connection, in RFC 9293's order; each step may end it.
        void receive_synchronized(Block& block, Datagram& datagram, Segment segment) noexcept {
            const bool rst         = segment.header.flags.has(wire::TcpFlag::Rst);
            const Sequence arrived = segment.header.sequence;  // SEG.SEQ before any trim, for the TS.Recent rule

            // 0. PAWS (RFC 7323), before the sequence check as Linux's tcp_validate_incoming: a timestamp older than
            // TS.Recent is an old duplicate, acknowledged at once and dropped. A RST is exempt; a segment without the
            // option goes on, as Linux lets it.
            if (block.timestamps.rejects(segment.ts_value, rst, context_.now)) {
                ++context_.counters.dropped_paws;
                ack_now(block);
                return;
            }

            // 1. Sequence check against [rcv_nxt, the offered edge).
            if (!accept_sequence(block, segment)) {
                return;
            }
            block.timestamps.update(segment.ts_value, arrived, context_.now);  // acceptable, trimmed or not

            // 2. RST, and a SYN, which a synchronized connection does not take.
            if (rst) {
                receive_reset(block);
                return;
            }
            if (segment.header.flags.has(wire::TcpFlag::Syn)) {
                ++context_.counters.dropped_unexpected;  // a SYN inside the window of a synchronized connection
                return;
            }

            // 3. ACK.
            if (!receive_ack(block, segment)) {
                return;
            }

            // 4. Payload.
            if (segment.payload_length > 0 && !receive_payload(block, datagram, segment)) {
                return;
            }

            // 5. FIN, in order: its sequence is rcv_nxt after the payload.
            if (segment.header.flags.has(wire::TcpFlag::Fin)) {
                if (block.state == State::CloseWait || block.state == State::Closing || block.state == State::LastAck) {
                    ++context_.counters
                          .dropped_unexpected;  // a second FIN: the peer's sequence space ended at the first
                    ack_now(block);
                    return;
                }
                receive_fin(block);
            }
        }

        /**
         * The sequence check against `[rcv_nxt, offered edge)`. A segment entirely before RCV.NXT, past a gap or past
         * the edge is dropped with an immediate ACK, unless it is a RST; data dropped past a gap is remembered, so the
         * data that fills it is acknowledged at once. One that overlaps RCV.NXT loses its old prefix, a SYN with it. A
         * duplicate SYN in SynReceived gets the SYN-ACK again, from its entry. False when the segment goes no further.
         */
        [[nodiscard]] bool accept_sequence(Block& block, Segment& segment) noexcept {
            wire::TcpHeader& header    = segment.header;
            const bool rst             = header.flags.has(wire::TcpFlag::Rst);
            const Sequence next        = block.receive.next();
            const Sequence edge        = block.receive.offered();
            const std::uint32_t window = block.receive.window();
            const Sequence sequence    = header.sequence;
            const Sequence end         = sequence + segment.length();
            const auto drop            = [this, &block, rst](std::uint64_t& counter) {
                ++counter;
                if (!rst) {
                    ack_now(block);
                }
                return false;
            };
            if (segment.length() == 0) {
                const bool acceptable =
                    window == 0 ? sequence == next : !sequence.before(next) && sequence.before(edge);
                return acceptable || drop(context_.counters.dropped_out_of_window);
            }
            if (!end.after(next)) {  // entirely before rcv_nxt
                if (block.state == State::SynReceived && header.flags.has(wire::TcpFlag::Syn) &&
                    !header.flags.has(wire::TcpFlag::Ack)) {
                    std::ignore =
                        transmitter_.resend_head(block);  // a duplicate SYN: the SYN-ACK again, from its entry
                    return false;
                }
                return drop(context_.counters.dropped_duplicate);
            }
            if (sequence.after(next)) {  // a gap before it
                block.ack_policy.gap(end);
                return drop(context_.counters.dropped_out_of_order);
            }
            if (sequence.before(next)) {  // overlap: the old prefix is already ours
                std::uint32_t trim = next.value - sequence.value;
                if (header.flags.has(wire::TcpFlag::Syn)) {
                    header.flags = wire::TcpFlags::from_raw(
                        static_cast<std::uint8_t>(header.flags.raw() & ~std::to_underlying(wire::TcpFlag::Syn)));
                    --trim;
                }
                assert(trim <= segment.payload_length);
                segment.payload_offset = static_cast<std::uint16_t>(segment.payload_offset + trim);
                segment.payload_length = static_cast<std::uint16_t>(segment.payload_length - trim);
                header.sequence        = next;
            }
            if (window == 0 || end.after(edge)) {
                return drop(context_.counters.dropped_out_of_window);
            }
            return true;
        }

        /// A RST in the window: a passive open nobody took is freed silently; any other connection is reset.
        void receive_reset(Block& block) noexcept {
            if (block.state == State::SynReceived && !block.owned) {
                ++context_.counters.handshakes_failed;
                free_slot(block);
                return;
            }
            ++context_.counters.connections_reset;
            set_closed(block);
            raise(block, stream::Event::Reset);
        }

        /**
         * The ACK. In SynReceived it completes the passive open. An ACK of new data frees what it covers, samples the
         * round trip and drives loss recovery (`acknowledge_data`); our FIN acknowledged moves the close on. A
         * duplicate ACK, of something in flight, is judged against the window as it stands, before this segment
         * updates it. RFC 793 updates the window only when SND.UNA <= SEG.ACK <= SND.NXT: a stale ACK carries a stale
         * window. False when the segment goes no further: no ACK, an ACK of what was never sent, or a connection that
         * finished.
         */
        [[nodiscard]] bool receive_ack(Block& block, const Segment& segment) noexcept {
            const wire::TcpHeader& header = segment.header;
            if (!header.flags.has(wire::TcpFlag::Ack)) {
                ++context_.counters.dropped_unexpected;
                return false;
            }
            const Sequence ack = header.acknowledgement;
            if (block.state == State::SynReceived) {
                if (ack != block.send.nxt) {
                    ++context_.counters.dropped_unexpected;
                    return false;
                }
                const typename Transmitter::Freed freed =
                    transmitter_.acknowledge(block, ack);  // frees the SYN-ACK's entry
                block.send.una = ack;
                on_acknowledged(block, freed, segment.ts_echo);  // a sample, and the timer stopped with the queue empty
                block.state = State::Established;
                transmitter_.reserve_fin_node(block);
                block.recovery.establish(block.mss());
                block.owned = true;
                ++context_.counters.connections_accepted;
                raise(block, stream::Event::Accepted);
                // The same segment may carry data or a FIN: keep going.
            }
            if (ack.after(block.send.nxt)) {
                ++context_.counters.dropped_unexpected;  // acknowledges what was never sent
                ack_now(block);
                return false;
            }
            const std::size_t writable_before = transmitter_.writable(block);
            const bool duplicate              = segment.payload_length == 0 && !header.flags.has(wire::TcpFlag::Syn) &&
                                                !header.flags.has(wire::TcpFlag::Fin) && ack == block.send.una &&
                                                header.window == block.send.wnd && transmitter_.in_flight(block);
            if (ack.after(block.send.una)) {
                acknowledge_data(block, ack, segment.ts_echo);
                raise(block, stream::Event::Acked);
                if (block.send.fin_sent && ack == block.send.nxt) {  // our FIN is acknowledged: the queue is empty
                    switch (block.state) {
                        case State::FinWait1:
                            block.state = State::FinWait2;
                            break;
                        case State::Closing:
                        case State::LastAck:
                            block.finish_pending = true;
                            break;
                        default:
                            break;
                    }
                }
            } else if (duplicate) {
                on_duplicate_ack(block, ack);
            }
            if (!ack.before(block.send.una)) {
                block.send.update_window(header);
                update_probing(block);  // a window that opened ends probing; the comparison below raises Writable
                send_unsent(block);     // and sends the FIN a close left waiting for it
            }
            if (transmitter_.writable(block) > writable_before) {
                raise(block, stream::Event::Writable);
            }
            maybe_finish(block);
            return block.state != State::Closed;
        }

        /**
         * In-order payload, held for the application and acknowledged by the ACK policy. Refused, with an immediate
         * ACK that reports the last byte accepted, past the peer's FIN, past the per-connection cap of held packets
         * or with the shard's pool empty; the FIN behind refused data is not consumed. False when refused.
         */
        [[nodiscard]] bool receive_payload(Block& block, Datagram& datagram, const Segment& segment) noexcept {
            if (block.receive.closed()) {  // the peer's sequence space ended at its FIN, whatever state we are in
                ++context_.counters.dropped_unexpected;
                ack_now(block);
                return false;
            }
            if (block.receive.segments() >= context_.config.receive_segments) {
                ++context_.counters.dropped_no_slot;
                ack_now(block);
                return false;
            }
            const auto offset =
                static_cast<std::uint16_t>(datagram.l3_offset + datagram.l3_length + segment.payload_offset);
            if (!block.receive.hold(context_.pool, datagram.packet, offset, segment.payload_length)) {
                ++context_.counters.dropped_no_node;
                ack_now(block);
                return false;
            }
            raise(block, stream::Event::Readable);
            // A full-sized segment is our MSS less the timestamp option a timestamped peer puts in every segment.
            const std::size_t full_size = context_.mss - block.timestamps.option_size();
            if (block.ack_policy.on_data(segment.payload_length, full_size, block.receive.next()) ==
                detail::AckPolicy::Ack::Now) {
                ack_now(block);
            } else {
                transmitter_.queue_ack(block);
            }
            return true;
        }

        void receive_fin(Block& block) noexcept {
            block.receive.take_fin();
            raise(block, stream::Event::PeerClosed);
            switch (block.state) {
                case State::Established:
                    block.state = State::CloseWait;
                    transmitter_.queue_ack(block);  // the application's close may carry it
                    break;
                case State::FinWait1:
                    block.state = State::Closing;
                    ack_now(block);
                    break;
                case State::FinWait2:
                    block.finish_pending = true;
                    ack_now(block);  // the terminal ACK goes before Closed is published; flush retries a refusal
                    break;
                default:
                    transmitter_.queue_ack(block);
                    break;
            }
        }

        /**
         * The end of an acceptable ACK, after its window update: the peer answered, so the probes' count and stamp
         * clear (`ZeroWindowProbe::answered`). A window that opened ends probing; a zero window with nothing in flight
         * starts it: in Established or CloseWait whether or not the application has data, and in any state while a
         * FIN the window did not cover waits in the queue (`close`). A head waiting for the next hop is the ARP poll's.
         */
        void update_probing(Block& block) noexcept {
            block.probe.answered();
            if (block.probing()) {
                if (block.send.wnd > 0) {
                    transmitter_.disarm(block);
                }
                return;
            }
            if (block.send.wnd == 0 && !transmitter_.in_flight(block) &&
                block.timer_kind != detail::TimerKind::ArpPoll &&
                (!block.queue.empty() || block.state == State::Established || block.state == State::CloseWait)) {
                transmitter_.start_probing(block);
            }
        }

        /// The head that never left, the FIN of a close the peer's window did not cover: sent once the window covers
        /// it, as Linux's `tcp_push_pending_frames` on an ACK. A head waiting for the next hop is the ARP poll's. The
        /// send arms the RTO (`transmit_entry`); a refused one leaves the RTO to try again, as `close` does.
        void send_unsent(Block& block) noexcept {
            if (block.queue.empty() || transmitter_.in_flight(block) ||
                block.timer_kind == detail::TimerKind::ArpPoll) {
                return;
            }
            if (const typename Transmitter::SentNode& head = transmitter_.head(block);
                !block.send.covers(head.first + head.length)) {
                return;
            }
            std::ignore = transmitter_.transmit_entry(block, block.queue.head());
            if (!block.timer.armed()) {
                transmitter_.arm_retransmit(block);
            }
        }

        /// An immediate ACK: queued through IP before the next segment is processed; a refusal leaves it pending. When
        /// the close only waited for this ACK, Closed follows it, whichever segment prompted it.
        void ack_now(Block& block) noexcept {
            if (transmitter_.send_pure_ack(block)) {
                maybe_finish(block);
            } else {
                transmitter_.queue_ack(block);
            }
        }

        [[nodiscard]] std::size_t writable(const Block& block) const noexcept {
            return transmitter_.writable(block);
        }

        [[nodiscard]] std::optional<std::span<std::byte>> prepare(Block& block, const std::size_t count) noexcept {
            return transmitter_.prepare(block, count);
        }

        [[nodiscard]] bool commit(Block& block, const std::size_t count) noexcept {
            return transmitter_.commit(block, count);
        }

        /// Advances the held payload's front by `count` bytes, freeing packets as they empty; a reopened edge queues
        /// an update.
        void consume(Block& block, const std::size_t count) noexcept {
            block.receive.consume(context_.pool, count);
            const bool receiving =
                block.state == State::Established || block.state == State::FinWait1 || block.state == State::FinWait2;
            if (!receiving) {
                return;  // the peer sends no more data, or the connection is closed: no offer to make
            }
            if (transmitter_.offer_of(block) != block.receive.offered()) {
                transmitter_.queue_ack(block);  // an ordinary window update, including the reopening of a zero window
            }
        }

        // The control path: events, slots, the opening, the retransmit and probe timers, close, abort and release.

        void raise(Block& block, const stream::Event event) noexcept {
            block.events |= event;
            if (!events_.contains(block)) {
                events_.push_back(block);
            }
        }

        /// The flow's identity, indexed in the table under `base_hash`; the caller found the flow free.
        void
        bind_flow(Block& block, const Endpoint local, const Endpoint remote, const std::uint32_t base_hash) noexcept {
            block.local     = local;
            block.remote    = remote;
            block.key       = FlowKey{.remote = remote.address, .remote_port = remote.port, .local_port = local.port};
            block.base_hash = base_hash;
            [[maybe_unused]] const bool inserted = table_.insert(block.key, block.index, base_hash);
            assert(inserted && "the caller found the flow free");
            block.indexed = true;
        }

        /// Numbers the connection's SYN from a fresh ISN and starts its sender from scratch: RFC 6582's `recover` at
        /// the ISN, no round-trip sample, the initial RTO and no retry.
        void open(Block& block) noexcept {
            block.send.open(isn_.next());
            block.recovery.open(block.send.iss);
            block.rtt.reset(context_.config.rto_initial);
            block.retransmit_count = 0;
        }

        void free_slot(Block& block) noexcept {
            // Every live connection is indexed by connect or passive_open; a free slot is not, and pushing its index
            // again would hand one slot to two opens. A release build ignores the second call.
            assert(block.indexed && "release of a free slot");
            if (!block.indexed) {
                return;
            }
            transmitter_.disarm(block);
            std::ignore = table_.erase(block.key, block.base_hash);
            return_chain(block);
            transmitter_.drop_queue(block);
            block.prepared.reset();
            events_.remove(block);
            transmitter_.forget(block);
            block.clear();
            table_.free_slot(block.index);
        }

        void return_chain(Block& block) noexcept {
            block.receive.release(context_.pool);
        }

        /**
         * SND.UNA moved forward. The retry count clears. The round-trip sample follows Linux's `tcp_clean_rtx_queue`
         * and `tcp_ack_update_rtt`: from the oldest entry the ACK freed, measured from its transmission, when no entry
         * it freed left more than once (Karn's rule); else, on a connection with timestamps, from the ACK's echo, which
         * names the transmission it answers, so the echo on the ACK of a resend ends the backoff too
         * (`Timestamps::sample`). The timer restarts from now while anything is in flight and stops when nothing is,
         * RFC 6298's rule, which covers RFC 6582's re-arm on a partial ACK; a head that never left, the FIN a close
         * holds for the window, is `send_unsent`'s or the probes'. A partial ACK frees nothing, samples only from its
         * echo, and restarts the timer. Returns the sample taken, if any.
         */
        std::optional<core::Duration> on_acknowledged(Block& block,
                                                      const typename Transmitter::Freed& freed,
                                                      const std::optional<std::uint32_t> echo) noexcept {
            block.retransmit_count = 0;
            const std::optional<core::Duration> sample =
                freed.length > 0 && !freed.resent
                    ? std::optional{std::max<core::Duration>(context_.now - freed.sent, core::Duration::zero())}
                    : block.timestamps.sample(echo, context_.now);
            if (sample) {
                block.rtt.sample(*sample, context_.config.rto_min, context_.config.rto_max);
                ++context_.counters.rtt_samples;
            }
            if (transmitter_.in_flight(block)) {
                transmitter_.arm_retransmit(block);
            } else {
                transmitter_.disarm(block);
            }
            return sample;
        }

        /**
         * An ACK of new data on a synchronized connection: the entries it frees, the sample and the timer
         * (`on_acknowledged`), then loss recovery (`LossRecovery::on_ack`), which hears the ACK with what the policy
         * reads and says what to resend: the head as a fast retransmit on a partial ACK in fast recovery, or the new
         * head in the loss episode after a timeout, counted in `retransmits`.
         */
        void acknowledge_data(Block& block, const Sequence ack, const std::optional<std::uint32_t> echo) noexcept {
            const std::uint32_t flight_before = block.send.flight();
            const auto acked                  = static_cast<std::uint32_t>(ack - block.send.una);
            const typename Transmitter::Freed freed =
                transmitter_.acknowledge(block, ack);  // a partial entry stays whole
            block.send.una                          = ack;
            const std::optional<core::Duration> rtt = on_acknowledged(block, freed, echo);
            const AckInfo info{.acked             = acked,
                               .flight            = flight_before,
                               .mss               = block.mss(),
                               .rtt               = rtt,
                               .now               = context_.now,
                               .sent              = freed.newest_sent,
                               .delivered         = block.queue.delivered(),
                               .delivered_at_send = freed.delivered_at_send,
                               .app_limited       = freed.app_limited};
            switch (block.recovery.on_ack(info, ack, block.send)) {
                case detail::Resend::Fast:
                    fast_retransmit(block);
                    break;
                case detail::Resend::Head:
                    if (!block.queue.empty()) {
                        std::ignore = transmitter_.resend_head(block);
                    }
                    ++context_.counters.retransmits;
                    break;
                case detail::Resend::None:
                    break;
            }
        }

        /// RFC 5681's duplicate ACK, counted; the third may start fast recovery (`LossRecovery::on_duplicate`), which
        /// resends the head at once.
        void on_duplicate_ack(Block& block, const Sequence ack) noexcept {
            ++context_.counters.duplicate_acks;
            if (block.recovery.on_duplicate(ack, block.send, block.mss()) == detail::Resend::Fast) {
                fast_retransmit(block);
                ++context_.counters.recoveries;
            }
        }

        /**
         * The head again, driven by the third duplicate or a partial ACK, counted in `fast_retransmits`. When it
         * leaves, in place or as a copy, the RTO restarts from now, as Linux re-arms it on resending the head
         * (`tcp_xmit_retransmit_queue`), so the timer the first send armed does not expire before the resend's ACK can
         * return. A resend deferred, or refused, leaves the timer as it was.
         */
        void fast_retransmit(Block& block) noexcept {
            ++context_.counters.fast_retransmits;
            if (!transmitter_.in_flight(block)) {
                return;  // nothing in flight: the head, if any, waits for the window
            }
            if (transmitter_.resend_head(block) && !transmitter_.deferred(block)) {
                transmitter_.arm_retransmit(block);
            }
        }

        /// The wheel's callback: the node's block, then the stack's timer handling.
        static void on_timer_fired(loop::Timer& timer) noexcept {
            Block& block = static_cast<typename Block::TimerNode&>(timer).block;
            block.stack.on_timer(block);
        }

        /// The unconsumed payloads of the block's held packets, for the handle.
        [[nodiscard]] typename ConnectionType::View unread(const Block& block) noexcept {
            return block.receive.view(context_.pool);
        }

        /// Called by the wheel with the timer unarmed; its kind says what for.
        void on_timer(Block& block) noexcept {
            const detail::TimerKind kind = std::exchange(block.timer_kind, detail::TimerKind::None);
            assert(kind != detail::TimerKind::None && "the wheel fires only an armed timer");
            switch (kind) {
                case detail::TimerKind::Retransmit:
                    on_retransmit_timeout(block);
                    break;
                case detail::TimerKind::ArpPoll:
                    on_arp_poll(block);
                    break;
                case detail::TimerKind::Probe:
                    on_probe_timeout(block);
                    break;
                case detail::TimerKind::None:
                    break;
            }
        }

        /**
         * The RTO ran out: one retry, the head of the queue again. First the user timeout, counted from the head's
         * first transmission on a synchronized connection, and then the cap, `retries` in the handshake and
         * `data_retries` after it, may end the connection instead. The first expiry since an ACK of new data starts
         * the loss episode (`LossRecovery::on_timeout`); each expiry doubles the RTO up to `rto_max` and re-arms. A
         * resend deferred or refused counts as the retry all the same: the next expiry tries again. A first send, of a
         * SYN, SYN-ACK or FIN the device refused before, that finds no next hop arms the ARP poll instead
         * (`transmit_entry`), and the expiry leaves the timer to it: nothing left, so no backoff; the poll arms the RTO
         * once the send leaves, or ends the connection `Unresolved`.
         */
        void on_retransmit_timeout(Block& block) noexcept {
            assert(!block.queue.empty() && "the RTO runs while something is queued");
            if (const typename Transmitter::SentNode* head = transmitter_.user_timed_head(block);
                head != nullptr && context_.now - head->first_sent >= context_.config.user_timeout) {
                time_out(block, /*by_user_timeout=*/true);
                return;
            }
            if (const bool handshake = block.state == State::SynSent || block.state == State::SynReceived;
                block.retransmit_count >= (handshake ? context_.config.retries : context_.config.data_retries)) {
                time_out(block, /*by_user_timeout=*/false);
                return;
            }
            if (++block.retransmit_count == 1) {
                block.recovery.on_timeout(block.send, block.mss());
            }
            std::ignore = transmitter_.resend_head(block);
            ++context_.counters.retransmits;
            if (block.timer_kind == detail::TimerKind::ArpPoll) {
                return;  // a first send that found no next hop: the poll retries it, and arms the RTO once it leaves
            }
            block.rtt.back_off(context_.config.rto_max);
            transmitter_.arm_retransmit(block);
        }

        /// The ARP poll: the SYN, SYN-ACK or FIN that never left is tried again, as no retry. Once it leaves, or the
        /// device refuses it, the RTO takes over; a next hop still silent after `unresolved_polls` polls ends the
        /// connection with `Unresolved`.
        void on_arp_poll(Block& block) noexcept {
            assert(!block.queue.empty() && "the poll retries an entry that never left");
            if (const std::expected<void, net::SendError> sent = transmitter_.transmit_entry(block, block.queue.head());
                sent || sent.error() != net::SendError::Unresolved) {
                transmitter_.arm_retransmit(block);
                return;
            }
            if (++block.unresolved_polls >= context_.config.unresolved_polls) {
                fail_unresolved(block);  // the bound Linux's ARP probes give: the next hop is not going to answer
                return;
            }
            transmitter_.arm_poll(block);
        }

        /// The probe timer ran out with the peer's window still zero (`ZeroWindowProbe::expire`): the user timeout or
        /// `data_retries` unanswered probes end the connection; else one probe goes, a refused one counted all the
        /// same, and the next is armed.
        void on_probe_timeout(Block& block) noexcept {
            assert(!transmitter_.in_flight(block) && block.send.wnd == 0 &&
                   (!block.queue.empty() || block.state == State::Established || block.state == State::CloseWait) &&
                   "a probe runs only while the window is zero and nothing is in flight");
            switch (block.probe.expire(
                context_.now, context_.config.user_timeout, context_.config.data_retries, context_.config.rto_max)) {
                case detail::ZeroWindowProbe::Expiry::UserTimedOut:
                    time_out(block, /*by_user_timeout=*/true);
                    return;
                case detail::ZeroWindowProbe::Expiry::TimedOut:
                    time_out(block, /*by_user_timeout=*/false);
                    return;
                case detail::ZeroWindowProbe::Expiry::Probe:
                    break;
            }
            std::ignore = transmitter_.send_probe(block);  // a refusal was counted by the send
            transmitter_.arm_probe(block);
        }

        /// The retry cap or the user timeout ran out: `TimedOut` for an owned connection, counted apart when the user
        /// timeout ended it.
        void time_out(Block& block, const bool by_user_timeout) noexcept {
            if (!block.owned) {
                ++context_.counters.handshakes_failed;  // a passive open nobody took: freed silently
                free_slot(block);
                return;
            }
            ++context_.counters.connections_timed_out;
            if (by_user_timeout) {
                ++context_.counters.connections_user_timed_out;
            }
            set_closed(block);
            raise(block, stream::Event::TimedOut);
        }

        /// The next hop never answered ARP: a failure of its own, told apart from a segment that left and was ignored.
        void fail_unresolved(Block& block) noexcept {
            if (!block.owned) {
                ++context_.counters.handshakes_failed;  // a passive open nobody took: freed silently
                free_slot(block);
                return;
            }
            ++context_.counters.connections_unresolved;
            set_closed(block);
            raise(block, stream::Event::Unresolved);
        }

        /// Closed, with nothing armed, pending or queued: after a terminal event nothing is resent, so the
        /// retransmission queue is dropped, its packets back now or when the device returns them. Held segments stay
        /// until release, and so does an open preparation: the application may still write into its span, and the
        /// next commit refuses it.
        void set_closed(Block& block) noexcept {
            if (block.state == State::SynSent) {
                block.timestamps.withdraw();  // the offer was never answered: nothing was negotiated
            }
            block.state = State::Closed;
            transmitter_.disarm(block);
            block.unresolved_polls = 0;
            transmitter_.forget(block);
            block.finish_pending = false;
            transmitter_.drop_queue(block);  // and off the deferred list
        }

        void finish(Block& block) noexcept {
            set_closed(block);
            ++context_.counters.connections_closed;
            raise(block, stream::Event::Closed);
        }

        /// The last ACK the peer is owed must be queued before Closed is published; flush retries the rest.
        void maybe_finish(Block& block) noexcept {
            if (block.finish_pending && !transmitter_.ack_pending(block)) {
                finish(block);
            }
        }

        /// FIN after what was committed: Established moves to FinWait1, CloseWait to LastAck; elsewhere nothing. An
        /// open preparation stays the application's: its next commit refuses it, as the connection left the sendable
        /// states. The FIN's entry is the node Established set aside, so the pool never refuses it. A FIN the peer's
        /// window does not cover, Linux's `tcp_snd_wnd_test`, waits in the queue, unsent, for the ACK that opens the
        /// window (`send_unsent`), while the probes go on; a Linux peer drops a FIN sent into its zero window. The
        /// congestion window does not gate a FIN, as it does not in Linux.
        void close(Block& block) noexcept {
            switch (block.state) {
                case State::Established:
                    block.state = State::FinWait1;
                    break;
                case State::CloseWait:
                    block.state = State::LastAck;
                    break;
                default:
                    return;
            }
            const Sequence fin  = block.send.nxt;
            const bool idle     = !transmitter_.in_flight(block);  // else the RTO already runs for the data
            block.send.fin_sent = true;
            block.send.nxt      = fin + 1;  // the FIN takes its number whether or not it left: the timer resends it
            const std::optional<std::uint32_t> entry = transmitter_.enqueue_fin(block, fin);
            if (!entry) {
                ++context_.counters
                      .send_refused;  // unreachable by construction; a release build resets rather than hangs
                abort(block);
                return;
            }
            if (!block.send.covers(fin + 1)) {
                if (idle && !block.probing()) {
                    transmitter_.start_probing(
                        block);  // a zero window with nothing in flight is probed already, as a rule
                }
                return;
            }
            std::ignore = transmitter_.transmit_entry(block, *entry);
            if (idle && (!block.timer.armed() || block.probing())) {
                transmitter_.arm_retransmit(block);  // refused: the RTO tries again, and takes the timer from a probe
            }
        }

        void abort(Block& block) noexcept {
            if (block.state == State::Closed) {
                return;
            }
            // RFC 9293's ABORT in SYN-SENT: the peer holds nothing of ours to take back, so no RST leaves.
            if (block.state != State::SynSent) {
                std::ignore = transmitter_.send_reset(block);
            }
            set_closed(block);
            raise(block, stream::Event::Closed);
        }

        void release(Block& block) noexcept {
            if (block.state != State::Closed) {
                abort(block);
            }
            free_slot(block);
        }

        detail::StackContext<Ip, CongestionControl> context_;
        detail::Transmitter<Ip, CongestionControl> transmitter_;
        detail::ConnectionTable table_;
        std::deque<ConnectionType> connections_ = {};
        EventList events_                       = {};
        detail::IsnGenerator isn_{};
    };

}  // namespace aloe::tcp
