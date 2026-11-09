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
#include <detail/segment_writer.hpp>
#include <detail/sent_segment_pool.hpp>
#include <detail/stack_context.hpp>
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
              wheel_{wheel},
              send_pool_{context_.config.send_pool},
              table_{context_.config, ip.queue().steering()},
              writer_{context_.ip, context_.counters} {
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
                wheel_.cancel(block.timer);
                block.prepared.reset();
                return_chain(block);
                drop_queue(block);
            }
        }

        /// Records the stamp, publishes deferred retry hints, then processes each datagram. Every packet is moved
        /// out or released. An empty span is valid.
        void process(const std::span<net::Datagram<Packet>> segments, const core::TimePoint now) noexcept {
            context_.now = now;
            for (Block* c = retries_.pop_front(); c != nullptr; c = retries_.pop_front()) {
                raise(*c, stream::Event::Writable);  // the deferred retry hint, at most one per marked connection
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
            for (Block* c = deferred_.front(); c != nullptr;) {
                Block* next = DeferredList::next_of(*c);  // a resend that leaves unlinks c
                std::ignore = resend_head(*c);
                c           = next;
            }
            for (Block* c = acks_.front(); c != nullptr;) {
                Block* next = AckList::next_of(*c);  // a successful send unlinks c
                if (send_pure_ack(*c)) {
                    maybe_finish(*c);
                }
                c = next;
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
            if (send_pool_.available() == 0) {
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
            std::ignore = queue_control(block, wire::TcpFlags{wire::TcpFlag::Syn}, block.send.iss);
            if (!block.timer.armed()) {
                arm_retransmit(block);
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
            return conn.block_.queue.at(send_pool_, position);
        }
        [[nodiscard]] std::size_t send_nodes_available() const noexcept {
            return send_pool_.available();
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

        using Block        = detail::ControlBlock<Ip, CongestionControl>;
        using Datagram     = net::Datagram<Packet>;
        using EventList    = detail::ConnectionList<Block, &Block::event_link>;
        using AckList      = detail::ConnectionList<Block, &Block::ack_link>;
        using RetryList    = detail::ConnectionList<Block, &Block::retry_link>;
        using DeferredList = detail::ConnectionList<Block, &Block::deferred_link>;
        using SentNode     = detail::SentSegment<Packet>;
        using Freed        = detail::RetransmissionQueue<Packet>::Freed;

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
                    reply_reset(datagram, segment);
                }
                return;
            }
            switch (Block& block = context_.blocks[*found]; block.state) {
                case State::Closed:  // still indexed because not released
                    ++context_.counters.dropped_closed;
                    if (!header->flags.has(wire::TcpFlag::Rst)) {
                        reply_reset(datagram, segment);
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
            if (send_pool_.available() == 0) {
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
            std::ignore = queue_control(block, wire::TcpFlags{wire::TcpFlag::Syn, wire::TcpFlag::Ack}, block.send.iss);
            if (!block.timer.armed()) {
                arm_retransmit(block);
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
            block.timestamps.answer(segment.ts_value, context_.now);         // our offer, answered
            const Freed freed = acknowledge(block, header.acknowledgement);  // frees the SYN's entry
            block.send.una    = header.acknowledgement;
            on_acknowledged(block, freed, segment.ts_echo);  // a sample, and the timer stopped with the queue empty
            block.receive.open(header.sequence + 1,
                               context_.byte_budget);         // the initial offer, carried by our first ACK
            block.timestamps.sent_ack(block.receive.next());  // as Linux's rcv_wup: data on the SYN-ACK may update it
            block.send.peer_mss = segment.mss.value_or(default_peer_mss);
            block.send.synchronize(header);
            block.state = State::Established;
            reserve_fin_node(block);
            block.recovery.establish(block.mss());
            update_probing(block);  // a SYN-ACK offering no window starts probing as any ACK does
            ++context_.counters.connections_opened;
            queue_ack(block);  // deferred: a commit on the Connected event completes the handshake with data
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
                    std::ignore = resend_head(block);  // a duplicate SYN: the SYN-ACK again, from its entry
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
                const Freed freed = acknowledge(block, ack);  // frees the SYN-ACK's entry
                block.send.una    = ack;
                on_acknowledged(block, freed, segment.ts_echo);  // a sample, and the timer stopped with the queue empty
                block.state = State::Established;
                reserve_fin_node(block);
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
            const std::size_t writable_before = writable(block);
            const bool duplicate              = segment.payload_length == 0 && !header.flags.has(wire::TcpFlag::Syn) &&
                                                !header.flags.has(wire::TcpFlag::Fin) && ack == block.send.una &&
                                                header.window == block.send.wnd && in_flight(block);
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
            if (writable(block) > writable_before) {
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
                queue_ack(block);
            }
            return true;
        }

        void receive_fin(Block& block) noexcept {
            block.receive.take_fin();
            raise(block, stream::Event::PeerClosed);
            switch (block.state) {
                case State::Established:
                    block.state = State::CloseWait;
                    queue_ack(block);  // the application's close may carry it
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
                    queue_ack(block);
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
                    disarm(block);
                }
                return;
            }
            if (block.send.wnd == 0 && !in_flight(block) && block.timer_kind != detail::TimerKind::ArpPoll &&
                (!block.queue.empty() || block.state == State::Established || block.state == State::CloseWait)) {
                start_probing(block);
            }
        }

        /// The head that never left, the FIN of a close the peer's window did not cover: sent once the window covers
        /// it, as Linux's `tcp_push_pending_frames` on an ACK. A head waiting for the next hop is the ARP poll's. The
        /// send arms the RTO (`transmit_entry`); a refused one leaves the RTO to try again, as `close` does.
        void send_unsent(Block& block) noexcept {
            if (block.queue.empty() || in_flight(block) || block.timer_kind == detail::TimerKind::ArpPoll) {
                return;
            }
            if (const SentNode& head = send_pool_.node(block.queue.head());
                !block.send.covers(head.first + head.length)) {
                return;
            }
            std::ignore = transmit_entry(block, block.queue.head());
            if (!block.timer.armed()) {
                arm_retransmit(block);
            }
        }

        /// RFC 793's reset for a segment with no connection: with ACK, sequence at its acknowledgement;
        /// without, sequence zero and acknowledgement past everything it occupied.
        void reply_reset(const Datagram& datagram, const Segment& segment) noexcept {
            detail::OutgoingSegment reply{
                .header = wire::TcpHeader{.source_port      = segment.header.destination_port,
                                          .destination_port = segment.header.source_port}
            };
            if (segment.header.flags.has(wire::TcpFlag::Ack)) {
                reply.header.sequence = segment.header.acknowledgement;
                reply.header.flags    = wire::TcpFlags{wire::TcpFlag::Rst};
            } else {
                reply.header.acknowledgement = segment.header.sequence + segment.length();
                reply.header.flags           = wire::TcpFlags{wire::TcpFlag::Rst, wire::TcpFlag::Ack};
            }
            if (writer_.send_detached(datagram.source, reply, context_.now)) {
                ++context_.counters.resets_sent;
            }
        }

        // The transmit path: every segment of a connection leaves through emit; the retransmission queue keeps those
        // that occupy sequence space; pure ACKs and their policy.

        /// The right edge an ACK offers now (`ReceiveQueue::offer`).
        [[nodiscard]] Sequence offer_of(const Block& block) const noexcept {
            return block.receive.offer(context_.byte_budget, context_.config.receive_segments);
        }

        /// A segment with ACK left offering `offer`: the edge is recorded and the ACK that waited is done.
        void record_offer(Block& block, const Sequence offer) noexcept {
            if (block.receive.record_offer(offer)) {
                ++context_.counters.window_updates;
            }
            acks_.remove(block);
            block.ack_policy.acknowledged();
        }

        /// The timestamp option a segment of the connection carries now: the clock's value and TS.Recent as the echo.
        [[nodiscard]] std::optional<detail::TimestampOption> timestamp_option(const Block& block) const noexcept {
            if (!block.timestamps.enabled()) {
                return std::nullopt;
            }
            return detail::TimestampOption{.value = block.timestamps.value(context_.now),
                                           .echo  = block.timestamps.echo()};
        }

        /**
         * The one send function: every segment of a connection leaves here, kept or not, framed by the writer. The
         * option block carries the MSS on a handshake segment and the timestamps on every segment of a connection
         * that sends them. A segment with ACK offers the receive window (`offer_of`). The packet stays with the
         * caller. Only a success records the offer and Last.ACK.sent and clears the pending ACK; a failure, counted
         * by the writer, leaves the packet framed.
         */
        [[nodiscard]] std::expected<void, net::SendError> emit(Block& block,
                                                               Packet& packet,
                                                               const bool framed,
                                                               const Sequence sequence,
                                                               const wire::TcpFlags flags,
                                                               const bool handshake) noexcept {
            const bool with_ack  = flags.has(wire::TcpFlag::Ack);
            const Sequence next  = block.receive.next();
            const Sequence offer = with_ack ? offer_of(block) : block.receive.offered();
            const detail::OutgoingSegment segment{
                .header     = wire::TcpHeader{.source_port      = block.local.port,
                                              .destination_port = block.remote.port,
                                              .sequence         = sequence,
                                              .acknowledgement  = with_ack ? next : Sequence{},
                                              .flags            = flags,
                                              .window = with_ack ? static_cast<std::uint16_t>(offer.value - next.value)
                                                                 : static_cast<std::uint16_t>(context_.byte_budget)},
                .mss        = handshake ? std::optional{context_.mss}
                      : std::nullopt,
                .timestamps = timestamp_option(block)
            };
            const std::expected<void, net::SendError> sent =
                writer_.send(block.frame, block.remote.address, packet, framed, segment, context_.now);
            if (sent && with_ack) {
                record_offer(block, offer);
                block.timestamps.sent_ack(next);
            }
            return sent;
        }

        /// Established: one node of the pool is set aside for the connection's FIN. The ACK that brought the
        /// connection here freed the handshake's node, so one is free to set aside.
        void reserve_fin_node(Block& block) noexcept {
            [[maybe_unused]] const bool reserved = block.queue.reserve_fin(send_pool_);
            assert(reserved && "the handshake's node just came back");
        }

        /// A queue entry for a segment about to leave, with what a rate sample reads: the delivered count, and whether
        /// the windows allowed more than it carries.
        [[nodiscard]] SentNode entry(const Block& block,
                                     Packet&& packet,
                                     const std::uint16_t payload,
                                     const wire::TcpFlags flags,
                                     const Sequence first,
                                     const std::uint32_t length) const noexcept {
            return SentNode{.packet            = std::move(packet),
                            .first             = first,
                            .length            = length,
                            .payload           = payload,
                            .flags             = flags,
                            .delivered_at_send = block.queue.delivered(),
                            .app_limited       = block.recovery.room(block.send) > payload};
        }

        /**
         * An entry for a segment about to leave, linked at the tail of the queue. `packet` may be empty for a SYN or
         * SYN-ACK, which `transmit_entry` gives one. Nothing, the packet back to its pool, when no node is free or a
         * data segment would pass the send cap. The FIN's entry is `enqueue_fin`.
         */
        [[nodiscard]] std::optional<std::uint32_t> enqueue(Block& block,
                                                           Packet&& packet,
                                                           const std::uint16_t payload,
                                                           const wire::TcpFlags flags,
                                                           const Sequence first,
                                                           const std::uint32_t length) noexcept {
            if (payload > 0 && block.queue.size() >= context_.config.send_segments) {
                return std::nullopt;
            }
            return block.queue.push(send_pool_, entry(block, std::move(packet), payload, flags, first, length));
        }

        /// The FIN's entry, on the node Established set aside for it; the stack's own, so the send cap does not hold
        /// it.
        [[nodiscard]] std::optional<std::uint32_t> enqueue_fin(Block& block, const Sequence fin) noexcept {
            return block.queue.push_fin(
                send_pool_, entry(block, Packet{}, 0, wire::TcpFlags{wire::TcpFlag::Fin, wire::TcpFlag::Ack}, fin, 1));
        }

        /**
         * One transmission of a queued entry through `emit`; its packet must be back from the device. A SYN, SYN-ACK
         * or FIN entry without a packet takes one from IP first. A success stamps the entry and counts a control
         * segment for a SYN or FIN; either way the packet holds its frame from then on. The first transmission of the
         * only entry, a segment sent into an empty queue, arms the timer: the RTO when it left, the ARP poll when a
         * SYN, SYN-ACK or FIN found no next hop. A data segment that found none is not kept: `commit` refuses it.
         */
        [[nodiscard]] std::expected<void, net::SendError> transmit_entry(Block& block,
                                                                         const std::uint32_t index) noexcept {
            SentNode& node = send_pool_.node(index);
            assert(!node.packet.lent() && "a queued packet is written only once the device gave it back");
            const bool first_into_empty = node.transmits == 0 && index == block.queue.head() && block.queue.size() == 1;
            if (node.packet.empty()) {
                std::optional<Packet> fresh = context_.ip.allocate();
                if (!fresh) {
                    ++context_.counters.send_refused;
                    return std::unexpected{net::SendError::Refused};
                }
                node.packet = std::move(*fresh);
            }
            const std::expected<void, net::SendError> sent =
                emit(block, node.packet, node.framed, node.first, node.flags, node.flags.has(wire::TcpFlag::Syn));
            node.framed = true;
            if (!sent) {
                if (first_into_empty && node.payload == 0 && sent.error() == net::SendError::Unresolved) {
                    arm_poll(block);
                }
                return sent;
            }
            record_transmission(block, node);
            if (first_into_empty) {
                arm_retransmit(block);
            }
            return {};
        }

        /// A transmission of the entry left, from its packet or as a copy: stamped, on the queue too when it carries
        /// payload (the idle restart's clock), counted as a control segment for a SYN or FIN, and the ARP poll's count
        /// cleared.
        void record_transmission(Block& block, SentNode& node) noexcept {
            block.queue.sent(node, context_.now);
            if (node.control()) {
                ++context_.counters.control_segments_sent;
            }
            block.unresolved_polls = 0;
        }

        /// Queues a SYN or SYN-ACK at `sequence` and transmits it. The entry stays when the send fails, for the
        /// timer; `Refused`, counted, when no node is free.
        [[nodiscard]] std::expected<void, net::SendError>
        queue_control(Block& block, const wire::TcpFlags flags, const Sequence sequence) noexcept {
            const std::optional<std::uint32_t> entry = enqueue(block, Packet{}, 0, flags, sequence, 1);
            if (!entry) {
                ++context_.counters.send_refused;
                return std::unexpected{net::SendError::Refused};
            }
            return transmit_entry(block, *entry);
        }

        /// The queue frees what `ack` covers (`RetransmissionQueue::acknowledge`); a resend that waited on the deferred
        /// list is moot once its entry is freed. Called before SND.UNA moves.
        [[nodiscard]] Freed acknowledge(Block& block, const Sequence ack) noexcept {
            const Freed freed = block.queue.acknowledge(send_pool_, ack, block.send.una);
            if (freed.length > 0) {
                deferred_.remove(block);
            }
            return freed;
        }

        /**
         * The head entry again. A copy out is reconciled first. Still lent: the device is asked to reclaim, and if it
         * still holds the packet nothing is written into it and the busy rule decides (`resend_busy`). Back: when
         * SND.UNA passed the entry's first byte, the acknowledged front is trimmed off (`SentSegment::trim_to`), and
         * `transmit_entry` sends what is left. IP's error, or nothing when the resend left or was deferred. A send that
         * failed leaves the deferred list too: the timer's next expiry tries again.
         */
        [[nodiscard]] std::expected<void, net::SendError> resend_head(Block& block) noexcept {
            assert(!block.queue.empty() && "a resend with nothing queued");
            SentNode& node = send_pool_.node(block.queue.head());
            node.reconcile();
            if (node.packet.lent() && context_.ip.queue().reclaim() > 0) {
                node.reconcile();  // what the device let go of may be either packet
            }
            if (node.packet.lent()) {
                return resend_busy(block, node);
            }
            if (block.send.una.after(node.first)) {
                node.trim_to(block.send.una);
            }
            deferred_.remove(block);  // the packet is back: whatever IP answers, this resend is not waiting any more
            const std::expected<void, net::SendError> sent = transmit_entry(block, block.queue.head());
            if (sent) {
                node.deferrals = 0;
            }
            return sent;
        }

        /**
         * The busy rule, for a head whose packet the device still holds after a reclaim. A resend at the stamp of the
         * last deferral is that deferral again, since the device had no time to finish: it stays on the deferred list
         * and counts nothing. Below `retransmit_defers` deferrals the resend waits for the next flush, counted once
         * per deferral; at the bound, with no copy out, the copy goes (`send_copy`); with a copy out and both packets
         * lent, it waits again.
         */
        [[nodiscard]] std::expected<void, net::SendError> resend_busy(Block& block, SentNode& node) noexcept {
            if (deferred_.contains(block) && node.deferred_at == context_.now) {
                return {};
            }
            if (node.deferrals < context_.config.retransmit_defers) {
                ++node.deferrals;
            } else if (node.copy.empty()) {
                return send_copy(block, node);
            }
            node.deferred_at = context_.now;
            if (!deferred_.contains(block)) {
                deferred_.push_back(block);
            }
            ++context_.counters.retransmits_deferred;
            return {};
        }

        /**
         * The copy fallback: a fresh packet with the lent packet's header bytes and the part of its payload not yet
         * acknowledged, read from the lent packet and never written into it (`SegmentWriter::copy`), framed by `emit`
         * from `max(first, snd_una)` and lent as the entry's second packet; counted in `retransmits_copied`. No packet
         * free counts `allocation_failures` and refuses; a refused send drops the copy. Either way the entry stays as
         * it was and the connection leaves the deferred list: the timer's next expiry tries again.
         */
        [[nodiscard]] std::expected<void, net::SendError> send_copy(Block& block, SentNode& node) noexcept {
            assert(node.framed && node.copy.empty() && "one copy at a time, of a frame that left");
            deferred_.remove(block);
            const Sequence first    = block.send.una.after(node.first) ? block.send.una : node.first;
            const auto acknowledged = static_cast<std::uint32_t>(first - node.first);
            assert((acknowledged == 0 || acknowledged < node.payload) && "only data is acknowledged in part");
            const auto payload            = static_cast<std::uint16_t>(node.payload - acknowledged);
            const std::size_t header_size = node.packet.size() - node.payload;
            std::optional<Packet> copy    = writer_.copy(node.packet, header_size, acknowledged, payload);
            if (!copy) {
                ++context_.counters.allocation_failures;
                return std::unexpected{net::SendError::Refused};
            }
            if (const std::expected<void, net::SendError> sent =
                    emit(block, *copy, true, first, node.flags, node.flags.has(wire::TcpFlag::Syn));
                !sent) {
                return sent;  // the copy goes back to the pool
            }
            node.copy         = std::move(*copy);
            node.copy_first   = first;
            node.copy_payload = payload;
            node.deferrals    = 0;
            record_transmission(block, node);
            ++context_.counters.retransmits_copied;
            return {};
        }

        /// A header-only ACK at `sequence` from a fresh packet: lent to the device and forgotten, never kept. A failure
        /// is counted.
        [[nodiscard]] bool send_bare_ack(Block& block, const Sequence sequence) noexcept {
            std::optional<Packet> packet = context_.ip.allocate();
            if (!packet) {
                ++context_.counters.send_refused;
                return false;
            }
            if (!emit(block, *packet, false, sequence, wire::TcpFlags{wire::TcpFlag::Ack}, false)) {
                return false;  // the packet goes back to the pool
            }
            *packet = Packet{};  // the device frees the buffer when it is done
            return true;
        }

        /// A pure ACK at `snd_nxt`.
        [[nodiscard]] bool send_pure_ack(Block& block) noexcept {
            if (!send_bare_ack(block, block.send.nxt)) {
                return false;
            }
            ++context_.counters.pure_acks_sent;
            return true;
        }

        /// A zero-window probe, Linux's `tcp_xmit_probe_skb`: a pure ACK at `snd_una - 1`, which occupies no sequence
        /// space and which the peer, finding it outside its window, answers with an ACK carrying its window.
        [[nodiscard]] bool send_probe(Block& block) noexcept {
            if (!send_bare_ack(block, Sequence{block.send.una.value - 1})) {
                return false;
            }
            ++context_.counters.zero_window_probes;
            return true;
        }

        /// abort's RST, at `snd_nxt` with the ACK, window and timestamps an ordinary segment carries (RFC 7323: should
        /// carry them, as Linux's does): through `Ipv4::send` and forgotten, since nothing of it is kept.
        [[nodiscard]] bool send_reset(Block& block) noexcept {
            const Sequence next  = block.receive.next();
            const Sequence offer = offer_of(block);
            const detail::OutgoingSegment reset{
                .header     = wire::TcpHeader{.source_port      = block.local.port,
                                              .destination_port = block.remote.port,
                                              .sequence         = block.send.nxt,
                                              .acknowledgement  = next,
                                              .flags            = wire::TcpFlags{wire::TcpFlag::Rst, wire::TcpFlag::Ack},
                                              .window           = static_cast<std::uint16_t>(offer.value - next.value)},
                .timestamps = timestamp_option(block)
            };
            if (!writer_.send_detached(block.remote.address, reset, context_.now)) {
                return false;
            }
            record_offer(block, offer);
            ++context_.counters.resets_sent;
            return true;
        }

        /// An ordinary ACK: on the ACK list, sent by flush unless a segment carrying an ACK leaves first.
        void queue_ack(Block& block) noexcept {
            if (!acks_.contains(block)) {
                acks_.push_back(block);
            }
        }

        /// An immediate ACK: queued through IP before the next segment is processed; a refusal leaves it pending. When
        /// the close only waited for this ACK, Closed follows it, whichever segment prompted it.
        void ack_now(Block& block) noexcept {
            if (send_pure_ack(block)) {
                maybe_finish(block);
            } else {
                queue_ack(block);
            }
        }

        /// A Writable hint at the next process, at most one per marked connection.
        void mark_retry(Block& block) noexcept {
            if (!retries_.contains(block)) {
                retries_.push_back(block);
            }
        }

        /// The MSS, within what the windows allow beyond the flight (`LossRecovery::room`), in Established or
        /// CloseWait with no prepare open and the queue below the send cap; else zero.
        [[nodiscard]] std::size_t writable(const Block& block) const noexcept {
            if (block.prepared || (block.state != State::Established && block.state != State::CloseWait)) {
                return 0;
            }
            if (block.queue.size() >= context_.config.send_segments) {
                return 0;  // the send cap: the ACK that frees an entry raises Writable
            }
            return static_cast<std::size_t>(std::min<std::uint64_t>(block.mss(), block.recovery.room(block.send)));
        }

        /**
         * A span of `min(count, writable())` bytes inside a fresh packet. The packet belongs to the application until
         * `commit` (with any count) or `release`: the span stays valid through any state change in between. Nothing,
         * without allocating, for a count of zero or nothing writable; an allocation failure counts one
         * `allocation_failures`, marks a retry hint and retains nothing.
         */
        [[nodiscard]] std::optional<std::span<std::byte>> prepare(Block& block, const std::size_t count) noexcept {
            assert(!block.prepared && "one prepare at a time: commit first");
            if (count == 0) {
                return std::nullopt;
            }
            const std::size_t room = writable(block);
            if (room == 0) {
                return std::nullopt;  // nothing allocates when nothing is writable
            }
            const std::size_t size       = std::min(count, room);
            std::optional<Packet> packet = context_.ip.allocate();
            if (!packet) {
                ++context_.counters.allocation_failures;
                mark_retry(block);  // the pool may have packets by the next process
                return std::nullopt;
            }
            const std::optional<std::span<std::byte>> span = packet->append(size);
            assert(span && "size is at most the MSS and a packet holds a whole frame");
            if (!span) {
                ++context_.counters.allocation_failures;  // unreachable by construction; counted in a release build
                mark_retry(block);
                return std::nullopt;
            }
            // The span points into the block, which does not move.
            block.prepared = typename Block::Preparation{.packet = std::move(*packet), .size = size};
            return span;
        }

        /**
         * One PSH+ACK segment of the first `count` prepared bytes; `commit(0)` discards the preparation. A segment into
         * an empty queue more than an RTO after the last data left tells the policy of the idle period first. A refusal
         * returns the packet to the pool, leaves every number as it was, marks a retry hint and returns false. A
         * connection that left Established and CloseWait since the prepare drops the packet and refuses the commit
         * with no hint: the events say why. A window that no longer covers `count`, because the peer retracted its
         * right edge or a loss shrank the congestion window since the prepare, refuses the commit with a hint: prepare
         * again for what `writable()` allows. No queue node free refuses it with a hint too.
         */
        [[nodiscard]] bool commit(Block& block, const std::size_t count) noexcept {
            assert(block.prepared && "commit without a prepare");
            if (!block.prepared) {
                ++context_.counters.commits_refused;  // a contract violation, refused in a release build
                return false;
            }
            assert(count <= block.prepared->size && "commit more than was prepared");
            Packet packet              = std::move(block.prepared->packet);
            const std::size_t prepared = block.prepared->size;
            block.prepared.reset();
            if (count == 0) {
                return true;  // discarded: `packet` goes back to the pool; nothing was refused
            }
            if (block.state != State::Established && block.state != State::CloseWait) {
                ++context_.counters.commits_refused;  // closed since the prepare: no hint, the events say why
                return false;
            }
            if (count > writable(block)) {  // the preparation is released, so this is the credit the peer offers now
                ++context_.counters
                      .commits_refused;  // RFC 1122: a sender copes with a shrunk window by not sending into it
                mark_retry(block);
                return false;
            }
            packet.trim_back(prepared - count);
            if (block.queue.idle(context_.now, block.rtt.rto())) {
                block.recovery.on_idle(block.mss());  // RFC 5681's restart after an idle RTO
            }
            const auto payload                       = static_cast<std::uint16_t>(count);
            const std::uint32_t previous_tail        = block.queue.tail();
            const std::optional<std::uint32_t> entry = enqueue(block,
                                                               std::move(packet),
                                                               payload,
                                                               wire::TcpFlags{wire::TcpFlag::Ack, wire::TcpFlag::Psh},
                                                               block.send.nxt,
                                                               payload);
            if (!entry) {
                ++context_.counters.allocation_failures;  // no queue node free: the packet goes back to the pool
                mark_retry(block);
                return false;
            }
            if (!transmit_entry(block, *entry)) {
                block.queue.pop_back(send_pool_, previous_tail);  // the packet goes back with the node
                mark_retry(block);                                // emit counted the refusal
                return false;
            }
            block.send.nxt = block.send.nxt + static_cast<std::uint32_t>(count);
            ++context_.counters.data_segments_sent;
            return true;
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
            if (offer_of(block) != block.receive.offered()) {
                queue_ack(block);  // an ordinary window update, including the reopening of a zero window
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
            disarm(block);
            std::ignore = table_.erase(block.key, block.base_hash);
            return_chain(block);
            drop_queue(block);
            block.prepared.reset();
            events_.remove(block);
            acks_.remove(block);
            retries_.remove(block);
            block.clear();
            table_.free_slot(block.index);
        }

        void return_chain(Block& block) noexcept {
            block.receive.release(context_.pool);
        }

        /// The retransmission queue dropped (`RetransmissionQueue::drop`), and the connection off the deferred list.
        void drop_queue(Block& block) noexcept {
            block.queue.drop(send_pool_);
            deferred_.remove(block);
        }

        /// The RTO from now; under a user timeout on a synchronized connection, never later than the head's first
        /// transmission plus the timeout, Linux's `tcp_clamp_rto_to_user_timeout`, and never earlier than now.
        void arm_retransmit(Block& block) noexcept {
            core::TimePoint deadline = context_.now + block.rtt.rto();
            if (const SentNode* head = user_timed_head(block); head != nullptr) {
                deadline = std::min(deadline, std::max(context_.now, head->first_sent + context_.config.user_timeout));
            }
            block.timer_kind = detail::TimerKind::Retransmit;
            wheel_.arm(block.timer, deadline);
        }

        /// A SYN, SYN-ACK or FIN whose first send found no next hop: tried again every `unresolved_retry`, as no retry.
        void arm_poll(Block& block) noexcept {
            block.timer_kind = detail::TimerKind::ArpPoll;
            wheel_.arm(block.timer, context_.now + context_.config.unresolved_retry);
        }

        /// The next probe (`ZeroWindowProbe::deadline`).
        void arm_probe(Block& block) noexcept {
            block.timer_kind = detail::TimerKind::Probe;
            wheel_.arm(block.timer, block.probe.deadline(context_.now, context_.config.user_timeout));
        }

        /// The peer's window is zero with nothing in flight: the first probe one RTO away.
        void start_probing(Block& block) noexcept {
            block.probe.start(block.rtt.rto());
            arm_probe(block);
        }

        void disarm(Block& block) noexcept {
            wheel_.cancel(block.timer);
            block.timer_kind = detail::TimerKind::None;
        }

        /// The entry the user timeout counts from: the head of the queue once it has left, on a synchronized
        /// connection, with the timeout set. Null otherwise: the handshake is bounded by `retries` alone.
        [[nodiscard]] const SentNode* user_timed_head(const Block& block) const noexcept {
            if (context_.config.user_timeout <= core::Duration::zero() || block.state == State::SynSent ||
                block.state == State::SynReceived || block.queue.empty()) {
                return nullptr;
            }
            const SentNode& head = send_pool_.node(block.queue.head());
            return head.transmits > 0 ? &head : nullptr;
        }

        /// Whether a segment the peer can acknowledge is out (`RetransmissionQueue::in_flight`).
        [[nodiscard]] bool in_flight(const Block& block) const noexcept {
            return block.queue.in_flight(send_pool_);
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
        std::optional<core::Duration>
        on_acknowledged(Block& block, const Freed& freed, const std::optional<std::uint32_t> echo) noexcept {
            block.retransmit_count = 0;
            const std::optional<core::Duration> sample =
                freed.length > 0 && !freed.resent
                    ? std::optional{std::max<core::Duration>(context_.now - freed.sent, core::Duration::zero())}
                    : block.timestamps.sample(echo, context_.now);
            if (sample) {
                block.rtt.sample(*sample, context_.config.rto_min, context_.config.rto_max);
                ++context_.counters.rtt_samples;
            }
            if (in_flight(block)) {
                arm_retransmit(block);
            } else {
                disarm(block);
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
            const std::uint32_t flight_before       = block.send.flight();
            const auto acked                        = static_cast<std::uint32_t>(ack - block.send.una);
            const Freed freed                       = acknowledge(block, ack);  // a partial entry stays whole
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
                        std::ignore = resend_head(block);
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
            if (!in_flight(block)) {
                return;  // nothing in flight: the head, if any, waits for the window
            }
            if (resend_head(block) && !deferred_.contains(block)) {
                arm_retransmit(block);
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
            if (const SentNode* head = user_timed_head(block);
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
            std::ignore = resend_head(block);
            ++context_.counters.retransmits;
            if (block.timer_kind == detail::TimerKind::ArpPoll) {
                return;  // a first send that found no next hop: the poll retries it, and arms the RTO once it leaves
            }
            block.rtt.back_off(context_.config.rto_max);
            arm_retransmit(block);
        }

        /// The ARP poll: the SYN, SYN-ACK or FIN that never left is tried again, as no retry. Once it leaves, or the
        /// device refuses it, the RTO takes over; a next hop still silent after `unresolved_polls` polls ends the
        /// connection with `Unresolved`.
        void on_arp_poll(Block& block) noexcept {
            assert(!block.queue.empty() && "the poll retries an entry that never left");
            if (const std::expected<void, net::SendError> sent = transmit_entry(block, block.queue.head());
                sent || sent.error() != net::SendError::Unresolved) {
                arm_retransmit(block);
                return;
            }
            if (++block.unresolved_polls >= context_.config.unresolved_polls) {
                fail_unresolved(block);  // the bound Linux's ARP probes give: the next hop is not going to answer
                return;
            }
            arm_poll(block);
        }

        /// The probe timer ran out with the peer's window still zero (`ZeroWindowProbe::expire`): the user timeout or
        /// `data_retries` unanswered probes end the connection; else one probe goes, a refused one counted all the
        /// same, and the next is armed.
        void on_probe_timeout(Block& block) noexcept {
            assert(!in_flight(block) && block.send.wnd == 0 &&
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
            std::ignore = send_probe(block);  // a refusal was counted by the send
            arm_probe(block);
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
            disarm(block);
            block.unresolved_polls = 0;
            acks_.remove(block);
            block.finish_pending = false;
            retries_.remove(block);
            drop_queue(block);  // and off the deferred list
        }

        void finish(Block& block) noexcept {
            set_closed(block);
            ++context_.counters.connections_closed;
            raise(block, stream::Event::Closed);
        }

        /// The last ACK the peer is owed must be queued before Closed is published; flush retries the rest.
        void maybe_finish(Block& block) noexcept {
            if (block.finish_pending && !acks_.contains(block)) {
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
            const bool idle     = !in_flight(block);  // else the RTO already runs for the data
            block.send.fin_sent = true;
            block.send.nxt      = fin + 1;  // the FIN takes its number whether or not it left: the timer resends it
            const std::optional<std::uint32_t> entry = enqueue_fin(block, fin);
            if (!entry) {
                ++context_.counters
                      .send_refused;  // unreachable by construction; a release build resets rather than hangs
                abort(block);
                return;
            }
            if (!block.send.covers(fin + 1)) {
                if (idle && !block.probing()) {
                    start_probing(block);  // a zero window with nothing in flight is probed already, as a rule
                }
                return;
            }
            std::ignore = transmit_entry(block, *entry);
            if (idle && (!block.timer.armed() || block.probing())) {
                arm_retransmit(block);  // refused: the RTO tries again, and takes the timer from a probe
            }
        }

        void abort(Block& block) noexcept {
            if (block.state == State::Closed) {
                return;
            }
            // RFC 9293's ABORT in SYN-SENT: the peer holds nothing of ours to take back, so no RST leaves.
            if (block.state != State::SynSent) {
                std::ignore = send_reset(block);
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
        loop::TimerWheel& wheel_;
        detail::SentSegmentPool<Packet> send_pool_;
        detail::ConnectionTable table_;
        std::deque<ConnectionType> connections_ = {};
        EventList events_                       = {};
        AckList acks_                           = {};
        RetryList retries_                      = {};
        DeferredList deferred_                  = {};  ///< Connections whose head resend waits for its packet.
        detail::IsnGenerator isn_{};
        detail::SegmentWriter<Ip> writer_;  ///< Counts into the context's counters, declared before it.
    };

}  // namespace aloe::tcp
