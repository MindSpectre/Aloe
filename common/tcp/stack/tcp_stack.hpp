#pragma once

#include <algorithm>
#include <aloe/core>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <expected>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include <checksum.hpp>
#include <congestion_control.hpp>
#include <datagram.hpp>
#include <flow_table.hpp>
#include <frame_template.hpp>
#include <ipv4.hpp>
#include <ipv4_checksum.hpp>
#include <newreno.hpp>
#include <packet.hpp>
#include <prebuilt.hpp>
#include <rss.hpp>
#include <sent_segment_pool.hpp>
#include <stream_events.hpp>
#include <tcp_config.hpp>
#include <tcp_connection.hpp>
#include <tcp_counters.hpp>
#include <tcp_fwd.hpp>
#include <tcp_header.hpp>
#include <tcp_node_pool.hpp>
#include <tcp_options.hpp>
#include <timer_wheel.hpp>

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

    public:
        using Device         = Ip::Device;
        using Packet         = Ip::Packet;
        using Policy         = CongestionControl;
        using ConnectionType = Connection<Ip, CongestionControl>;
        using EventType      = ConnectionEvent<Ip, CongestionControl>;
        using Sequence       = wire::TcpSequence;

        /// Validates and throws std::invalid_argument; allocates everything; sends nothing.
        Stack(Ip& ip, loop::TimerWheel& wheel, const TcpConfig& config)
            : ip_{&ip},
              wheel_{&wheel},
              config_{validated(config, ip.max_l4_size(), wheel.resolution())},
              mss_{static_cast<std::uint16_t>(ip.max_l4_size() - wire::TcpHeader::size)},
              byte_budget_{static_cast<std::uint32_t>(std::min<std::uint64_t>(
                  std::uint64_t{config_.receive_segments} * mss_, std::numeric_limits<std::uint16_t>::max()))},
              pool_{config_.receive_pool},
              send_pool_{config_.send_pool},
              table_{config_.connections},
              cursor_{config_.ephemeral_first},
              hardware_hash_{ip.queue().steering().enabled && !ip.queue().steering().table.empty() &&
                             ip.queue().steering().types.ipv4_tcp} {
            listeners_.reserve(config_.listeners);
            free_.reserve(config_.connections);
            for (std::size_t index = 0; index < config_.connections; ++index) {
                ConnectionType& slot = connections_.emplace_back(typename ConnectionType::PrivateTag{});
                slot.stack_          = this;
                slot.index_          = static_cast<std::uint32_t>(index);
            }
            for (std::size_t index = config_.connections; index > 0; --index) {
                free_.push_back(static_cast<std::uint32_t>(index - 1));  // slot 0 is taken first
            }
        }

        Stack(const Stack&)            = delete;
        Stack& operator=(const Stack&) = delete;
        Stack(Stack&&)                 = delete;
        Stack& operator=(Stack&&)      = delete;
        /// Cancels every timer, returns every held packet and drops every queued one, which goes back to its pool
        /// now or when the device returns it; the IP brick and the wheel outlive it.
        ~Stack() {
            for (ConnectionType& c : connections_) {
                wheel_->cancel(c.timer_);
                c.prepared_.reset();
                return_chain(c);
                drop_queue(c);
            }
        }

        /// Records the stamp, publishes deferred retry hints, then processes each datagram. Every packet is moved
        /// out or released. An empty span is valid.
        void process(const std::span<net::Datagram<Packet>> segments, const core::TimePoint now) noexcept {
            now_ = now;
            for (ConnectionType* c = retries_.pop_front(); c != nullptr; c = retries_.pop_front()) {
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
            ConnectionType* c = events_.pop_front();
            if (c == nullptr) {
                return std::nullopt;
            }
            const stream::Events snapshot = c->events_;
            c->events_                    = {};
            return EventType{.connection = c, .events = snapshot};
        }

        /// Retries the resends that found their packet lent, which leave once it is back or go by the busy rule, then
        /// sends the ordinary ACKs and window updates that waited, one pure ACK per connection.
        void flush(const core::TimePoint now) noexcept {
            now_ = now;
            for (ConnectionType* c = deferred_.front(); c != nullptr;) {
                ConnectionType* next = DeferredList::next_of(*c);  // a resend that leaves unlinks c
                std::ignore          = resend_head(*c);
                c                    = next;
            }
            for (ConnectionType* c = acks_.front(); c != nullptr;) {
                ConnectionType* next = AckList::next_of(*c);  // a successful send unlinks c
                if (send_pure_ack(*c)) {
                    maybe_finish(*c);
                }
                c = next;
            }
        }

        [[nodiscard]] std::expected<void, ListenError> listen(const std::uint16_t port) noexcept {
            if (listening(port)) {
                return std::unexpected{ListenError::InUse};
            }
            if (listeners_.size() == config_.listeners) {
                return std::unexpected{ListenError::TableFull};
            }
            listeners_.push_back(port);  // within the reserved capacity: no allocation
            return {};
        }

        /// Connections already open stay open.
        void unlisten(const std::uint16_t port) noexcept {
            if (const auto found = std::ranges::find(listeners_, port); found != listeners_.end()) {
                *found = listeners_.back();
                listeners_.pop_back();
            }
        }

        /// A SynSent connection the caller owns; its SYN is queued, or waits for ARP on the timer.
        [[nodiscard]] std::expected<ConnectionType*, ConnectError> connect(const Endpoint peer,
                                                                           const core::TimePoint now) noexcept {
            now_ = now;
            if (!ip_->next_hop(peer.address)) {
                return std::unexpected{ConnectError::NoRoute};
            }
            if (send_pool_.available() == 0) {
                return std::unexpected{ConnectError::TableFull};  // no retransmission-queue node for the SYN
            }
            std::uint32_t base_hash = 0;
            const auto port         = choose_port(peer, base_hash);
            if (!port) {
                return std::unexpected{port.error()};
            }
            const auto slot = take_slot();
            if (!slot) {
                return std::unexpected{ConnectError::TableFull};
            }
            ConnectionType& c = connections_[*slot];
            c.local_          = Endpoint{.address = ip_->address(), .port = *port};
            c.remote_         = peer;
            c.key_            = FlowKey{.remote = peer.address, .remote_port = peer.port, .local_port = *port};
            c.base_hash_      = base_hash;
            [[maybe_unused]] const bool inserted = table_.insert(c.key_, *slot, base_hash);
            assert(inserted && "choose_port found the tuple free");
            c.indexed_          = true;
            c.state_            = State::SynSent;
            c.owned_            = true;
            c.iss_              = draw_isn();
            c.snd_nxt_          = c.iss_ + 1;
            c.snd_una_          = c.iss_;
            c.recover_          = c.iss_;  // RFC 6582: recover starts at the initial send sequence number
            c.rto_              = config_.rto_initial;
            c.srtt_             = std::nullopt;
            c.retransmit_count_ = 0;
            c.send_timestamps_  = config_.timestamps;  // the SYN offers; the SYN-ACK decides
            c.ts_offset_        = static_cast<std::uint32_t>(isn_engine_());
            // Arms the timer on a send or an ARP wait; a refused send leaves the SYN to the RTO.
            std::ignore         = queue_control(c, wire::TcpFlags{wire::TcpFlag::Syn}, c.iss_);
            if (!c.timer_.armed()) {
                arm_retransmit(c);
            }
            return &c;
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
            return mss_;
        }
        /// `min(receive_segments * mss, 65535)`: the receive credit a connection starts with.
        [[nodiscard]] std::uint32_t byte_budget() const noexcept {
            return byte_budget_;
        }
        [[nodiscard]] const TcpCounters& counters() const noexcept {
            return counters_;
        }
        [[nodiscard]] const TcpConfig& config() const noexcept {
            return config_;
        }
        [[nodiscard]] bool listening(const std::uint16_t port) const noexcept {
            return std::ranges::find(listeners_, port) != listeners_.end();
        }
        [[nodiscard]] std::size_t pending_events() const noexcept {
            return events_.size();
        }
        [[nodiscard]] core::TimePoint now() const noexcept {
            return now_;
        }
        [[nodiscard]] std::size_t nodes_available() const noexcept {
            return pool_.available();
        }
        [[nodiscard]] std::size_t table_size() const noexcept {
            return table_.size();
        }
        /// For tests and monitoring: the entry at `position` of the connection's retransmission queue, 0 the head.
        [[nodiscard]] const detail::SentSegmentPool<Packet>::Node&
        sent_segment(const ConnectionType& conn, const std::size_t position) const noexcept {
            assert(position < conn.queue_count_ && "past the tail of the retransmission queue");
            std::uint32_t index = conn.queue_head_;
            for (std::size_t at = 0; at < position; ++at) {
                index = send_pool_.node(index).next;
            }
            return send_pool_.node(index);
        }
        [[nodiscard]] std::size_t send_nodes_available() const noexcept {
            return send_pool_.available();
        }

        /// For tests: the next connection opened takes this initial sequence number instead of a drawn one.
        void force_next_isn(const wire::TcpSequence isn) noexcept {
            forced_isn_ = isn;
        }

        /// For tests: the connection's RFC 6582 `recover` takes this value, as if an episode had recorded it.
        void force_recover(ConnectionType& conn, const wire::TcpSequence recover) noexcept {
            core::force_non_static(this);
            conn.recover_ = recover;
        }

    private:
        friend class Connection<Ip, CongestionControl>;

        using Datagram     = net::Datagram<Packet>;
        using EventList    = detail::ConnectionList<ConnectionType, &ConnectionType::event_link_>;
        using AckList      = detail::ConnectionList<ConnectionType, &ConnectionType::ack_link_>;
        using RetryList    = detail::ConnectionList<ConnectionType, &ConnectionType::retry_link_>;
        using DeferredList = detail::ConnectionList<ConnectionType, &ConnectionType::deferred_link_>;
        using SentNode     = detail::SentSegmentPool<Packet>::Node;

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

        /// What one ACK freed from the head of the retransmission queue: whole entries only.
        struct Freed {
            std::uint32_t length = 0;      ///< Sequence space freed.
            bool resent          = false;  ///< Some entry freed left other than exactly once: no sample from it (Karn).
            core::TimePoint sent{};        ///< The oldest entry freed: its last transmission.
            core::TimePoint newest_sent{};            ///< The newest entry freed: its last transmission.
            std::uint64_t delivered_at_send = 0;      ///< The newest entry freed: the delivered count when it was sent.
            bool app_limited                = false;  ///< The newest entry freed: its send left window unused.
        };

        // The receive path: header, checksum, lookup, the handshake, the synchronized states.

        void on_datagram(Datagram& datagram) noexcept {
            ++counters_.segments_received;
            const std::span<std::byte> l4               = datagram.l4();
            const std::optional<wire::TcpHeader> header = wire::TcpHeader::parse(l4);
            if (!header) {
                ++counters_.dropped_bad_header;
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
                    ++counters_.dropped_bad_header;
                    return;
                }
                if (header->flags.has(wire::TcpFlag::Syn)) {
                    segment.mss = options->mss;  // RFC 9293: the option means something only on a SYN
                }
                segment.ts_value = options->timestamp_value;
                segment.ts_echo  = options->timestamp_echo;
            }
            if (!checksum_ok(datagram)) {
                ++counters_.dropped_bad_checksum;
                return;
            }
            const FlowKey key{
                .remote = datagram.source, .remote_port = header->source_port, .local_port = header->destination_port};
            segment.base_hash                        = base_hash_of(datagram, segment, key);
            const std::optional<std::uint32_t> found = table_.find(key, segment.base_hash);
            if (!found) {
                const bool opening = header->flags.has(wire::TcpFlag::Syn) && !header->flags.has(wire::TcpFlag::Ack) &&
                                     !header->flags.has(wire::TcpFlag::Rst);
                if (opening && listening(header->destination_port)) {
                    passive_open(datagram, segment, key);
                    return;
                }
                ++counters_.dropped_no_connection;
                if (!header->flags.has(wire::TcpFlag::Rst)) {
                    reply_reset(datagram, segment);
                }
                return;
            }
            switch (ConnectionType& conn = connections_[*found]; conn.state_) {
                case State::Closed:  // still indexed because not released
                    ++counters_.dropped_closed;
                    if (!header->flags.has(wire::TcpFlag::Rst)) {
                        reply_reset(datagram, segment);
                    }
                    return;
                case State::SynSent:
                    receive_syn_sent(conn, datagram, segment);
                    return;
                default:
                    receive_synchronized(conn, datagram, segment);
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

        [[nodiscard]] std::uint32_t
        base_hash_of(const Datagram& datagram, const Segment& segment, const FlowKey key) const noexcept {
            if (!hardware_hash_) {
                return detail::software_flow_hash(key);
            }
            if (const std::optional<std::uint32_t> hash = datagram.packet.rx().rss_hash) {
                return *hash;
            }
            return device::flow_hash(ip_->queue().steering(),
                                     device::FlowTuple{.source           = datagram.source,
                                                       .destination      = datagram.destination,
                                                       .source_port      = segment.header.source_port,
                                                       .destination_port = segment.header.destination_port,
                                                       .protocol         = wire::Ipv4Protocol::Tcp});
        }

        void passive_open(Datagram& datagram, const Segment& segment, const FlowKey key) noexcept {
            if (send_pool_.available() == 0) {
                ++counters_.dropped_no_node;  // no retransmission-queue node for the SYN-ACK
                return;
            }
            const std::optional<std::uint32_t> slot = take_slot();
            if (!slot) {
                ++counters_.dropped_table_full;
                return;
            }
            ConnectionType& conn = connections_[*slot];
            conn.local_          = Endpoint{.address = ip_->address(), .port = segment.header.destination_port};
            conn.remote_         = Endpoint{.address = datagram.source, .port = segment.header.source_port};
            conn.key_            = key;
            conn.base_hash_      = segment.base_hash;
            [[maybe_unused]] const bool inserted = table_.insert(key, *slot, segment.base_hash);
            assert(inserted && "the lookup just missed");
            conn.indexed_          = true;
            conn.state_            = State::SynReceived;
            conn.owned_            = false;
            conn.rcv_nxt_          = segment.header.sequence + 1;
            conn.rcv_adv_          = conn.rcv_nxt_ + byte_budget_;  // the initial offer, carried by the SYN-ACK
            conn.peer_mss_         = segment.mss.value_or(default_peer_mss);
            conn.snd_wnd_          = segment.header.window;
            conn.wl1_              = segment.header.sequence;
            conn.wl2_              = Sequence{};
            conn.iss_              = draw_isn();
            conn.snd_nxt_          = conn.iss_ + 1;
            conn.snd_una_          = conn.iss_;
            conn.recover_          = conn.iss_;  // RFC 6582: recover starts at the initial send sequence number
            conn.rto_              = config_.rto_initial;
            conn.srtt_             = std::nullopt;
            conn.retransmit_count_ = 0;
            conn.send_timestamps_  = config_.timestamps && segment.ts_value.has_value();
            conn.ts_offset_        = static_cast<std::uint32_t>(isn_engine_());
            conn.last_ack_sent_    = conn.rcv_nxt_;  // what the SYN-ACK acknowledges, as Linux's rcv_wup
            if (conn.send_timestamps_) {
                take_ts_recent(conn, *segment.ts_value);
            }
            // Arms the timer on a send or an ARP wait; a refused send leaves the SYN-ACK to the RTO.
            std::ignore = queue_control(conn, wire::TcpFlags{wire::TcpFlag::Syn, wire::TcpFlag::Ack}, conn.iss_);
            if (!conn.timer_.armed()) {
                arm_retransmit(conn);
            }
        }

        void receive_syn_sent(ConnectionType& conn, Datagram& datagram, Segment segment) noexcept {
            const wire::TcpHeader& header = segment.header;
            if (header.flags.has(wire::TcpFlag::Ack) && header.acknowledgement != conn.snd_nxt_) {
                ++counters_.dropped_unexpected;  // not an answer to our SYN
                return;
            }
            if (header.flags.has(wire::TcpFlag::Rst)) {
                if (!header.flags.has(wire::TcpFlag::Ack)) {
                    ++counters_.dropped_unexpected;
                    return;
                }
                ++counters_.connections_reset;
                set_closed(conn);
                raise(conn, stream::Event::Reset);
                return;
            }
            if (!header.flags.has(wire::TcpFlag::Syn) || !header.flags.has(wire::TcpFlag::Ack)) {
                ++counters_.dropped_unexpected;  // a simultaneous open is out of scope
                return;
            }
            conn.send_timestamps_ = conn.send_timestamps_ && segment.ts_value.has_value();  // our offer, answered
            if (conn.send_timestamps_) {
                take_ts_recent(conn, *segment.ts_value);
            }
            const Freed freed = acknowledge(conn, header.acknowledgement);  // frees the SYN's entry
            conn.snd_una_     = header.acknowledgement;
            on_acknowledged(conn, freed, segment.ts_echo);  // a sample, and the timer stopped with the queue empty
            conn.rcv_nxt_ = header.sequence + 1;
            conn.rcv_adv_ =
                conn.rcv_nxt_ + byte_budget_;     // the initial offer, carried by our ACK or the first data segment
            conn.last_ack_sent_ = conn.rcv_nxt_;  // as Linux's rcv_wup: data on the SYN-ACK may update TS.Recent
            conn.peer_mss_      = segment.mss.value_or(default_peer_mss);
            conn.snd_wnd_       = header.window;
            conn.wl1_           = header.sequence;
            conn.wl2_           = header.acknowledgement;
            conn.state_         = State::Established;
            reserve_fin_node(conn);
            CongestionControl::reset(conn.congestion_, conn.mss());
            update_probing(conn);  // a SYN-ACK offering no window starts probing as any ACK does
            ++counters_.connections_opened;
            queue_ack(conn);  // deferred: a commit on the Connected event completes the handshake with data
            raise(conn, stream::Event::Connected);
            if (segment.payload_length > 0 || header.flags.has(wire::TcpFlag::Fin)) {
                segment.header.sequence = conn.rcv_nxt_;
                segment.header.flags    = wire::TcpFlags::from_raw(
                    static_cast<std::uint8_t>(header.flags.raw() & ~std::to_underlying(wire::TcpFlag::Syn)));
                receive_synchronized(conn, datagram, segment);
            }
        }

        void receive_synchronized(ConnectionType& conn, Datagram& datagram, Segment segment) noexcept {
            wire::TcpHeader& header = segment.header;
            const bool rst          = header.flags.has(wire::TcpFlag::Rst);
            const Sequence arrived  = header.sequence;  // SEG.SEQ before any trim, for the TS.Recent rule

            // 0. PAWS (RFC 7323), before the sequence check as Linux's tcp_validate_incoming: a timestamp older than
            // TS.Recent is an old duplicate, acknowledged at once and dropped. A RST is exempt; a segment without the
            // option goes on, as Linux lets it.
            if (paws_rejects(conn, segment, rst)) {
                ++counters_.dropped_paws;
                ack_now(conn);
                return;
            }

            // 1. Sequence check against [rcv_nxt, rcv_adv): trim an old prefix, drop the rest with an immediate ACK.
            const std::uint32_t window = conn.rcv_adv_.value - conn.rcv_nxt_.value;
            const std::uint32_t length = segment.length();
            Sequence sequence          = header.sequence;
            const Sequence end         = sequence + length;
            if (length == 0) {
                const bool acceptable = window == 0 ? sequence == conn.rcv_nxt_
                                                    : !sequence.before(conn.rcv_nxt_) && sequence.before(conn.rcv_adv_);
                if (!acceptable) {
                    ++counters_.dropped_out_of_window;
                    if (!rst) {
                        ack_now(conn);
                    }
                    return;
                }
            } else {
                if (!end.after(conn.rcv_nxt_)) {  // entirely before rcv_nxt
                    if (conn.state_ == State::SynReceived && header.flags.has(wire::TcpFlag::Syn) &&
                        !header.flags.has(wire::TcpFlag::Ack)) {
                        std::ignore = resend_head(conn);  // a duplicate SYN: the SYN-ACK again, from its entry
                        return;
                    }
                    ++counters_.dropped_duplicate;
                    if (!rst) {
                        ack_now(conn);
                    }
                    return;
                }
                if (sequence.after(conn.rcv_nxt_)) {  // a gap before it
                    ++counters_.dropped_out_of_order;
                    if (!conn.recovery_end_ || end.after(*conn.recovery_end_)) {
                        conn.recovery_end_ = end;
                    }
                    if (!rst) {
                        ack_now(conn);
                    }
                    return;
                }
                if (sequence.before(conn.rcv_nxt_)) {  // overlap: the old prefix is already ours
                    std::uint32_t trim = conn.rcv_nxt_.value - sequence.value;
                    if (header.flags.has(wire::TcpFlag::Syn)) {
                        header.flags = wire::TcpFlags::from_raw(
                            static_cast<std::uint8_t>(header.flags.raw() & ~std::to_underlying(wire::TcpFlag::Syn)));
                        --trim;
                    }
                    assert(trim <= segment.payload_length);
                    segment.payload_offset = static_cast<std::uint16_t>(segment.payload_offset + trim);
                    segment.payload_length = static_cast<std::uint16_t>(segment.payload_length - trim);
                    sequence               = conn.rcv_nxt_;
                    header.sequence        = sequence;
                }
                if (window == 0 || end.after(conn.rcv_adv_)) {
                    ++counters_.dropped_out_of_window;
                    if (!rst) {
                        ack_now(conn);
                    }
                    return;
                }
            }
            update_ts_recent(conn, segment, arrived);  // acceptable, trimmed or not

            // 2. RST.
            if (rst) {
                if (conn.state_ == State::SynReceived && !conn.owned_) {
                    ++counters_.handshakes_failed;
                    free_slot(conn);
                    return;
                }
                ++counters_.connections_reset;
                set_closed(conn);
                raise(conn, stream::Event::Reset);
                return;
            }
            if (header.flags.has(wire::TcpFlag::Syn)) {
                ++counters_.dropped_unexpected;  // a SYN inside the window of a synchronized connection
                return;
            }

            // 3. ACK.
            if (!header.flags.has(wire::TcpFlag::Ack)) {
                ++counters_.dropped_unexpected;
                return;
            }
            const Sequence ack = header.acknowledgement;
            if (conn.state_ == State::SynReceived) {
                if (ack != conn.snd_nxt_) {
                    ++counters_.dropped_unexpected;
                    return;
                }
                const Freed freed = acknowledge(conn, ack);  // frees the SYN-ACK's entry
                conn.snd_una_     = ack;
                on_acknowledged(conn, freed, segment.ts_echo);  // a sample, and the timer stopped with the queue empty
                conn.state_ = State::Established;
                reserve_fin_node(conn);
                CongestionControl::reset(conn.congestion_, conn.mss());
                conn.owned_ = true;
                ++counters_.connections_accepted;
                raise(conn, stream::Event::Accepted);
                // The same segment may carry data or a FIN: keep going.
            }
            if (ack.after(conn.snd_nxt_)) {
                ++counters_.dropped_unexpected;  // acknowledges what was never sent
                ack_now(conn);
                return;
            }
            const std::size_t writable_before = writable(conn);

            // RFC 5681's duplicate ACK, of something in flight, judged against the window as it stands, before this
            // segment updates it.
            const bool duplicate = segment.payload_length == 0 && !header.flags.has(wire::TcpFlag::Syn) &&
                                   !header.flags.has(wire::TcpFlag::Fin) && ack == conn.snd_una_ &&
                                   header.window == conn.snd_wnd_ && in_flight(conn);
            if (ack.after(conn.snd_una_)) {
                acknowledge_data(conn, ack, segment.ts_echo);
                raise(conn, stream::Event::Acked);
                if (conn.fin_sent_ && ack == conn.snd_nxt_) {  // our FIN is acknowledged: the queue is empty
                    switch (conn.state_) {
                        case State::FinWait1:
                            conn.state_ = State::FinWait2;
                            break;
                        case State::Closing:
                        case State::LastAck:
                            conn.finish_pending_ = true;
                            break;
                        default:
                            break;
                    }
                }
            } else if (duplicate) {
                on_duplicate_ack(conn, ack);
            }
            // RFC 793 updates the window only when SND.UNA <= SEG.ACK <= SND.NXT: a stale ACK carries a stale window.
            if (!ack.before(conn.snd_una_)) {
                update_peer_window(conn, header);
                update_probing(conn);  // a window that opened ends probing; the comparison below raises Writable
                send_unsent(conn);     // and sends the FIN a close left waiting for it
            }
            if (writable(conn) > writable_before) {
                raise(conn, stream::Event::Writable);
            }
            maybe_finish(conn);
            if (conn.state_ == State::Closed) {
                return;
            }

            // 4. Payload.
            if (segment.payload_length > 0) {
                if (conn.peer_closed_) {  // the peer's sequence space ended at its FIN, whatever state we are in
                    ++counters_.dropped_unexpected;
                    ack_now(conn);
                    return;
                }
                if (!accept_payload(conn, datagram, segment)) {
                    ack_now(conn);  // reports the last accepted byte; the FIN behind refused data is not consumed
                    return;
                }
                apply_ack_policy(conn, segment.payload_length);
            }

            // 5. FIN, in order: its sequence is rcv_nxt after the payload.
            if (header.flags.has(wire::TcpFlag::Fin)) {
                if (conn.state_ == State::CloseWait || conn.state_ == State::Closing || conn.state_ == State::LastAck) {
                    ++counters_.dropped_unexpected;  // a second FIN: the peer's sequence space ended at the first
                    ack_now(conn);
                    return;
                }
                receive_fin(conn);
            }
        }

        [[nodiscard]] bool accept_payload(ConnectionType& conn, Datagram& datagram, const Segment& segment) noexcept {
            if (conn.chain_count_ >= config_.receive_segments) {
                ++counters_.dropped_no_slot;
                return false;
            }
            if (pool_.available() == 0) {
                ++counters_.dropped_no_node;
                return false;
            }
            const auto offset =
                static_cast<std::uint16_t>(datagram.l3_offset + datagram.l3_length + segment.payload_offset);
            const std::optional<std::uint32_t> node =
                pool_.acquire(std::move(datagram.packet), offset, segment.payload_length);
            assert(node.has_value() && "a node was available");
            if (conn.chain_tail_ == detail::no_node) {
                conn.chain_head_ = *node;
            } else {
                pool_.node(conn.chain_tail_).next = *node;
            }
            conn.chain_tail_ = *node;
            ++conn.chain_count_;
            conn.rcv_nxt_       = conn.rcv_nxt_ + segment.payload_length;
            conn.unread_bytes_ += segment.payload_length;
            raise(conn, stream::Event::Readable);
            return true;
        }

        /// RFC 5681: ordinary in-order data waits for flush; recovery, and every second full-sized segment, do not. A
        /// full-sized segment is our MSS less the timestamp option a timestamped peer puts in every segment.
        void apply_ack_policy(ConnectionType& conn, const std::size_t accepted) noexcept {
            if (conn.recovery_end_) {
                ack_now(conn);
                if (!conn.rcv_nxt_.before(*conn.recovery_end_)) {
                    conn.recovery_end_.reset();
                }
                return;
            }
            if (accepted >= mss_ - (conn.send_timestamps_ ? wire::TcpOptions::timestamps_size : 0)) {
                if (conn.full_segments_since_ack_ < 2) {
                    ++conn.full_segments_since_ack_;
                }
                if (conn.full_segments_since_ack_ >= 2) {
                    ack_now(conn);  // a success resets the count; a refusal leaves it saturated at two
                    return;
                }
            }
            queue_ack(conn);
        }

        void receive_fin(ConnectionType& conn) noexcept {
            conn.rcv_nxt_     = conn.rcv_nxt_ + 1;
            conn.peer_closed_ = true;
            raise(conn, stream::Event::PeerClosed);
            switch (conn.state_) {
                case State::Established:
                    conn.state_ = State::CloseWait;
                    queue_ack(conn);  // the application's close may carry it
                    break;
                case State::FinWait1:
                    conn.state_ = State::Closing;
                    ack_now(conn);
                    break;
                case State::FinWait2:
                    conn.finish_pending_ = true;
                    ack_now(conn);  // the terminal ACK goes before Closed is published; flush retries a refusal
                    break;
                default:
                    queue_ack(conn);
                    break;
            }
        }

        /// RFC 793's window update rule: a newer segment, or the same one with a newer acknowledgement.
        static void update_peer_window(ConnectionType& conn, const wire::TcpHeader& header) noexcept {
            if (conn.wl1_.before(header.sequence) ||
                (conn.wl1_ == header.sequence && !conn.wl2_.after(header.acknowledgement))) {
                conn.snd_wnd_ = header.window;
                conn.wl1_     = header.sequence;
                conn.wl2_     = header.acknowledgement;
            }
        }

        /**
         * The end of an acceptable ACK, after its window update. The peer answered, so the count of unanswered probes
         * clears, as Linux's `tcp_ack` clears it on every acceptable ACK, and so does the stamp of the first unanswered
         * one: RFC 9293 keeps the connection open while the peer acknowledges probes, so the user timeout bounds a run
         * of unanswered probes only. A window that opened ends probing; a zero window with nothing in flight starts
         * it: in Established or CloseWait whether or not the application has data, and in any state while a FIN the
         * window did not cover waits in the queue (`close`). A head waiting for the next hop is the ARP poll's.
         */
        void update_probing(ConnectionType& conn) noexcept {
            conn.probes_out_    = 0;
            conn.probe_started_ = std::nullopt;
            if (conn.probing()) {
                if (conn.snd_wnd_ > 0) {
                    disarm(conn);
                }
                return;
            }
            if (conn.snd_wnd_ == 0 && !in_flight(conn) && conn.timer_kind_ != detail::TimerKind::ArpPoll &&
                (conn.queue_head_ != detail::no_node || conn.state_ == State::Established ||
                 conn.state_ == State::CloseWait)) {
                start_probing(conn);
            }
        }

        /// The head that never left, the FIN of a close the peer's window did not cover: sent once the window covers
        /// it, as Linux's `tcp_push_pending_frames` on an ACK. A head waiting for the next hop is the ARP poll's. The
        /// send arms the RTO (`transmit_entry`); a refused one leaves the RTO to try again, as `close` does.
        void send_unsent(ConnectionType& conn) noexcept {
            if (conn.queue_head_ == detail::no_node || in_flight(conn) ||
                conn.timer_kind_ == detail::TimerKind::ArpPoll) {
                return;
            }
            if (const SentNode& head = send_pool_.node(conn.queue_head_); !covered(conn, head.first + head.length)) {
                return;
            }
            std::ignore = transmit_entry(conn, conn.queue_head_);
            if (!conn.timer_.armed()) {
                arm_retransmit(conn);
            }
        }

        /**
         * RFC 7323's PAWS test: on a connection with timestamps and a TS.Recent, a non-RST segment whose value is older
         * than TS.Recent, in the option's signed 32-bit arithmetic, is rejected, unless TS.Recent is older than 24
         * days, past which the peer's clock may have wrapped.
         */
        [[nodiscard]] bool
        paws_rejects(const ConnectionType& conn, const Segment& segment, const bool rst) const noexcept {
            return conn.send_timestamps_ && conn.ts_recent_valid_ && segment.ts_value && !rst &&
                   static_cast<std::int32_t>(*segment.ts_value - conn.ts_recent_) < 0 &&
                   now_ - conn.ts_recent_stamp_ < paws_window;
        }

        /**
         * RFC 7323's TS.Recent rule, for a segment that passed the sequence check: its value replaces TS.Recent when
         * it is not older, or TS.Recent is past the PAWS window, and the segment starts at or before Last.ACK.sent, so
         * the echo is the value of the segment the last ACK sent acknowledged, as Linux's `tcp_replace_ts_recent`.
         */
        void update_ts_recent(ConnectionType& conn, const Segment& segment, const Sequence arrived) const noexcept {
            if (!conn.send_timestamps_ || !segment.ts_value || arrived.after(conn.last_ack_sent_)) {
                return;
            }
            if (static_cast<std::int32_t>(*segment.ts_value - conn.ts_recent_) >= 0 ||
                now_ - conn.ts_recent_stamp_ >= paws_window) {
                take_ts_recent(conn, *segment.ts_value);
            }
        }

        /// TS.Recent takes `value`, stamped now.
        void take_ts_recent(ConnectionType& conn, const std::uint32_t value) const noexcept {
            conn.ts_recent_       = value;
            conn.ts_recent_stamp_ = now_;
            conn.ts_recent_valid_ = true;
        }

        /// The connection's timestamp clock: the stamp's milliseconds, wrapping, plus its own offset.
        [[nodiscard]] std::uint32_t ts_now(const ConnectionType& conn) const noexcept {
            return static_cast<std::uint32_t>(
                       std::chrono::duration_cast<std::chrono::milliseconds>(now_.time_since_epoch()).count()) +
                   conn.ts_offset_;
        }

        /// RFC 793's reset for a segment with no connection: with ACK, sequence at its acknowledgement;
        /// without, sequence zero and acknowledgement past everything it occupied.
        void reply_reset(const Datagram& datagram, const Segment& segment) noexcept {
            std::optional<Packet> packet = ip_->allocate();
            if (!packet) {
                ++counters_.send_refused;
                return;
            }
            const std::optional<std::span<std::byte>> room = packet->append(wire::TcpHeader::size);
            if (!room) {
                ++counters_.send_refused;
                return;
            }
            wire::TcpHeader reply{.source_port      = segment.header.destination_port,
                                  .destination_port = segment.header.source_port};
            if (segment.header.flags.has(wire::TcpFlag::Ack)) {
                reply.sequence = segment.header.acknowledgement;
                reply.flags    = wire::TcpFlags{wire::TcpFlag::Rst};
            } else {
                reply.acknowledgement = segment.header.sequence + segment.length();
                reply.flags           = wire::TcpFlags{wire::TcpFlag::Rst, wire::TcpFlag::Ack};
            }
            reply.write(*room);
            const auto sent = ip_->send(std::move(*packet),
                                        net::SendRequest{.destination = datagram.source,
                                                         .protocol    = wire::Ipv4Protocol::Tcp,
                                                         .checksum    = device::L4Checksum::Tcp},
                                        now_);
            if (sent) {
                ++counters_.resets_sent;
            } else if (sent.error() == net::SendError::Unresolved) {
                ++counters_.send_unresolved;
            } else {
                ++counters_.send_refused;
            }
            // On failure the packet is still in `packet` and goes back to the pool with it.
        }

        // The transmit path: every segment of a connection leaves through emit; the retransmission queue keeps those
        // that occupy sequence space; pure ACKs and their policy.

        /// The right edge an ACK offers now: the candidate `rcv_nxt + (B - unread)` when it is later than the current
        /// edge and a packet slot is free, else the current edge.
        [[nodiscard]] Sequence offer_of(const ConnectionType& conn) const noexcept {
            if (const Sequence candidate =
                    conn.rcv_nxt_ + (byte_budget_ - static_cast<std::uint32_t>(conn.unread_bytes_));
                candidate.after(conn.rcv_adv_) && conn.chain_count_ < config_.receive_segments) {
                return candidate;
            }
            return conn.rcv_adv_;
        }

        /// A segment with ACK left offering `offer`: the edge is recorded and the ACK that waited is done.
        void record_offer(ConnectionType& conn, const Sequence offer) noexcept {
            if (offer != conn.rcv_adv_) {
                ++counters_.window_updates;
            }
            conn.rcv_adv_     = offer;
            conn.ack_pending_ = false;
            acks_.remove(conn);
            conn.full_segments_since_ack_ = 0;
        }

        void count_failure(const net::SendError error) noexcept {
            if (error == net::SendError::Unresolved) {
                ++counters_.send_unresolved;
            } else {
                ++counters_.send_refused;
            }
        }

        /// Rebuilds the link and network headers when they went stale: never built, past their validity, or the IP
        /// brick's generation moved. A failure was counted by IP; the old headers stay stale.
        [[nodiscard]] std::expected<void, net::SendError> ensure_template(ConnectionType& conn) noexcept {
            if (!conn.template_.stale(now_, ip_->generation())) {
                return {};
            }
            const std::expected<net::Prebuilt, net::SendError> built =
                ip_->prebuild(conn.remote_.address, wire::Ipv4Protocol::Tcp, now_);
            if (!built) {
                return std::unexpected{built.error()};
            }
            conn.template_.headers = *built;
            conn.template_.built   = true;
            return {};
        }

        /**
         * The one send function: every segment of a connection leaves here, kept or not. Places the frame's headers in
         * front of the payload, prepended when the packet is not framed yet and over the old ones when it is: the
         * template's link and network part, the TCP header with a zero checksum field, and the option block: the MSS on
         * a handshake segment, then `nop, nop, TS` on every segment of a connection that sends timestamps, the clock's
         * value now and TS.Recent as the echo. A segment with ACK offers the receive window (`offer_of`).
         * `Ipv4::send_frame` finishes the frame and lends the packet to the device; the packet stays with the caller.
         * Only a success records the offer and Last.ACK.sent and clears the pending ACK; a failure is counted and
         * leaves the packet framed. One connection's segments of one kind keep one header size, so a framed packet is
         * rewritten in place.
         */
        [[nodiscard]] std::expected<void, net::SendError> emit(ConnectionType& conn,
                                                               Packet& packet,
                                                               const bool framed,
                                                               const Sequence sequence,
                                                               const wire::TcpFlags flags,
                                                               const bool handshake) noexcept {
            assert(!packet.lent() && "a lent packet is not written");
            const bool stamped            = conn.send_timestamps_;
            const std::size_t tcp_size    = wire::TcpHeader::size + (handshake ? wire::TcpOptions::mss_size : 0) +
                                            (stamped ? wire::TcpOptions::timestamps_size : 0);
            const std::size_t header_size = net::Prebuilt::size + tcp_size;
            assert((!framed || packet.size() >= header_size) && "a framed packet holds its headers");
            const std::optional<std::span<std::byte>> headers =
                framed ? std::optional{packet.data().first(header_size)} : packet.prepend(header_size);
            assert(headers.has_value() && "a packet from allocate() has room for the headers");
            if (!headers) {
                ++counters_.send_refused;  // unreachable by construction; refused in a release build
                return std::unexpected{net::SendError::Refused};
            }
            if (const std::expected<void, net::SendError> built = ensure_template(conn); !built) {
                count_failure(built.error());
                return built;
            }
            std::ranges::copy(conn.template_.headers.headers, headers->begin());
            const bool with_ack            = flags.has(wire::TcpFlag::Ack);
            const Sequence offer           = with_ack ? offer_of(conn) : conn.rcv_adv_;
            const std::span<std::byte> tcp = headers->subspan(net::Prebuilt::size);
            wire::TcpHeader{.source_port      = conn.local_.port,
                            .destination_port = conn.remote_.port,
                            .sequence         = sequence,
                            .acknowledgement  = with_ack ? conn.rcv_nxt_ : Sequence{},
                            .data_offset      = static_cast<std::uint8_t>(tcp_size),
                            .flags            = flags,
                            .window           = with_ack ? static_cast<std::uint16_t>(offer.value - conn.rcv_nxt_.value)
                                                         : static_cast<std::uint16_t>(byte_budget_)}
                .write(tcp);
            std::span<std::byte> options = tcp.subspan(wire::TcpHeader::size);
            if (handshake) {
                wire::TcpOptions::write_mss(options, mss_);
                options = options.subspan(wire::TcpOptions::mss_size);
            }
            if (stamped) {
                wire::TcpOptions::write_timestamps(options, ts_now(conn), conn.ts_recent_);
            }
            const std::expected<void, net::SendError> sent =
                ip_->send_frame(packet,
                                net::SendRequest{.destination = conn.remote_.address,
                                                 .protocol    = wire::Ipv4Protocol::Tcp,
                                                 .checksum    = device::L4Checksum::Tcp},
                                now_);
            if (!sent) {
                count_failure(sent.error());
                return sent;
            }
            if (with_ack) {
                record_offer(conn, offer);
                conn.last_ack_sent_ = conn.rcv_nxt_;
            }
            return {};
        }

        /// Established: one node of the pool is set aside for the connection's FIN, so a close never finds the pool
        /// empty, as Linux's FIN takes memory its accounting cannot refuse. The ACK that brought the connection here
        /// freed the handshake's node, so one is free to set aside; the end of a connection that never closed gives
        /// it back (`drop_queue`).
        void reserve_fin_node(ConnectionType& conn) noexcept {
            conn.fin_reserved_ = send_pool_.reserve();
            assert(conn.fin_reserved_ && "the handshake's node just came back");
        }

        /**
         * A blank entry for a segment about to leave, linked at the tail of the queue. `packet` is moved in, and may
         * be empty for a SYN or SYN-ACK, which `transmit_entry` gives one. Nothing, with the packet still the
         * caller's, when no node is free or a data segment would pass the send cap. The FIN's entry is `enqueue_fin`.
         */
        [[nodiscard]] std::optional<std::uint32_t> enqueue(ConnectionType& conn,
                                                           Packet&& packet,
                                                           const std::uint16_t payload,
                                                           const wire::TcpFlags flags,
                                                           const Sequence first,
                                                           const std::uint32_t length) noexcept {
            if (payload > 0 && conn.queue_count_ >= config_.send_segments) {
                return std::nullopt;
            }
            const std::optional<std::uint32_t> index = send_pool_.acquire();
            if (!index) {
                return std::nullopt;
            }
            link_tail(conn, *index, std::move(packet), payload, flags, first, length);
            return index;
        }

        /// The FIN's entry, on the node Established set aside for it, which spends the reservation; the stack's own,
        /// so the send cap does not hold it. With no node set aside, which construction rules out, any free node.
        [[nodiscard]] std::optional<std::uint32_t> enqueue_fin(ConnectionType& conn, const Sequence fin) noexcept {
            assert(conn.fin_reserved_ && "a FIN without the node Established set aside");
            const std::optional<std::uint32_t> index =
                std::exchange(conn.fin_reserved_, false) ? send_pool_.acquire_reserved() : send_pool_.acquire();
            if (!index) {
                return std::nullopt;
            }
            link_tail(conn, *index, Packet{}, 0, wire::TcpFlags{wire::TcpFlag::Fin, wire::TcpFlag::Ack}, fin, 1);
            return index;
        }

        /// Fills the blank node `index` for a segment about to leave and links it at the tail of the queue.
        void link_tail(ConnectionType& conn,
                       const std::uint32_t index,
                       Packet&& packet,
                       const std::uint16_t payload,
                       const wire::TcpFlags flags,
                       const Sequence first,
                       const std::uint32_t length) noexcept {
            SentNode& node         = send_pool_.node(index);
            node.packet            = std::move(packet);
            node.first             = first;
            node.length            = length;
            node.payload           = payload;
            node.flags             = flags;
            node.delivered_at_send = conn.delivered_;
            node.app_limited       = send_room(conn) > payload;  // before the send: the windows allowed more
            if (conn.queue_tail_ == detail::no_node) {
                conn.queue_head_ = index;
            } else {
                send_pool_.node(conn.queue_tail_).next = index;
            }
            conn.queue_tail_ = index;
            ++conn.queue_count_;
        }

        /// Takes back the tail `enqueue` just linked after `previous`, the tail before it; its packet goes with it.
        void unlink_tail(ConnectionType& conn, const std::uint32_t previous) noexcept {
            send_pool_.release(conn.queue_tail_);
            conn.queue_tail_ = previous;
            if (previous == detail::no_node) {
                conn.queue_head_ = detail::no_node;
            } else {
                send_pool_.node(previous).next = detail::no_node;
            }
            --conn.queue_count_;
        }

        /**
         * One transmission of a queued entry through `emit`; its packet must be back from the device. A SYN, SYN-ACK
         * or FIN entry without a packet takes one from IP first. A success stamps the entry and counts a control
         * segment for a SYN or FIN; either way the packet holds its frame from then on. The first transmission of the
         * only entry, a segment sent into an empty queue, arms the timer: the RTO when it left, the ARP poll when a
         * SYN, SYN-ACK or FIN found no next hop. A data segment that found none is not kept: `commit` refuses it.
         */
        [[nodiscard]] std::expected<void, net::SendError> transmit_entry(ConnectionType& conn,
                                                                         const std::uint32_t index) noexcept {
            SentNode& node = send_pool_.node(index);
            assert(!node.packet.lent() && "a queued packet is written only once the device gave it back");
            const bool first_into_empty = node.transmits == 0 && index == conn.queue_head_ && conn.queue_count_ == 1;
            if (node.packet.empty()) {
                std::optional<Packet> fresh = ip_->allocate();
                if (!fresh) {
                    ++counters_.send_refused;
                    return std::unexpected{net::SendError::Refused};
                }
                node.packet = std::move(*fresh);
            }
            const std::expected<void, net::SendError> sent =
                emit(conn, node.packet, node.framed, node.first, node.flags, node.flags.has(wire::TcpFlag::Syn));
            node.framed = true;
            if (!sent) {
                if (first_into_empty && node.payload == 0 && sent.error() == net::SendError::Unresolved) {
                    arm_poll(conn);
                }
                return sent;
            }
            record_transmission(conn, node);
            if (first_into_empty) {
                arm_retransmit(conn);
            }
            return {};
        }

        /// A transmission of the entry left, from its packet or as a copy: stamped, on the connection too when it
        /// carries payload (the idle restart's clock), counted as a control segment for a SYN or FIN, and the ARP
        /// poll's count cleared.
        void record_transmission(ConnectionType& conn, SentNode& node) noexcept {
            if (node.transmits == 0) {
                node.first_sent = now_;
            }
            node.sent = now_;
            if (node.payload > 0) {
                conn.last_data_send_ = now_;
            }
            if (node.transmits < std::numeric_limits<std::uint8_t>::max()) {
                ++node.transmits;
            }
            if (node.flags.has(wire::TcpFlag::Syn) || node.flags.has(wire::TcpFlag::Fin)) {
                ++counters_.control_segments_sent;
            }
            conn.unresolved_polls_ = 0;
        }

        /// Queues a SYN or SYN-ACK at `sequence` and transmits it. The entry stays when the send fails, for the
        /// timer; `Refused`, counted, when no node is free.
        [[nodiscard]] std::expected<void, net::SendError>
        queue_control(ConnectionType& conn, const wire::TcpFlags flags, const Sequence sequence) noexcept {
            const std::optional<std::uint32_t> entry = enqueue(conn, Packet{}, 0, flags, sequence, 1);
            if (!entry) {
                ++counters_.send_refused;
                return std::unexpected{net::SendError::Refused};
            }
            return transmit_entry(conn, *entry);
        }

        /**
         * Frees every entry from the head that `ack` covers whole and returns what it freed; an entry covered in part
         * stays whole, trimmed only if it is resent. Each entry it reaches is reconciled first, so a copy that came
         * back leaves its range. What `ack` covers past `snd_una`, less the SYN and FIN of the entries freed, counts
         * as delivered, a part of an entry included. A resend that waited on the deferred list is moot once its entry
         * is freed; the packets of a freed entry go back now or when the device returns them. Called before `snd_una`
         * moves.
         */
        [[nodiscard]] Freed acknowledge(ConnectionType& conn, const Sequence ack) noexcept {
            Freed freed{};
            std::uint32_t control = 0;  // the SYN and FIN sequence space of the entries freed
            while (conn.queue_head_ != detail::no_node) {
                SentNode& node = send_pool_.node(conn.queue_head_);
                reconcile(node);
                if ((node.first + node.length).after(ack)) {
                    break;
                }
                if (freed.length == 0) {
                    freed.sent = node.sent;  // the oldest entry freed
                }
                freed.length             += node.length;
                freed.resent              = freed.resent || node.transmits != 1;
                freed.newest_sent         = node.sent;
                freed.delivered_at_send   = node.delivered_at_send;
                freed.app_limited         = node.app_limited;
                control                  += node.length - node.payload;
                const std::uint32_t next  = node.next;
                send_pool_.release(conn.queue_head_);
                conn.queue_head_ = next;
                --conn.queue_count_;
            }
            if (conn.queue_head_ == detail::no_node) {
                conn.queue_tail_ = detail::no_node;
            }
            if (freed.length > 0) {
                deferred_.remove(conn);
            }
            if (const auto covered = static_cast<std::uint32_t>(ack - conn.snd_una_); covered > control) {
                conn.delivered_      += covered - control;
                conn.delivered_time_  = now_;
            }
            return freed;
        }

        /**
         * The head entry again. A copy out is reconciled first. Still lent: the device is asked to reclaim, and if it
         * still holds the packet nothing is written into it and the busy rule decides (`resend_busy`). Back: when
         * `snd_una` passed the entry's first byte, the headers move forward over the acknowledged front, which is
         * trimmed off, and `transmit_entry` sends what is left. IP's error, or nothing when the resend left or was
         * deferred. A send that failed leaves the deferred list too: the timer's next expiry tries again.
         */
        [[nodiscard]] std::expected<void, net::SendError> resend_head(ConnectionType& conn) noexcept {
            assert(conn.queue_head_ != detail::no_node && "a resend with nothing queued");
            SentNode& node = send_pool_.node(conn.queue_head_);
            reconcile(node);
            if (node.packet.lent() && ip_->queue().reclaim() > 0) {
                reconcile(node);  // what the device let go of may be either packet
            }
            if (node.packet.lent()) {
                return resend_busy(conn, node);
            }
            if (conn.snd_una_.after(node.first)) {
                const auto acknowledged = static_cast<std::uint32_t>(conn.snd_una_ - node.first);
                assert(node.framed && acknowledged < node.payload && "only data is acknowledged in part");
                const std::span<std::byte> frame = node.packet.data();
                std::memmove(frame.subspan(acknowledged).data(), frame.data(), frame.size() - node.payload);
                node.packet.trim_front(acknowledged);
                node.first    = node.first + acknowledged;
                node.length  -= acknowledged;
                node.payload  = static_cast<std::uint16_t>(node.payload - acknowledged);
            }
            deferred_.remove(conn);  // the packet is back: whatever IP answers, this resend is not waiting any more
            const std::expected<void, net::SendError> sent = transmit_entry(conn, conn.queue_head_);
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
        [[nodiscard]] std::expected<void, net::SendError> resend_busy(ConnectionType& conn, SentNode& node) noexcept {
            if (deferred_.contains(conn) && node.deferred_at == now_) {
                return {};
            }
            if (node.deferrals < config_.retransmit_defers) {
                ++node.deferrals;
            } else if (node.copy.empty()) {
                return send_copy(conn, node);
            }
            node.deferred_at = now_;
            if (!deferred_.contains(conn)) {
                deferred_.push_back(conn);
            }
            ++counters_.retransmits_deferred;
            return {};
        }

        /**
         * The copy fallback: a fresh packet with the lent packet's header bytes and the part of its payload not yet
         * acknowledged, read from the lent packet and never written into it, framed by `emit` from `max(first,
         * snd_una)` and lent as the entry's second packet; counted in `retransmits_copied`. No packet free counts
         * `allocation_failures` and refuses; a refused send drops the copy. Either way the entry stays as it was and
         * the connection leaves the deferred list: the timer's next expiry tries again.
         */
        [[nodiscard]] std::expected<void, net::SendError> send_copy(ConnectionType& conn, SentNode& node) noexcept {
            assert(node.framed && node.copy.empty() && "one copy at a time, of a frame that left");
            deferred_.remove(conn);
            const Sequence first    = conn.snd_una_.after(node.first) ? conn.snd_una_ : node.first;
            const auto acknowledged = static_cast<std::uint32_t>(first - node.first);
            assert((acknowledged == 0 || acknowledged < node.payload) && "only data is acknowledged in part");
            const auto payload                     = static_cast<std::uint16_t>(node.payload - acknowledged);
            const std::span<const std::byte> frame = node.packet.data();  // read only: the device holds the packet
            const std::size_t header_size          = frame.size() - node.payload;
            std::optional<Packet> copy             = ip_->allocate();
            if (!copy) {
                ++counters_.allocation_failures;
                return std::unexpected{net::SendError::Refused};
            }
            const std::optional<std::span<std::byte>> bytes = copy->append(header_size + payload);
            assert(bytes.has_value() && "a packet from allocate() holds the frame it copies");
            if (!bytes) {
                ++counters_.allocation_failures;  // unreachable by construction; counted in a release build
                return std::unexpected{net::SendError::Refused};
            }
            std::ranges::copy(frame.first(header_size), bytes->begin());
            std::ranges::copy(frame.subspan(header_size + acknowledged), bytes->subspan(header_size).begin());
            if (const std::expected<void, net::SendError> sent =
                    emit(conn, *copy, true, first, node.flags, node.flags.has(wire::TcpFlag::Syn));
                !sent) {
                return sent;  // the copy goes back to the pool
            }
            node.copy         = std::move(*copy);
            node.copy_first   = first;
            node.copy_payload = payload;
            node.deferrals    = 0;
            record_transmission(conn, node);
            ++counters_.retransmits_copied;
            return {};
        }

        /**
         * Settles an entry with a copy out once either packet is back. The original back: the copy is dropped. The
         * copy back first: it replaces the original, and the entry shrinks to the copy's range; the front it left out
         * was counted as delivered by the ACK that passed it. A dropped handle's buffer goes back now or when the
         * device returns it. Nothing with no copy out, or with both packets still lent.
         */
        static void reconcile(SentNode& node) noexcept {
            if (node.copy.empty()) {
                return;
            }
            if (!node.packet.lent()) {
                node.copy = Packet{};
                return;
            }
            if (node.copy.lent()) {
                return;
            }
            node.packet  = std::exchange(node.copy, Packet{});
            node.first   = node.copy_first;
            node.payload = node.copy_payload;
            node.length  = node.payload + (node.flags.has(wire::TcpFlag::Syn) ? 1U : 0U) +
                           (node.flags.has(wire::TcpFlag::Fin) ? 1U : 0U);
        }

        /// A header-only ACK at `sequence` from a fresh packet: lent to the device and forgotten, never kept. A failure
        /// is counted.
        [[nodiscard]] bool send_bare_ack(ConnectionType& conn, const Sequence sequence) noexcept {
            std::optional<Packet> packet = ip_->allocate();
            if (!packet) {
                ++counters_.send_refused;
                return false;
            }
            if (!emit(conn, *packet, false, sequence, wire::TcpFlags{wire::TcpFlag::Ack}, false)) {
                return false;  // the packet goes back to the pool
            }
            *packet = Packet{};  // the device frees the buffer when it is done
            return true;
        }

        /// A pure ACK at `snd_nxt`.
        [[nodiscard]] bool send_pure_ack(ConnectionType& conn) noexcept {
            if (!send_bare_ack(conn, conn.snd_nxt_)) {
                return false;
            }
            ++counters_.pure_acks_sent;
            return true;
        }

        /// A zero-window probe, Linux's `tcp_xmit_probe_skb`: a pure ACK at `snd_una - 1`, which occupies no sequence
        /// space and which the peer, finding it outside its window, answers with an ACK carrying its window.
        [[nodiscard]] bool send_probe(ConnectionType& conn) noexcept {
            if (!send_bare_ack(conn, Sequence{conn.snd_una_.value - 1})) {
                return false;
            }
            ++counters_.zero_window_probes;
            return true;
        }

        /// abort's RST, at `snd_nxt` with the ACK, window and timestamps an ordinary segment carries (RFC 7323: should
        /// carry them, as Linux's does): through `Ipv4::send` and forgotten, since nothing of it is kept.
        [[nodiscard]] bool send_reset(ConnectionType& conn) noexcept {
            std::optional<Packet> packet = ip_->allocate();
            if (!packet) {
                ++counters_.send_refused;
                return false;
            }
            const std::size_t tcp_size =
                wire::TcpHeader::size + (conn.send_timestamps_ ? wire::TcpOptions::timestamps_size : 0);
            const std::optional<std::span<std::byte>> room = packet->append(tcp_size);
            if (!room) {
                ++counters_.send_refused;
                return false;
            }
            const Sequence offer = offer_of(conn);
            wire::TcpHeader{
                .source_port      = conn.local_.port,
                .destination_port = conn.remote_.port,
                .sequence         = conn.snd_nxt_,
                .acknowledgement  = conn.rcv_nxt_,
                .data_offset      = static_cast<std::uint8_t>(tcp_size),
                .flags            = wire::TcpFlags{wire::TcpFlag::Rst, wire::TcpFlag::Ack},
                .window           = static_cast<std::uint16_t>(offer.value - conn.rcv_nxt_.value)
            }
                .write(*room);
            if (conn.send_timestamps_) {
                wire::TcpOptions::write_timestamps(room->subspan(wire::TcpHeader::size), ts_now(conn), conn.ts_recent_);
            }
            const auto sent = ip_->send(std::move(*packet),
                                        net::SendRequest{.destination = conn.remote_.address,
                                                         .protocol    = wire::Ipv4Protocol::Tcp,
                                                         .checksum    = device::L4Checksum::Tcp},
                                        now_);
            if (!sent) {
                count_failure(sent.error());  // the packet is still in `packet` and goes back to the pool with it
                return false;
            }
            record_offer(conn, offer);
            ++counters_.resets_sent;
            return true;
        }

        /// An ordinary ACK: marked, sent by flush unless a data segment carries it first.
        void queue_ack(ConnectionType& conn) noexcept {
            conn.ack_pending_ = true;
            if (!acks_.contains(conn)) {
                acks_.push_back(conn);
            }
        }

        /// An immediate ACK: queued through IP before the next segment is processed; a refusal leaves it pending. When
        /// the close only waited for this ACK, Closed follows it, whichever segment prompted it.
        void ack_now(ConnectionType& conn) noexcept {
            if (send_pure_ack(conn)) {
                maybe_finish(conn);
            } else {
                queue_ack(conn);
            }
        }

        /// A Writable hint at the next process, at most one per marked connection.
        void mark_retry(ConnectionType& conn) noexcept {
            if (!retries_.contains(conn)) {
                retries_.push_back(conn);
            }
        }

        [[nodiscard]] std::size_t writable(const ConnectionType& conn) const noexcept {
            if (conn.prepared_ || (conn.state_ != State::Established && conn.state_ != State::CloseWait)) {
                return 0;
            }
            if (conn.queue_count_ >= config_.send_segments) {
                return 0;  // the send cap: the ACK that frees an entry raises Writable
            }
            return static_cast<std::size_t>(std::min<std::uint64_t>(conn.mss(), send_room(conn)));
        }

        /// What the windows allow beyond the flight: the congestion window with limited transmit's allowance, within
        /// the peer's window, less what is in flight, and zero when the flight fills it. In 64 bits, since
        /// `NoCongestionControl`'s window is the largest 32-bit value.
        [[nodiscard]] static std::uint64_t send_room(const ConnectionType& conn) noexcept {
            const std::uint64_t flight = conn.snd_nxt_.value - conn.snd_una_.value;
            const std::uint64_t limit  = std::min<std::uint64_t>(
                conn.snd_wnd_, std::uint64_t{CongestionControl::window(conn.congestion_)} + conn.limited_transmit_);
            return limit > flight ? limit - flight : 0;
        }

        /**
         * A span of `min(count, writable())` bytes inside a fresh packet. The packet belongs to the application until
         * `commit` (with any count) or `release`: the span stays valid through any state change in between. Nothing,
         * without allocating, for a count of zero or nothing writable; an allocation failure counts one
         * `allocation_failures`, marks a retry hint and retains nothing.
         */
        [[nodiscard]] std::optional<std::span<std::byte>> prepare(ConnectionType& conn,
                                                                  const std::size_t count) noexcept {
            assert(!conn.prepared_ && "one prepare at a time: commit first");
            if (count == 0) {
                return std::nullopt;
            }
            const std::size_t room = writable(conn);
            if (room == 0) {
                return std::nullopt;  // nothing allocates when nothing is writable
            }
            const std::size_t size       = std::min(count, room);
            std::optional<Packet> packet = ip_->allocate();
            if (!packet) {
                ++counters_.allocation_failures;
                mark_retry(conn);  // the pool may have packets by the next process
                return std::nullopt;
            }
            const std::optional<std::span<std::byte>> span = packet->append(size);
            assert(span && "size is at most the MSS and a packet holds a whole frame");
            if (!span) {
                ++counters_.allocation_failures;  // unreachable by construction; counted in a release build
                mark_retry(conn);
                return std::nullopt;
            }
            conn.prepared_      = std::move(*packet);  // the span points into the block, which does not move
            conn.prepared_size_ = size;
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
        [[nodiscard]] bool commit(ConnectionType& conn, const std::size_t count) noexcept {
            assert(conn.prepared_ && "commit without a prepare");
            if (!conn.prepared_) {
                ++counters_.commits_refused;  // a contract violation, refused in a release build
                return false;
            }
            assert(count <= conn.prepared_size_ && "commit more than was prepared");
            Packet packet = std::move(*conn.prepared_);
            conn.prepared_.reset();
            const std::size_t prepared = conn.prepared_size_;
            conn.prepared_size_        = 0;
            if (count == 0) {
                return true;  // discarded: `packet` goes back to the pool; nothing was refused
            }
            if (conn.state_ != State::Established && conn.state_ != State::CloseWait) {
                ++counters_.commits_refused;  // closed since the prepare: no hint, the events say why
                return false;
            }
            if (count > writable(conn)) {     // the preparation is released, so this is the credit the peer offers now
                ++counters_.commits_refused;  // RFC 1122: a sender copes with a shrunk window by not sending into it
                mark_retry(conn);
                return false;
            }
            packet.trim_back(prepared - count);
            if (conn.queue_count_ == 0 && conn.last_data_send_ && now_ - *conn.last_data_send_ > conn.rto_) {
                CongestionControl::on_idle(conn.congestion_, conn.mss());  // RFC 5681's restart after an idle RTO
            }
            const auto payload                       = static_cast<std::uint16_t>(count);
            const std::uint32_t previous_tail        = conn.queue_tail_;
            const std::optional<std::uint32_t> entry = enqueue(conn,
                                                               std::move(packet),
                                                               payload,
                                                               wire::TcpFlags{wire::TcpFlag::Ack, wire::TcpFlag::Psh},
                                                               conn.snd_nxt_,
                                                               payload);
            if (!entry) {
                ++counters_.allocation_failures;  // no queue node free: the packet goes back to the pool
                mark_retry(conn);
                return false;
            }
            if (!transmit_entry(conn, *entry)) {
                unlink_tail(conn, previous_tail);  // the packet goes back to the pool with the node
                mark_retry(conn);                  // emit counted the refusal
                return false;
            }
            conn.snd_nxt_ = conn.snd_nxt_ + static_cast<std::uint32_t>(count);
            ++counters_.data_segments_sent;
            return true;
        }

        /// Advances the chain's front by `count` bytes, freeing packets as they empty; a reopened edge queues an
        /// update.
        void consume(ConnectionType& conn, const std::size_t count) noexcept {
            assert(count <= conn.unread_bytes_ && "consume past unread()");
            std::size_t remaining = count;
            while (remaining > 0) {
                typename detail::TcpNodePool<Packet>::Node& node  = pool_.node(conn.chain_head_);
                const std::size_t taken                           = std::min<std::size_t>(remaining, node.length);
                node.offset                                       = static_cast<std::uint16_t>(node.offset + taken);
                node.length                                       = static_cast<std::uint16_t>(node.length - taken);
                remaining                                        -= taken;
                conn.unread_bytes_                               -= taken;
                if (node.length == 0) {
                    const std::uint32_t next = node.next;
                    pool_.release(conn.chain_head_);
                    conn.chain_head_ = next;
                    if (next == detail::no_node) {
                        conn.chain_tail_ = detail::no_node;
                    }
                    --conn.chain_count_;
                }
            }
            const bool receiving =
                conn.state_ == State::Established || conn.state_ == State::FinWait1 || conn.state_ == State::FinWait2;
            if (!receiving) {
                return;  // the peer sends no more data, or the connection is closed: no offer to make
            }
            if (offer_of(conn) != conn.rcv_adv_) {
                queue_ack(conn);  // an ordinary window update, including the reopening of a zero window
            }
        }

        // The control path: events, slots, the port choice, the retransmit and probe timers, close, abort and release.

        void raise(ConnectionType& conn, const stream::Event event) noexcept {
            conn.events_ |= event;
            if (!events_.contains(conn)) {
                events_.push_back(conn);
            }
        }

        [[nodiscard]] std::optional<std::uint32_t> take_slot() noexcept {
            if (free_.empty()) {
                return std::nullopt;
            }
            const std::uint32_t index = free_.back();
            free_.pop_back();
            return index;
        }

        void free_slot(ConnectionType& conn) noexcept {
            // Every live connection is indexed by connect or passive_open; a free slot is not, and pushing its index
            // again would hand one slot to two opens. A release build ignores the second call.
            assert(conn.indexed_ && "release of a free slot");
            if (!conn.indexed_) {
                return;
            }
            disarm(conn);
            std::ignore = table_.erase(conn.key_, conn.base_hash_);
            return_chain(conn);
            drop_queue(conn);
            conn.prepared_.reset();
            events_.remove(conn);
            acks_.remove(conn);
            retries_.remove(conn);
            conn.clear();
            free_.push_back(conn.index_);
        }

        void return_chain(ConnectionType& conn) noexcept {
            for (std::uint32_t index = conn.chain_head_; index != detail::no_node;) {
                const std::uint32_t next = pool_.node(index).next;
                pool_.release(index);
                index = next;
            }
            conn.chain_head_   = detail::no_node;
            conn.chain_tail_   = detail::no_node;
            conn.chain_count_  = 0;
            conn.unread_bytes_ = 0;
        }

        /// Every entry back to the pool: the node now, its packet and a copy out now or when the device returns them,
        /// each exactly once; and the node set aside for a FIN that never came, any node's again.
        void drop_queue(ConnectionType& conn) noexcept {
            for (std::uint32_t index = conn.queue_head_; index != detail::no_node;) {
                const std::uint32_t next = send_pool_.node(index).next;
                send_pool_.release(index);
                index = next;
            }
            conn.queue_head_  = detail::no_node;
            conn.queue_tail_  = detail::no_node;
            conn.queue_count_ = 0;
            if (std::exchange(conn.fin_reserved_, false)) {
                send_pool_.unreserve();
            }
            deferred_.remove(conn);
        }

        [[nodiscard]] Sequence draw_isn() noexcept {
            if (forced_isn_) {
                const Sequence isn = *forced_isn_;
                forced_isn_.reset();
                return isn;
            }
            return Sequence{static_cast<std::uint32_t>(isn_engine_())};
        }

        [[nodiscard]] std::expected<std::uint16_t, ConnectError> choose_port(const Endpoint peer,
                                                                             std::uint32_t& base_hash) noexcept {
            const device::RssDescription& steering = ip_->queue().steering();
            const std::uint16_t mine               = ip_->queue().index();
            device::FlowTuple tuple{.source           = peer.address,  // received orientation: the peer is the source
                                    .destination      = ip_->address(),
                                    .source_port      = peer.port,
                                    .destination_port = 0,
                                    .protocol         = wire::Ipv4Protocol::Tcp};
            if (!hardware_hash_) {
                // Steering off, or addresses only: the queue the replies land on does not depend on the port.
                if (device::queue_for(steering, tuple) != mine) {
                    return std::unexpected{ConnectError::Unplaceable};
                }
            }
            const std::uint32_t range =
                static_cast<std::uint32_t>(config_.ephemeral_last - config_.ephemeral_first) + 1U;
            for (std::uint32_t tried = 0; tried < range; ++tried) {
                const std::uint16_t port = cursor_;
                cursor_                  = cursor_ == config_.ephemeral_last ? config_.ephemeral_first
                                                                             : static_cast<std::uint16_t>(cursor_ + 1);
                if (listening(port)) {
                    continue;
                }
                tuple.destination_port = port;
                if (hardware_hash_ && device::queue_for(steering, tuple) != mine) {
                    continue;
                }
                const FlowKey key{.remote = peer.address, .remote_port = peer.port, .local_port = port};
                const std::uint32_t hash =
                    hardware_hash_ ? device::flow_hash(steering, tuple) : detail::software_flow_hash(key);
                if (table_.find(key, hash)) {
                    continue;
                }
                base_hash = hash;
                return port;
            }
            return std::unexpected{ConnectError::NoPort};
        }

        /// The RTO from now; under a user timeout on a synchronized connection, never later than the head's first
        /// transmission plus the timeout, Linux's `tcp_clamp_rto_to_user_timeout`, and never earlier than now.
        void arm_retransmit(ConnectionType& conn) noexcept {
            core::TimePoint deadline = now_ + conn.rto_;
            if (const SentNode* head = user_timed_head(conn); head != nullptr) {
                deadline = std::min(deadline, std::max(now_, head->first_sent + config_.user_timeout));
            }
            conn.timer_kind_ = detail::TimerKind::Retransmit;
            wheel_->arm(conn.timer_, deadline);
        }

        /// A SYN, SYN-ACK or FIN whose first send found no next hop: tried again every `unresolved_retry`, as no retry.
        void arm_poll(ConnectionType& conn) noexcept {
            conn.timer_kind_ = detail::TimerKind::ArpPoll;
            wheel_->arm(conn.timer_, now_ + config_.unresolved_retry);
        }

        /// The next probe, `probe_interval_` from now; under a user timeout, while a probe is unanswered, never later
        /// than the first unanswered probe plus the timeout, Linux's `tcp_clamp_probe0_to_user_timeout`, and never
        /// earlier than now. An answer leaves the deadline as it was, so while the peer answers a probe goes at least
        /// once per user timeout, and a peer that falls silent is found within about twice the timeout.
        void arm_probe(ConnectionType& conn) noexcept {
            core::TimePoint deadline = now_ + conn.probe_interval_;
            if (config_.user_timeout > core::Duration::zero() && conn.probe_started_) {
                deadline = std::min(deadline, std::max(now_, *conn.probe_started_ + config_.user_timeout));
            }
            conn.timer_kind_ = detail::TimerKind::Probe;
            wheel_->arm(conn.timer_, deadline);
        }

        /// The peer's window is zero with nothing in flight: no probe out yet, nothing unanswered stamped, and the
        /// first probe one RTO away.
        void start_probing(ConnectionType& conn) noexcept {
            conn.probes_out_     = 0;
            conn.probe_started_  = std::nullopt;
            conn.probe_interval_ = conn.rto_;
            arm_probe(conn);
        }

        void disarm(ConnectionType& conn) noexcept {
            wheel_->cancel(conn.timer_);
            conn.timer_kind_ = detail::TimerKind::None;
        }

        /// The entry the user timeout counts from: the head of the queue once it has left, on a synchronized
        /// connection, with the timeout set. Null otherwise: the handshake is bounded by `retries` alone.
        [[nodiscard]] const SentNode* user_timed_head(const ConnectionType& conn) const noexcept {
            if (config_.user_timeout <= core::Duration::zero() || conn.state_ == State::SynSent ||
                conn.state_ == State::SynReceived || conn.queue_head_ == detail::no_node) {
                return nullptr;
            }
            const SentNode& head = send_pool_.node(conn.queue_head_);
            return head.transmits > 0 ? &head : nullptr;
        }

        /// Whether a segment the peer can acknowledge is out: the head left at least once. A queue whose head never
        /// left, held for the peer's window or the next hop, has nothing in flight; entries leave in order, so no
        /// later entry left before the head.
        [[nodiscard]] bool in_flight(const ConnectionType& conn) const noexcept {
            return conn.queue_head_ != detail::no_node && send_pool_.node(conn.queue_head_).transmits > 0;
        }

        /// RFC 9293's send window test, Linux's `tcp_snd_wnd_test`: the peer's window covers a segment ending at
        /// `end`. The congestion window does not gate a FIN, as it does not in Linux.
        [[nodiscard]] static bool covered(const ConnectionType& conn, const Sequence end) noexcept {
            return !end.after(conn.snd_una_ + conn.snd_wnd_);
        }

        /**
         * One round-trip sample into RFC 6298's estimator: the first sets `srtt = R` and `rttvar = R / 2`, each later
         * one `rttvar = 3/4 rttvar + 1/4 |srtt - R|` and then `srtt = 7/8 srtt + 1/8 R`, in integer nanoseconds,
         * truncating. The RTO follows with Linux's floor and ceiling, `srtt + max(4 rttvar, rto_min)` capped at
         * `rto_max`, which ends any backoff.
         */
        void take_sample(ConnectionType& conn, const core::Duration sample) noexcept {
            if (!conn.srtt_) {
                conn.srtt_   = sample;
                conn.rttvar_ = sample / 2;
            } else {
                conn.rttvar_ = (3 * conn.rttvar_ + std::chrono::abs(*conn.srtt_ - sample)) / 4;
                conn.srtt_   = (7 * *conn.srtt_ + sample) / 8;
            }
            conn.rto_ = std::min(*conn.srtt_ + std::max(4 * conn.rttvar_, config_.rto_min), config_.rto_max);
            ++counters_.rtt_samples;
        }

        /**
         * `snd_una` moved forward. The retry count clears. The round-trip sample follows Linux's `tcp_clean_rtx_queue`
         * and `tcp_ack_update_rtt`: from the oldest entry the ACK freed, measured from its transmission, when no entry
         * it freed left more than once (Karn's rule); else, on a connection with timestamps, from the ACK's echo, which
         * names the transmission it answers, so the echo on the ACK of a resend ends the backoff too. An echo of zero
         * or from the future gives none. The timer restarts from now while anything is in flight and stops when
         * nothing is, RFC 6298's rule, which covers RFC 6582's re-arm on a partial ACK; a head that never left, the
         * FIN a close holds for the window, is `send_unsent`'s or the probes'. A partial ACK frees nothing, samples
         * only from its echo, and restarts the timer. Returns the sample taken, if any.
         */
        std::optional<core::Duration>
        on_acknowledged(ConnectionType& conn, const Freed& freed, const std::optional<std::uint32_t> echo) noexcept {
            conn.retransmit_count_ = 0;
            std::optional<core::Duration> sample{};
            if (freed.length > 0 && !freed.resent) {
                sample = std::max<core::Duration>(now_ - freed.sent, core::Duration::zero());
            } else if (conn.send_timestamps_ && echo && *echo != 0) {
                if (const auto elapsed = static_cast<std::uint32_t>(ts_now(conn) - *echo);
                    static_cast<std::int32_t>(elapsed) >= 0) {
                    sample = std::chrono::milliseconds{elapsed};
                }
            }
            if (sample) {
                take_sample(conn, *sample);
            }
            if (in_flight(conn)) {
                arm_retransmit(conn);
            } else {
                disarm(conn);
            }
            return sample;
        }

        /**
         * An ACK of new data on a synchronized connection: the entries it frees, the sample and the timer
         * (`on_acknowledged`), then loss recovery; the duplicate count and limited transmit's allowance clear. In fast
         * recovery, an ACK past `recover` is the full ACK and ends it, the policy told the flight left; one short of it
         * is RFC 6582's partial ACK: the new head goes again at once (`fast_retransmit`), and the policy is told what
         * was acknowledged. Outside fast recovery the policy hears of the ACK (`on_ack`); in the loss episode
         * after a timeout an ACK short of `recover` resends the new head at once, counted in `retransmits`, as Linux
         * resends what its loss state marked lost (RACK refines this later), and one past it ends the episode. Outside
         * both, `recover` slides to one before the new `snd_una`.
         */
        void
        acknowledge_data(ConnectionType& conn, const Sequence ack, const std::optional<std::uint32_t> echo) noexcept {
            const std::uint32_t flight_before       = conn.snd_nxt_.value - conn.snd_una_.value;
            const auto acked                        = static_cast<std::uint32_t>(ack - conn.snd_una_);
            const Freed freed                       = acknowledge(conn, ack);  // a partial entry stays whole
            conn.snd_una_                           = ack;
            const std::optional<core::Duration> rtt = on_acknowledged(conn, freed, echo);
            conn.dupacks_                           = 0;
            conn.limited_transmit_                  = 0;
            const std::uint16_t smss                = conn.mss();
            if (conn.in_recovery_) {
                if (ack.after(conn.recover_)) {
                    conn.in_recovery_ = false;
                    CongestionControl::on_recovery_end(
                        conn.congestion_, conn.snd_nxt_.value - conn.snd_una_.value, smss);
                    return;
                }
                fast_retransmit(conn);
                CongestionControl::on_partial_ack(conn.congestion_, acked, smss);
                return;
            }
            CongestionControl::on_ack(conn.congestion_,
                                      AckInfo{.acked             = acked,
                                              .flight            = flight_before,
                                              .mss               = smss,
                                              .rtt               = rtt,
                                              .now               = now_,
                                              .sent              = freed.newest_sent,
                                              .delivered         = conn.delivered_,
                                              .delivered_at_send = freed.delivered_at_send,
                                              .app_limited       = freed.app_limited});
            if (!conn.loss_episode_) {
                // Outside recovery and the loss episode, `recover` follows one before `snd_una`. The guard compares in
                // serial arithmetic, which reads a `recover` left more than 2^31 behind `snd_una` as ahead of it, so a
                // `recover` set at the open or by the last episode would refuse every fast retransmit after 2 GiB.
                conn.recover_ = Sequence{ack.value - 1};
                return;
            }
            if (ack.after(conn.recover_)) {
                conn.loss_episode_ = false;
                return;
            }
            if (conn.queue_head_ != detail::no_node) {
                std::ignore = resend_head(conn);
            }
            ++counters_.retransmits;
        }

        /**
         * RFC 5681's duplicate ACK. In fast recovery the policy hears of it; NewReno inflates its window a segment.
         * Outside it the first and second each allow a segment beyond the windows, RFC 3042's limited transmit, an
         * allowance of the stack's; the third starts fast recovery only when, in RFC 6582's words, "the Cumulative
         * Acknowledgment field covers more than recover", that is `ack` past `recover`: the guard against a second fast
         * retransmit for data a timeout already resent. Fast recovery records `recover`, tells the policy of the loss
         * with the flight less what limited transmit sent past the windows, which RFC 5681 leaves out of the
         * threshold, and resends the head at once (`fast_retransmit`). Later duplicates outside recovery do nothing.
         */
        void on_duplicate_ack(ConnectionType& conn, const Sequence ack) noexcept {
            ++counters_.duplicate_acks;
            if (conn.dupacks_ < std::numeric_limits<std::uint8_t>::max()) {
                ++conn.dupacks_;
            }
            const std::uint16_t smss = conn.mss();
            if (conn.in_recovery_) {
                CongestionControl::on_dupack(conn.congestion_, smss);
                return;
            }
            if (conn.dupacks_ < 3) {
                conn.limited_transmit_ = std::min<std::uint32_t>(conn.limited_transmit_ + smss, 2U * smss);
                return;
            }
            if (conn.dupacks_ != 3 || !ack.after(conn.recover_)) {
                return;  // RFC 6582: the acknowledgement does not cover more than recover
            }
            const std::uint64_t flight = conn.snd_nxt_.value - conn.snd_una_.value;
            const std::uint64_t window =
                std::min<std::uint64_t>(conn.snd_wnd_, CongestionControl::window(conn.congestion_));
            conn.recover_          = Sequence{conn.snd_nxt_.value - 1};
            conn.in_recovery_      = true;
            conn.limited_transmit_ = 0;
            CongestionControl::on_loss(conn.congestion_, static_cast<std::uint32_t>(std::min(flight, window)), smss);
            fast_retransmit(conn);
            ++counters_.recoveries;
        }

        /**
         * The head again, driven by the third duplicate or a partial ACK, counted in `fast_retransmits`. When it
         * leaves, in place or as a copy, the RTO restarts from now, as Linux re-arms it on resending the head
         * (`tcp_xmit_retransmit_queue`), so the timer the first send armed does not expire before the resend's ACK can
         * return. A resend deferred, or refused, leaves the timer as it was.
         */
        void fast_retransmit(ConnectionType& conn) noexcept {
            ++counters_.fast_retransmits;
            if (!in_flight(conn)) {
                return;  // nothing in flight: the head, if any, waits for the window
            }
            if (resend_head(conn) && !deferred_.contains(conn)) {
                arm_retransmit(conn);
            }
        }

        /// Called by the wheel with the timer unarmed; its kind says what for.
        void on_timer(ConnectionType& conn) noexcept {
            const detail::TimerKind kind = std::exchange(conn.timer_kind_, detail::TimerKind::None);
            assert(kind != detail::TimerKind::None && "the wheel fires only an armed timer");
            switch (kind) {
                case detail::TimerKind::Retransmit:
                    on_retransmit_timeout(conn);
                    break;
                case detail::TimerKind::ArpPoll:
                    on_arp_poll(conn);
                    break;
                case detail::TimerKind::Probe:
                    on_probe_timeout(conn);
                    break;
                case detail::TimerKind::None:
                    break;
            }
        }

        /**
         * The RTO ran out: one retry, the head of the queue again. First the user timeout, counted from the head's
         * first transmission on a synchronized connection, and then the cap, `retries` in the handshake and
         * `data_retries` after it, may end the connection instead. The first expiry since an ACK of new data leaves
         * fast recovery, records `recover` and starts the loss episode, and tells the congestion control, with what
         * was in flight; each expiry doubles the RTO up to `rto_max` and re-arms. A resend deferred or refused counts
         * as the retry all the same: the next expiry tries again. A first send, of a SYN, SYN-ACK or FIN the device
         * refused before, that finds no next hop arms the ARP poll instead (`transmit_entry`), and the expiry leaves
         * the timer to it: nothing left, so no backoff; the poll arms the RTO once the send leaves, or ends the
         * connection `Unresolved`.
         */
        void on_retransmit_timeout(ConnectionType& conn) noexcept {
            assert(conn.queue_head_ != detail::no_node && "the RTO runs while something is queued");
            if (const SentNode* head = user_timed_head(conn);
                head != nullptr && now_ - head->first_sent >= config_.user_timeout) {
                time_out(conn, /*by_user_timeout=*/true);
                return;
            }
            if (const bool handshake = conn.state_ == State::SynSent || conn.state_ == State::SynReceived;
                conn.retransmit_count_ >= (handshake ? config_.retries : config_.data_retries)) {
                time_out(conn, /*by_user_timeout=*/false);
                return;
            }
            if (++conn.retransmit_count_ == 1) {
                // RFC 6582 on a retransmit timeout: record the highest sequence number transmitted in recover and
                // leave fast recovery; the loss episode resends what was sent before it, one head per ACK of new data.
                conn.in_recovery_      = false;
                conn.loss_episode_     = true;
                conn.recover_          = Sequence{conn.snd_nxt_.value - 1};
                conn.dupacks_          = 0;
                conn.limited_transmit_ = 0;
                CongestionControl::on_timeout(conn.congestion_, conn.snd_nxt_.value - conn.snd_una_.value, conn.mss());
            }
            std::ignore = resend_head(conn);
            ++counters_.retransmits;
            if (conn.timer_kind_ == detail::TimerKind::ArpPoll) {
                return;  // a first send that found no next hop: the poll retries it, and arms the RTO once it leaves
            }
            conn.rto_ = std::min(conn.rto_ * 2, config_.rto_max);
            arm_retransmit(conn);
        }

        /// The ARP poll: the SYN, SYN-ACK or FIN that never left is tried again, as no retry. Once it leaves, or the
        /// device refuses it, the RTO takes over; a next hop still silent after `unresolved_polls` polls ends the
        /// connection with `Unresolved`.
        void on_arp_poll(ConnectionType& conn) noexcept {
            assert(conn.queue_head_ != detail::no_node && "the poll retries an entry that never left");
            if (const std::expected<void, net::SendError> sent = transmit_entry(conn, conn.queue_head_);
                sent || sent.error() != net::SendError::Unresolved) {
                arm_retransmit(conn);
                return;
            }
            if (++conn.unresolved_polls_ >= config_.unresolved_polls) {
                fail_unresolved(conn);  // the bound Linux's ARP probes give: the next hop is not going to answer
                return;
            }
            arm_poll(conn);
        }

        /**
         * The probe timer ran out with the peer's window still zero. The first expiry since the last acceptable ACK
         * stamps the first unanswered probe, which the user timeout counts from, as Linux's `tcp_probe_timer` stamps
         * `icsk_probes_tstamp` but cleared by every answer (`update_probing`); a later expiry ends the connection once
         * the timeout passed since that stamp. Then `data_retries` unanswered probes may end it instead; else one probe
         * goes, it counts as unanswered until an acceptable ACK clears the count, and the interval doubles up to
         * `rto_max`. A probe refused counts all the same: the next expiry tries again.
         */
        void on_probe_timeout(ConnectionType& conn) noexcept {
            assert(!in_flight(conn) && conn.snd_wnd_ == 0 &&
                   (conn.queue_head_ != detail::no_node || conn.state_ == State::Established ||
                    conn.state_ == State::CloseWait) &&
                   "a probe runs only while the window is zero and nothing is in flight");
            if (!conn.probe_started_) {
                conn.probe_started_ = now_;
            } else if (config_.user_timeout > core::Duration::zero() &&
                       now_ - *conn.probe_started_ >= config_.user_timeout) {
                time_out(conn, /*by_user_timeout=*/true);
                return;
            }
            if (conn.probes_out_ >= config_.data_retries) {
                time_out(conn, /*by_user_timeout=*/false);
                return;
            }
            std::ignore = send_probe(conn);  // a refusal was counted by the send
            ++conn.probes_out_;
            conn.probe_interval_ = std::min(conn.probe_interval_ * 2, config_.rto_max);
            arm_probe(conn);
        }

        /// The retry cap or the user timeout ran out: `TimedOut` for an owned connection, counted apart when the user
        /// timeout ended it.
        void time_out(ConnectionType& conn, const bool by_user_timeout) noexcept {
            if (!conn.owned_) {
                ++counters_.handshakes_failed;  // a passive open nobody took: freed silently
                free_slot(conn);
                return;
            }
            ++counters_.connections_timed_out;
            if (by_user_timeout) {
                ++counters_.connections_user_timed_out;
            }
            set_closed(conn);
            raise(conn, stream::Event::TimedOut);
        }

        /// The next hop never answered ARP: a failure of its own, told apart from a segment that left and was ignored.
        void fail_unresolved(ConnectionType& conn) noexcept {
            if (!conn.owned_) {
                ++counters_.handshakes_failed;  // a passive open nobody took: freed silently
                free_slot(conn);
                return;
            }
            ++counters_.connections_unresolved;
            set_closed(conn);
            raise(conn, stream::Event::Unresolved);
        }

        /// Closed, with nothing armed, pending or queued: after a terminal event nothing is resent, so the
        /// retransmission queue is dropped, its packets back now or when the device returns them. Held segments stay
        /// until release, and so does an open preparation: the application may still write into its span, and the
        /// next commit refuses it.
        void set_closed(ConnectionType& conn) noexcept {
            if (conn.state_ == State::SynSent) {
                conn.send_timestamps_ = false;  // the offer was never answered: nothing was negotiated
            }
            conn.state_ = State::Closed;
            disarm(conn);
            conn.unresolved_polls_ = 0;
            acks_.remove(conn);
            conn.ack_pending_    = false;
            conn.finish_pending_ = false;
            retries_.remove(conn);
            drop_queue(conn);  // and off the deferred list
        }

        void finish(ConnectionType& conn) noexcept {
            set_closed(conn);
            ++counters_.connections_closed;
            raise(conn, stream::Event::Closed);
        }

        /// The last ACK the peer is owed must be queued before Closed is published; flush retries the rest.
        void maybe_finish(ConnectionType& conn) noexcept {
            if (conn.finish_pending_ && !conn.ack_pending_) {
                finish(conn);
            }
        }

        /// FIN after what was committed: Established moves to FinWait1, CloseWait to LastAck; elsewhere nothing. An
        /// open preparation stays the application's: its next commit refuses it, as the connection left the sendable
        /// states. The FIN's entry is the node Established set aside, so the pool never refuses it. A FIN the peer's
        /// window does not cover, Linux's `tcp_snd_wnd_test`, waits in the queue, unsent, for the ACK that opens the
        /// window (`send_unsent`), while the probes go on; a Linux peer drops a FIN sent into its zero window.
        void close(ConnectionType& conn) noexcept {
            switch (conn.state_) {
                case State::Established:
                    conn.state_ = State::FinWait1;
                    break;
                case State::CloseWait:
                    conn.state_ = State::LastAck;
                    break;
                default:
                    return;
            }
            const Sequence fin = conn.snd_nxt_;
            const bool idle    = !in_flight(conn);  // else the RTO already runs for the data
            conn.fin_sent_     = true;
            conn.snd_nxt_      = fin + 1;  // the FIN takes its number whether or not it left: the timer resends it
            const std::optional<std::uint32_t> entry = enqueue_fin(conn, fin);
            if (!entry) {
                ++counters_.send_refused;  // unreachable by construction; a release build resets rather than hangs
                abort(conn);
                return;
            }
            if (!covered(conn, fin + 1)) {
                if (idle && !conn.probing()) {
                    start_probing(conn);  // a zero window with nothing in flight is probed already, as a rule
                }
                return;
            }
            std::ignore = transmit_entry(conn, *entry);
            if (idle && (!conn.timer_.armed() || conn.probing())) {
                arm_retransmit(conn);  // refused: the RTO tries again, and takes the timer from a probe
            }
        }

        void abort(ConnectionType& conn) noexcept {
            if (conn.state_ == State::Closed) {
                return;
            }
            // RFC 9293's ABORT in SYN-SENT: the peer holds nothing of ours to take back, so no RST leaves.
            if (conn.state_ != State::SynSent) {
                std::ignore = send_reset(conn);
            }
            set_closed(conn);
            raise(conn, stream::Event::Closed);
        }

        void release(ConnectionType& conn) noexcept {
            if (conn.state_ != State::Closed) {
                abort(conn);
            }
            free_slot(conn);
        }

        [[nodiscard]] static TcpConfig
        validated(const TcpConfig& config, const std::uint16_t max_l4_size, const core::Duration resolution) {
            if (config.connections == 0 || config.listeners == 0 || config.receive_segments == 0 ||
                config.receive_pool == 0) {
                throw std::invalid_argument{
                    "TcpConfig: connections, listeners, receive_segments and receive_pool must be positive"};
            }
            if (config.send_segments == 0 || config.send_pool == 0) {
                throw std::invalid_argument{"TcpConfig: send_segments and send_pool must be positive"};
            }
            if (config.connections > (std::size_t{1} << 30U)) {
                throw std::invalid_argument{"TcpConfig: too many connections for a 32-bit index"};
            }
            if (config.ephemeral_first == 0 || config.ephemeral_first > config.ephemeral_last) {
                throw std::invalid_argument{"TcpConfig: the ephemeral range must be non-empty and exclude port 0"};
            }
            if (config.rto_initial <= core::Duration::zero() || config.unresolved_retry <= core::Duration::zero()) {
                throw std::invalid_argument{"TcpConfig: rto_initial and unresolved_retry must be positive"};
            }
            if (config.rto_min <= core::Duration::zero() || config.rto_min < resolution) {
                throw std::invalid_argument{
                    "TcpConfig: rto_min must be positive and at least the timer wheel's resolution"};
            }
            if (config.rto_min > config.rto_initial || config.rto_initial > config.rto_max) {
                throw std::invalid_argument{"TcpConfig: rto_min <= rto_initial <= rto_max must hold"};
            }
            if (config.rto_max.count() > core::Duration::max().count() / 4) {
                throw std::invalid_argument{"TcpConfig: rto_max must fit a quarter of core::Duration"};
            }
            if (config.user_timeout < core::Duration::zero() || config.user_timeout > core::Duration::max() / 4) {
                throw std::invalid_argument{
                    "TcpConfig: user_timeout must not be negative and must fit a quarter of core::Duration"};
            }
            if (config.retries == 0 || config.data_retries == 0) {
                throw std::invalid_argument{"TcpConfig: retries and data_retries must be positive"};
            }
            if (config.data_retries >= 63) {
                throw std::invalid_argument{"TcpConfig: data_retries must be below 63"};
            }
            if (config.unresolved_polls == 0) {
                throw std::invalid_argument{"TcpConfig: unresolved_polls must be positive"};
            }
            // Bounds the longest handshake delay, rto_initial * 2^retries, and the whole schedule, rto_initial *
            // (2^(retries+1) - 1), to a quarter of core::Duration's range, so no deadline now + delay can overflow;
            // the doubling itself stops at rto_max.
            if (config.retries >= 63 ||
                config.rto_initial.count() > (core::Duration::max().count() / 4) >> config.retries) {
                throw std::invalid_argument{"TcpConfig: rto_initial doubled retries times must fit core::Duration"};
            }
            if (max_l4_size <= wire::TcpHeader::size) {
                throw std::invalid_argument{"TcpConfig: the MTU leaves no room for a TCP segment"};
            }
            return config;
        }

        /// RFC 7323's bound on TS.Recent: PAWS ignores a value older than 24 days, past which the peer's clock may
        /// have wrapped half its range at one tick per millisecond.
        static constexpr core::Duration paws_window = std::chrono::hours{24 * 24};

        Ip* ip_                    = nullptr;
        loop::TimerWheel* wheel_   = nullptr;
        TcpConfig config_          = {};
        std::uint16_t mss_         = 0;
        std::uint32_t byte_budget_ = 0;
        detail::TcpNodePool<Packet> pool_;
        detail::SentSegmentPool<Packet> send_pool_;
        FlowTable table_;
        std::deque<ConnectionType> connections_ = {};
        std::vector<std::uint16_t> listeners_   = {};
        std::vector<std::uint32_t> free_        = {};  ///< A stack of free slot indices.
        EventList events_                       = {};
        AckList acks_                           = {};
        RetryList retries_                      = {};
        DeferredList deferred_                  = {};  ///< Connections whose head resend waits for its packet.
        std::uint16_t cursor_                   = 0;
        std::mt19937 isn_engine_{std::random_device{}()};
        std::optional<Sequence> forced_isn_{};
        core::TimePoint now_{};
        TcpCounters counters_{};
        bool hardware_hash_ = false;  ///< The card hashes IPv4 TCP tuples: its hash is the table's base hash.
    };

}  // namespace aloe::tcp
