#pragma once

#include <algorithm>
#include <aloe/core>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
#include <datagram.hpp>
#include <flow_table.hpp>
#include <ipv4.hpp>
#include <ipv4_checksum.hpp>
#include <rss.hpp>
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
     * connections' non-blocking operations, `flush` for the ACKs that waited. The stack keeps the
     * stamp of the last `process`, `flush` or `connect` and uses it for everything a connection
     * does in between. Everything is allocated at construction; nothing here logs, blocks or
     * names a clock.
     */
    template <typename Ip>
    class Stack {
        static_assert(IsIp<Ip>, "tcp::Stack needs net::Ipv4<Device>, or an IP brick with the same members");

    public:
        using Device         = Ip::Device;
        using Packet         = Ip::Packet;
        using ConnectionType = Connection<Ip>;
        using EventType      = ConnectionEvent<Ip>;
        using Sequence       = wire::TcpSequence;

        /// Validates and throws std::invalid_argument; allocates everything; sends nothing.
        Stack(Ip& ip, loop::TimerWheel& wheel, const TcpConfig& config)
            : ip_{&ip},
              wheel_{&wheel},
              config_{validated(config, ip.max_l4_size())},
              mss_{static_cast<std::uint16_t>(ip.max_l4_size() - wire::TcpHeader::size)},
              byte_budget_{static_cast<std::uint32_t>(std::min<std::uint64_t>(
                  std::uint64_t{config_.receive_segments} * mss_, std::numeric_limits<std::uint16_t>::max()))},
              pool_{config_.receive_pool},
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
        /// Cancels every timer and returns every held packet; the IP brick and the wheel outlive it.
        ~Stack() {
            for (ConnectionType& c : connections_) {
                wheel_->cancel(c.timer_);
                c.prepared_.reset();
                return_chain(c);
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

        /// Sends the ordinary ACKs and window updates that waited, one pure ACK per connection.
        void flush(const core::TimePoint now) noexcept {
            now_ = now;
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
            c.indexed_      = true;
            c.state_        = State::SynSent;
            c.owned_        = true;
            c.iss_          = draw_isn();
            c.snd_nxt_      = c.iss_ + 1;
            c.snd_una_      = c.iss_;
            const auto sent = send_control(c, wire::TcpFlags{wire::TcpFlag::Syn}, c.iss_);
            if (sent) {
                ++counters_.control_segments_sent;
            }
            c.tries_           = 0;
            c.unresolved_wait_ = !sent && sent.error() == net::SendError::Unresolved;
            arm_retry(c);
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

    private:
        friend class Connection<Ip>;

        using Datagram  = net::Datagram<Packet>;
        using EventList = detail::ConnectionList<ConnectionType, &ConnectionType::event_link_>;
        using AckList   = detail::ConnectionList<ConnectionType, &ConnectionType::ack_link_>;
        using RetryList = detail::ConnectionList<ConnectionType, &ConnectionType::retry_link_>;

        /// What the receive path parsed from one datagram.
        struct Segment {
            wire::TcpHeader header{};
            std::uint16_t payload_offset = 0;  ///< From the start of `l4()`: the data offset.
            std::uint16_t payload_length = 0;
            std::optional<std::uint16_t> mss;  ///< From the options of a SYN.
            std::uint32_t base_hash = 0;

            /// Sequence space the segment occupies: payload, SYN and FIN.
            [[nodiscard]] std::uint32_t length() const noexcept {
                return payload_length + (header.flags.has(wire::TcpFlag::Syn) ? 1U : 0U) +
                       (header.flags.has(wire::TcpFlag::Fin) ? 1U : 0U);
            }
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
                            .base_hash      = 0};
            if (header->flags.has(wire::TcpFlag::Syn)) {
                const auto options = wire::TcpOptions::parse(std::span<const std::byte>{l4}.subspan(
                    wire::TcpHeader::size, header->data_offset - wire::TcpHeader::size));
                if (!options) {
                    ++counters_.dropped_bad_header;
                    return;
                }
                segment.mss = options->mss;
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
            switch (ConnectionType& c = connections_[*found]; c.state_) {
                case State::Closed:  // still indexed because not released
                    ++counters_.dropped_closed;
                    if (!header->flags.has(wire::TcpFlag::Rst)) {
                        reply_reset(datagram, segment);
                    }
                    return;
                case State::SynSent:
                    receive_syn_sent(c, datagram, segment);
                    return;
                default:
                    receive_synchronized(c, datagram, segment);
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
            const std::optional<std::uint32_t> slot = take_slot();
            if (!slot) {
                ++counters_.dropped_table_full;
                return;
            }
            ConnectionType& c = connections_[*slot];
            c.local_          = Endpoint{.address = ip_->address(), .port = segment.header.destination_port};
            c.remote_         = Endpoint{.address = datagram.source, .port = segment.header.source_port};
            c.key_            = key;
            c.base_hash_      = segment.base_hash;
            [[maybe_unused]] const bool inserted = table_.insert(key, *slot, segment.base_hash);
            assert(inserted && "the lookup just missed");
            c.indexed_      = true;
            c.state_        = State::SynReceived;
            c.owned_        = false;
            c.rcv_nxt_      = segment.header.sequence + 1;
            c.rcv_adv_      = c.rcv_nxt_ + byte_budget_;  // the initial offer, carried by the SYN-ACK
            c.peer_mss_     = segment.mss.value_or(default_peer_mss);
            c.snd_wnd_      = segment.header.window;
            c.wl1_          = segment.header.sequence;
            c.wl2_          = Sequence{};
            c.iss_          = draw_isn();
            c.snd_nxt_      = c.iss_ + 1;
            c.snd_una_      = c.iss_;
            const auto sent = send_control(c, wire::TcpFlags{wire::TcpFlag::Syn, wire::TcpFlag::Ack}, c.iss_);
            if (sent) {
                ++counters_.control_segments_sent;
            }
            c.tries_           = 0;
            c.unresolved_wait_ = !sent && sent.error() == net::SendError::Unresolved;
            arm_retry(c);
        }

        void receive_syn_sent(ConnectionType& c, Datagram& datagram, Segment segment) noexcept {
            const wire::TcpHeader& header = segment.header;
            if (header.flags.has(wire::TcpFlag::Ack) && header.acknowledgement != c.snd_nxt_) {
                ++counters_.dropped_unexpected;  // not an answer to our SYN
                return;
            }
            if (header.flags.has(wire::TcpFlag::Rst)) {
                if (!header.flags.has(wire::TcpFlag::Ack)) {
                    ++counters_.dropped_unexpected;
                    return;
                }
                ++counters_.connections_reset;
                set_closed(c);
                raise(c, stream::Event::Reset);
                return;
            }
            if (!header.flags.has(wire::TcpFlag::Syn) || !header.flags.has(wire::TcpFlag::Ack)) {
                ++counters_.dropped_unexpected;  // a simultaneous open is out of scope
                return;
            }
            c.snd_una_  = header.acknowledgement;
            c.rcv_nxt_  = header.sequence + 1;
            c.rcv_adv_  = c.rcv_nxt_ + byte_budget_;  // the initial offer, carried by our ACK or the first data segment
            c.peer_mss_ = segment.mss.value_or(default_peer_mss);
            c.snd_wnd_  = header.window;
            c.wl1_      = header.sequence;
            c.wl2_      = header.acknowledgement;
            c.state_    = State::Established;
            wheel_->cancel(c.timer_);
            c.tries_           = 0;
            c.unresolved_wait_ = false;
            ++counters_.connections_opened;
            queue_ack(c);  // deferred: a commit on the Connected event completes the handshake with data
            raise(c, stream::Event::Connected);
            if (segment.payload_length > 0 || header.flags.has(wire::TcpFlag::Fin)) {
                segment.header.sequence = c.rcv_nxt_;
                segment.header.flags    = wire::TcpFlags::from_raw(
                    static_cast<std::uint8_t>(header.flags.raw() & ~std::to_underlying(wire::TcpFlag::Syn)));
                receive_synchronized(c, datagram, segment);
            }
        }

        void receive_synchronized(ConnectionType& c, Datagram& datagram, Segment segment) noexcept {
            wire::TcpHeader& header = segment.header;
            const bool rst          = header.flags.has(wire::TcpFlag::Rst);

            // 1. Sequence check against [rcv_nxt, rcv_adv): trim an old prefix, drop the rest with an immediate ACK.
            const std::uint32_t window = c.rcv_adv_.value - c.rcv_nxt_.value;
            const std::uint32_t length = segment.length();
            Sequence sequence          = header.sequence;
            const Sequence end         = sequence + length;
            if (length == 0) {
                const bool acceptable =
                    window == 0 ? sequence == c.rcv_nxt_ : !sequence.before(c.rcv_nxt_) && sequence.before(c.rcv_adv_);
                if (!acceptable) {
                    ++counters_.dropped_out_of_window;
                    if (!rst) {
                        ack_now(c);
                    }
                    return;
                }
            } else {
                if (!end.after(c.rcv_nxt_)) {  // entirely before rcv_nxt
                    if (c.state_ == State::SynReceived && header.flags.has(wire::TcpFlag::Syn) &&
                        !header.flags.has(wire::TcpFlag::Ack)) {
                        if (send_control(c, wire::TcpFlags{wire::TcpFlag::Syn, wire::TcpFlag::Ack}, c.iss_)) {
                            ++counters_.control_segments_sent;  // a duplicate SYN: the SYN-ACK again
                        }
                        return;
                    }
                    ++counters_.dropped_duplicate;
                    if (!rst) {
                        ack_now(c);
                    }
                    return;
                }
                if (sequence.after(c.rcv_nxt_)) {  // a gap before it
                    ++counters_.dropped_out_of_order;
                    if (!c.recovery_end_ || end.after(*c.recovery_end_)) {
                        c.recovery_end_ = end;
                    }
                    if (!rst) {
                        ack_now(c);
                    }
                    return;
                }
                if (sequence.before(c.rcv_nxt_)) {  // overlap: the old prefix is already ours
                    std::uint32_t trim = c.rcv_nxt_.value - sequence.value;
                    if (header.flags.has(wire::TcpFlag::Syn)) {
                        header.flags = wire::TcpFlags::from_raw(
                            static_cast<std::uint8_t>(header.flags.raw() & ~std::to_underlying(wire::TcpFlag::Syn)));
                        --trim;
                    }
                    assert(trim <= segment.payload_length);
                    segment.payload_offset = static_cast<std::uint16_t>(segment.payload_offset + trim);
                    segment.payload_length = static_cast<std::uint16_t>(segment.payload_length - trim);
                    sequence               = c.rcv_nxt_;
                    header.sequence        = sequence;
                }
                if (window == 0 || end.after(c.rcv_adv_)) {
                    ++counters_.dropped_out_of_window;
                    if (!rst) {
                        ack_now(c);
                    }
                    return;
                }
            }

            // 2. RST.
            if (rst) {
                if (c.state_ == State::SynReceived && !c.owned_) {
                    ++counters_.handshakes_failed;
                    free_slot(c);
                    return;
                }
                ++counters_.connections_reset;
                set_closed(c);
                raise(c, stream::Event::Reset);
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
            if (c.state_ == State::SynReceived) {
                if (ack != c.snd_nxt_) {
                    ++counters_.dropped_unexpected;
                    return;
                }
                c.snd_una_ = ack;
                c.state_   = State::Established;
                c.owned_   = true;
                wheel_->cancel(c.timer_);
                c.tries_           = 0;
                c.unresolved_wait_ = false;
                ++counters_.connections_accepted;
                raise(c, stream::Event::Accepted);
                // The same segment may carry data or a FIN: keep going.
            }
            if (ack.after(c.snd_nxt_)) {
                ++counters_.dropped_unexpected;  // acknowledges what was never sent
                ack_now(c);
                return;
            }
            const std::size_t writable_before = writable(c);
            if (ack.after(c.snd_una_)) {
                c.snd_una_ = ack;
                raise(c, stream::Event::Acked);
                if (c.fin_sent_ && ack == c.snd_nxt_) {  // our FIN is acknowledged
                    switch (c.state_) {
                        case State::FinWait1:
                            c.state_ = State::FinWait2;
                            wheel_->cancel(c.timer_);
                            break;
                        case State::Closing:
                        case State::LastAck:
                            wheel_->cancel(c.timer_);
                            c.finish_pending_ = true;
                            break;
                        default:
                            break;
                    }
                }
            }
            update_peer_window(c, header);
            if (writable(c) > writable_before) {
                raise(c, stream::Event::Writable);
            }
            maybe_finish(c);
            if (c.state_ == State::Closed) {
                return;
            }

            // 4. Payload.
            if (segment.payload_length > 0) {
                const bool receiving =
                    c.state_ == State::Established || c.state_ == State::FinWait1 || c.state_ == State::FinWait2;
                if (!receiving) {
                    ++counters_.dropped_unexpected;  // data after the peer's FIN
                    ack_now(c);
                    return;
                }
                if (!accept_payload(c, datagram, segment)) {
                    ack_now(c);  // reports the last accepted byte; the FIN behind refused data is not consumed
                    return;
                }
                apply_ack_policy(c, segment.payload_length);
            }

            // 5. FIN, in order: its sequence is rcv_nxt after the payload.
            if (header.flags.has(wire::TcpFlag::Fin)) {
                if (c.state_ == State::CloseWait || c.state_ == State::Closing || c.state_ == State::LastAck) {
                    ++counters_.dropped_unexpected;  // a second FIN: the peer's sequence space ended at the first
                    ack_now(c);
                    return;
                }
                receive_fin(c);
            }
        }

        [[nodiscard]] bool accept_payload(ConnectionType& c, Datagram& datagram, const Segment& segment) noexcept {
            if (c.chain_count_ >= config_.receive_segments) {
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
            if (c.chain_tail_ == detail::no_node) {
                c.chain_head_ = *node;
            } else {
                pool_.node(c.chain_tail_).next = *node;
            }
            c.chain_tail_ = *node;
            ++c.chain_count_;
            c.rcv_nxt_       = c.rcv_nxt_ + segment.payload_length;
            c.unread_bytes_ += segment.payload_length;
            raise(c, stream::Event::Readable);
            return true;
        }

        /// RFC 5681: ordinary in-order data waits for flush; recovery, and every second full-sized segment, do not.
        void apply_ack_policy(ConnectionType& c, const std::size_t accepted) noexcept {
            if (c.recovery_end_) {
                ack_now(c);
                if (!c.rcv_nxt_.before(*c.recovery_end_)) {
                    c.recovery_end_.reset();
                }
                return;
            }
            if (accepted >= mss_) {
                if (c.full_segments_since_ack_ < 2) {
                    ++c.full_segments_since_ack_;
                }
                if (c.full_segments_since_ack_ >= 2) {
                    ack_now(c);  // a success resets the count; a refusal leaves it saturated at two
                    return;
                }
            }
            queue_ack(c);
        }

        void receive_fin(ConnectionType& c) noexcept {
            c.rcv_nxt_     = c.rcv_nxt_ + 1;
            c.peer_closed_ = true;
            raise(c, stream::Event::PeerClosed);
            switch (c.state_) {
                case State::Established:
                    c.state_ = State::CloseWait;
                    queue_ack(c);  // the application's close may carry it
                    break;
                case State::FinWait1:
                    c.state_ = State::Closing;
                    ack_now(c);
                    break;
                case State::FinWait2:
                    c.finish_pending_ = true;
                    ack_now(c);  // the terminal ACK goes before Closed is published; flush retries a refusal
                    maybe_finish(c);
                    break;
                default:
                    queue_ack(c);
                    break;
            }
        }

        /// RFC 793's window update rule: a newer segment, or the same one with a newer acknowledgement.
        static void update_peer_window(ConnectionType& c, const wire::TcpHeader& header) noexcept {
            if (c.wl1_.before(header.sequence) ||
                (c.wl1_ == header.sequence && !c.wl2_.after(header.acknowledgement))) {
                c.snd_wnd_ = header.window;
                c.wl1_     = header.sequence;
                c.wl2_     = header.acknowledgement;
            }
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

        // The transmit path: every segment leaves through send_segment; pure ACKs and their policy.

        /**
         * Prepends the header (and the MSS option on a SYN) to `packet`, which holds the payload, and hands
         * it to IP. A segment with ACK offers the receive window: the candidate edge `rcv_nxt + (B - unread)`
         * when it is later than the current edge and a packet slot is free, else the current edge. Only a
         * successful send records the offer and clears the pending ACK; a refusal returns the packet to the
         * pool and leaves every number as it was.
         */
        [[nodiscard]] std::expected<void, net::SendError> send_segment(ConnectionType& c,
                                                                       Packet&& packet,
                                                                       const Sequence sequence,
                                                                       const wire::TcpFlags flags,
                                                                       const bool with_mss) noexcept {
            const std::size_t header_size = wire::TcpHeader::size + (with_mss ? wire::TcpOptions::mss_size : 0);
            const std::optional<std::span<std::byte>> room = packet.prepend(header_size);
            if (!room) {
                ++counters_.send_refused;  // not a packet from allocate(): no headroom
                packet = Packet{};
                return std::unexpected{net::SendError::Refused};
            }
            const bool with_ack = flags.has(wire::TcpFlag::Ack);
            Sequence offer      = c.rcv_adv_;
            bool extends        = false;
            if (with_ack) {
                if (const Sequence candidate =
                        c.rcv_nxt_ + (byte_budget_ - static_cast<std::uint32_t>(c.unread_bytes_));
                    candidate.after(c.rcv_adv_) && c.chain_count_ < config_.receive_segments) {
                    offer   = candidate;
                    extends = true;
                }
            }
            const std::uint16_t window = with_ack ? static_cast<std::uint16_t>(offer.value - c.rcv_nxt_.value)
                                                  : static_cast<std::uint16_t>(byte_budget_);
            wire::TcpHeader{.source_port      = c.local_.port,
                            .destination_port = c.remote_.port,
                            .sequence         = sequence,
                            .acknowledgement  = with_ack ? c.rcv_nxt_ : Sequence{},
                            .data_offset      = static_cast<std::uint8_t>(header_size),
                            .flags            = flags,
                            .window           = window}
                .write(*room);
            if (with_mss) {
                wire::TcpOptions::write_mss(room->subspan(wire::TcpHeader::size), mss_);
            }
            const auto sent = ip_->send(std::move(packet),
                                        net::SendRequest{.destination = c.remote_.address,
                                                         .protocol    = wire::Ipv4Protocol::Tcp,
                                                         .checksum    = device::L4Checksum::Tcp},
                                        now_);
            if (!sent) {
                packet =
                    Packet{};  // NOLINT(bugprone-use-after-move): IP moves only on success; the packet is ours to drop
                if (sent.error() == net::SendError::Unresolved) {
                    ++counters_.send_unresolved;
                } else {
                    ++counters_.send_refused;
                }
                return sent;
            }
            if (with_ack) {
                c.rcv_adv_ = offer;
                if (extends) {
                    ++counters_.window_updates;
                }
                c.ack_pending_ = false;
                acks_.remove(c);
                c.full_segments_since_ack_ = 0;
            }
            return {};
        }

        [[nodiscard]] std::expected<void, net::SendError>
        send_control(ConnectionType& c, const wire::TcpFlags flags, const Sequence sequence) noexcept {
            std::optional<Packet> packet = ip_->allocate();
            if (!packet) {
                ++counters_.send_refused;
                return std::unexpected{net::SendError::Refused};
            }
            return send_segment(c, std::move(*packet), sequence, flags, flags.has(wire::TcpFlag::Syn));
        }

        [[nodiscard]] bool send_pure_ack(ConnectionType& c) noexcept {
            if (send_control(c, wire::TcpFlags{wire::TcpFlag::Ack}, c.snd_nxt_)) {
                ++counters_.pure_acks_sent;
                return true;
            }
            return false;
        }

        /// An ordinary ACK: marked, sent by flush unless a data segment carries it first.
        void queue_ack(ConnectionType& c) noexcept {
            c.ack_pending_ = true;
            if (!acks_.contains(c)) {
                acks_.push_back(c);
            }
        }

        /// An immediate ACK: queued through IP before the next segment is processed; a refusal leaves it pending.
        void ack_now(ConnectionType& c) noexcept {
            if (!send_pure_ack(c)) {
                queue_ack(c);
            }
        }

        /// A Writable hint at the next process, at most one per marked connection.
        void mark_retry(ConnectionType& c) noexcept {
            if (!retries_.contains(c)) {
                retries_.push_back(c);
            }
        }

        [[nodiscard]] static std::size_t writable(const ConnectionType& c) noexcept {
            if (c.prepared_ || (c.state_ != State::Established && c.state_ != State::CloseWait)) {
                return 0;
            }
            const std::uint32_t in_flight = c.snd_nxt_.value - c.snd_una_.value;
            const std::uint32_t usable = c.snd_wnd_ > in_flight ? c.snd_wnd_ - in_flight : 0;  // no unsigned underflow
            return std::min<std::size_t>(c.mss(), usable);
        }

        /**
         * A span of `min(count, writable())` bytes inside a fresh packet. The packet belongs to the application until
         * `commit` (with any count) or `release`: the span stays valid through any state change in between. Nothing,
         * without allocating, for a count of zero or nothing writable; an allocation failure counts one
         * `allocation_failures`, marks a retry hint and retains nothing.
         */
        [[nodiscard]] std::optional<std::span<std::byte>> prepare(ConnectionType& c, const std::size_t count) noexcept {
            assert(!c.prepared_ && "one prepare at a time: commit first");
            if (count == 0) {
                return std::nullopt;
            }
            const std::size_t room = writable(c);
            if (room == 0) {
                return std::nullopt;  // nothing allocates when nothing is writable
            }
            const std::size_t size       = std::min(count, room);
            std::optional<Packet> packet = ip_->allocate();
            if (!packet) {
                ++counters_.allocation_failures;
                mark_retry(c);  // the pool may have packets by the next process
                return std::nullopt;
            }
            const std::optional<std::span<std::byte>> span = packet->append(size);
            assert(span && "size is at most the MSS and a packet holds a whole frame");
            if (!span) {
                ++counters_.allocation_failures;  // unreachable by construction; counted in a release build
                mark_retry(c);
                return std::nullopt;
            }
            c.prepared_      = std::move(*packet);  // the span points into the block, which does not move
            c.prepared_size_ = size;
            return span;
        }

        /**
         * One PSH+ACK segment of the first `count` prepared bytes; `commit(0)` discards the preparation. A refusal
         * returns the packet to the pool, leaves every number as it was, marks a retry hint and returns false. A
         * connection that left Established and CloseWait since the prepare drops the packet and refuses the commit
         * with no hint: the events say why.
         */
        [[nodiscard]] bool commit(ConnectionType& c, const std::size_t count) noexcept {
            assert(c.prepared_ && "commit without a prepare");
            if (!c.prepared_) {
                ++counters_.commits_refused;  // a contract violation, refused in a release build
                return false;
            }
            assert(count <= c.prepared_size_ && "commit more than was prepared");
            Packet packet = std::move(*c.prepared_);
            c.prepared_.reset();
            const std::size_t prepared = c.prepared_size_;
            c.prepared_size_           = 0;
            if (count == 0) {
                return true;  // discarded: `packet` goes back to the pool; nothing was refused
            }
            if (c.state_ != State::Established && c.state_ != State::CloseWait) {
                ++counters_.commits_refused;  // closed since the prepare: no hint, the events say why
                return false;
            }
            packet.trim_back(prepared - count);
            if (!send_segment(
                    c, std::move(packet), c.snd_nxt_, wire::TcpFlags{wire::TcpFlag::Ack, wire::TcpFlag::Psh}, false)) {
                mark_retry(c);  // send_segment counted the refusal
                return false;
            }
            c.snd_nxt_ = c.snd_nxt_ + static_cast<std::uint32_t>(count);
            ++counters_.data_segments_sent;
            return true;
        }

        /// Advances the chain's front by `count` bytes, freeing packets as they empty; a reopened edge queues an
        /// update.
        void consume(ConnectionType& c, const std::size_t count) noexcept {
            assert(count <= c.unread_bytes_ && "consume past unread()");
            std::size_t remaining = count;
            while (remaining > 0) {
                typename detail::TcpNodePool<Packet>::Node& node  = pool_.node(c.chain_head_);
                const std::size_t taken                           = std::min<std::size_t>(remaining, node.length);
                node.offset                                       = static_cast<std::uint16_t>(node.offset + taken);
                node.length                                       = static_cast<std::uint16_t>(node.length - taken);
                remaining                                        -= taken;
                c.unread_bytes_                                  -= taken;
                if (node.length == 0) {
                    const std::uint32_t next = node.next;
                    pool_.release(c.chain_head_);
                    c.chain_head_ = next;
                    if (next == detail::no_node) {
                        c.chain_tail_ = detail::no_node;
                    }
                    --c.chain_count_;
                }
            }
            const bool receiving =
                c.state_ == State::Established || c.state_ == State::FinWait1 || c.state_ == State::FinWait2;
            if (!receiving) {
                return;  // the peer sends no more data, or the connection is closed: no offer to make
            }
            if (const Sequence candidate = c.rcv_nxt_ + (byte_budget_ - static_cast<std::uint32_t>(c.unread_bytes_));
                candidate.after(c.rcv_adv_) && c.chain_count_ < config_.receive_segments) {
                queue_ack(c);  // an ordinary window update, including the reopening of a zero window
            }
        }

        // The control path: events, slots, the port choice, the retransmit timer, close, abort and release.

        void raise(ConnectionType& c, const stream::Event event) noexcept {
            c.events_ |= event;
            if (!events_.contains(c)) {
                events_.push_back(c);
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

        void free_slot(ConnectionType& c) noexcept {
            // Every live connection is indexed by connect or passive_open; a free slot is not, and pushing its index
            // again would hand one slot to two opens. A release build ignores the second call.
            assert(c.indexed_ && "release of a free slot");
            if (!c.indexed_) {
                return;
            }
            wheel_->cancel(c.timer_);
            std::ignore = table_.erase(c.key_, c.base_hash_);
            return_chain(c);
            c.prepared_.reset();
            events_.remove(c);
            acks_.remove(c);
            retries_.remove(c);
            c.clear();
            free_.push_back(c.index_);
        }

        void return_chain(ConnectionType& c) noexcept {
            for (std::uint32_t index = c.chain_head_; index != detail::no_node;) {
                const std::uint32_t next = pool_.node(index).next;
                pool_.release(index);
                index = next;
            }
            c.chain_head_   = detail::no_node;
            c.chain_tail_   = detail::no_node;
            c.chain_count_  = 0;
            c.unread_bytes_ = 0;
        }

        [[nodiscard]] Sequence draw_isn() noexcept {
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

        void arm_retry(ConnectionType& c) noexcept {
            const core::Duration delay =
                c.unresolved_wait_ ? config_.unresolved_retry : config_.retry_initial * (std::int64_t{1} << c.tries_);
            wheel_->arm(c.timer_, now_ + delay);
        }

        /// Called by the wheel with the timer unarmed: a try, or a poll while the next hop is unresolved.
        void on_timer(ConnectionType& c) noexcept {
            switch (c.state_) {
                case State::SynSent:
                case State::SynReceived:
                case State::FinWait1:
                case State::Closing:
                case State::LastAck:
                    break;
                default:
                    return;  // nothing to retransmit in this state
            }
            if (!c.unresolved_wait_) {
                if (c.tries_ >= config_.retries) {
                    time_out(c);
                    return;
                }
                ++c.tries_;
                ++counters_.retransmits;
            }
            const auto sent = resend_control(c);
            if (sent) {
                ++counters_.control_segments_sent;
            }
            c.unresolved_wait_ = !sent && sent.error() == net::SendError::Unresolved;
            arm_retry(c);
        }

        [[nodiscard]] std::expected<void, net::SendError> resend_control(ConnectionType& c) noexcept {
            switch (c.state_) {
                case State::SynSent:
                    return send_control(c, wire::TcpFlags{wire::TcpFlag::Syn}, c.iss_);
                case State::SynReceived:
                    return send_control(c, wire::TcpFlags{wire::TcpFlag::Syn, wire::TcpFlag::Ack}, c.iss_);
                case State::FinWait1:
                case State::Closing:
                case State::LastAck:
                    return send_control(c, wire::TcpFlags{wire::TcpFlag::Fin, wire::TcpFlag::Ack}, c.fin_sequence_);
                default:
                    return {};
            }
        }

        void time_out(ConnectionType& c) noexcept {
            if (!c.owned_) {
                ++counters_.handshakes_failed;  // a passive open nobody took: freed silently
                free_slot(c);
                return;
            }
            ++counters_.connections_timed_out;
            set_closed(c);
            raise(c, stream::Event::TimedOut);
        }

        /// Closed, with nothing armed or pending. Held segments stay until release, and so does an open preparation:
        /// the application may still write into its span, and the next commit refuses it.
        void set_closed(ConnectionType& c) noexcept {
            c.state_ = State::Closed;
            wheel_->cancel(c.timer_);
            c.unresolved_wait_ = false;
            acks_.remove(c);
            c.ack_pending_    = false;
            c.finish_pending_ = false;
            retries_.remove(c);
        }

        void finish(ConnectionType& c) noexcept {
            set_closed(c);
            ++counters_.connections_closed;
            raise(c, stream::Event::Closed);
        }

        /// The last ACK the peer is owed must be queued before Closed is published; flush retries the rest.
        void maybe_finish(ConnectionType& c) noexcept {
            if (c.finish_pending_ && !c.ack_pending_) {
                finish(c);
            }
        }

        /// FIN after what was committed: Established moves to FinWait1, CloseWait to LastAck; elsewhere nothing. An
        /// open preparation stays the application's: its next commit refuses it, as the connection left the sendable
        /// states.
        void close(ConnectionType& c) noexcept {
            switch (c.state_) {
                case State::Established:
                    c.state_ = State::FinWait1;
                    break;
                case State::CloseWait:
                    c.state_ = State::LastAck;
                    break;
                default:
                    return;
            }
            c.fin_sequence_ = c.snd_nxt_;
            c.fin_sent_     = true;
            const auto sent = send_control(c, wire::TcpFlags{wire::TcpFlag::Fin, wire::TcpFlag::Ack}, c.fin_sequence_);
            if (sent) {
                ++counters_.control_segments_sent;
            }
            c.snd_nxt_ = c.snd_nxt_ + 1;  // the FIN takes its number whether or not it left: the timer resends it
            c.tries_   = 0;
            c.unresolved_wait_ = !sent && sent.error() == net::SendError::Unresolved;
            arm_retry(c);
        }

        void abort(ConnectionType& c) noexcept {
            if (c.state_ == State::Closed) {
                return;
            }
            if (send_control(c, wire::TcpFlags{wire::TcpFlag::Rst, wire::TcpFlag::Ack}, c.snd_nxt_)) {
                ++counters_.resets_sent;
            }
            set_closed(c);
            raise(c, stream::Event::Closed);
        }

        void release(ConnectionType& c) noexcept {
            if (c.state_ != State::Closed) {
                abort(c);
            }
            free_slot(c);
        }

        [[nodiscard]] static TcpConfig validated(const TcpConfig& config, const std::uint16_t max_l4_size) {
            if (config.connections == 0 || config.listeners == 0 || config.receive_segments == 0 ||
                config.receive_pool == 0) {
                throw std::invalid_argument{
                    "TcpConfig: connections, listeners, receive_segments and receive_pool must be positive"};
            }
            if (config.connections > (std::size_t{1} << 30U)) {
                throw std::invalid_argument{"TcpConfig: too many connections for a 32-bit index"};
            }
            if (config.ephemeral_first == 0 || config.ephemeral_first > config.ephemeral_last) {
                throw std::invalid_argument{"TcpConfig: the ephemeral range must be non-empty and exclude port 0"};
            }
            if (config.retry_initial <= core::Duration::zero() || config.unresolved_retry <= core::Duration::zero()) {
                throw std::invalid_argument{"TcpConfig: retry_initial and unresolved_retry must be positive"};
            }
            if (config.retries == 0) {
                throw std::invalid_argument{"TcpConfig: retries must be positive"};
            }
            // Bounds the longest delay, retry_initial * 2^retries, and the whole schedule, retry_initial *
            // (2^(retries+1) - 1), to a quarter of core::Duration's range, so neither arm_retry's shift nor now + delay
            // can overflow.
            if (config.retries >= 63 ||
                config.retry_initial.count() > (core::Duration::max().count() / 4) >> config.retries) {
                throw std::invalid_argument{"TcpConfig: retry_initial doubled retries times must fit core::Duration"};
            }
            if (max_l4_size <= wire::TcpHeader::size) {
                throw std::invalid_argument{"TcpConfig: the MTU leaves no room for a TCP segment"};
            }
            return config;
        }

        Ip* ip_                    = nullptr;
        loop::TimerWheel* wheel_   = nullptr;
        TcpConfig config_          = {};
        std::uint16_t mss_         = 0;
        std::uint32_t byte_budget_ = 0;
        detail::TcpNodePool<Packet> pool_;
        FlowTable table_;
        std::deque<ConnectionType> connections_ = {};
        std::vector<std::uint16_t> listeners_   = {};
        std::vector<std::uint32_t> free_        = {};  ///< A stack of free slot indices.
        EventList events_                       = {};
        AckList acks_                           = {};
        RetryList retries_                      = {};
        std::uint16_t cursor_                   = 0;
        std::mt19937 isn_engine_{std::random_device{}()};
        core::TimePoint now_{};
        TcpCounters counters_{};
        bool hardware_hash_ = false;  ///< The card hashes IPv4 TCP tuples: its hash is the table's base hash.
    };

}  // namespace aloe::tcp
