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

#include <datagram.hpp>
#include <flow_table.hpp>
#include <ipv4.hpp>
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
        using Device         = typename Ip::Device;
        using Packet         = typename Ip::Packet;
        using ConnectionType = Connection<Ip>;
        using EventType      = ConnectionEvent<Ip>;
        using Sequence       = wire::TcpSequence;

        /// Validates and throws std::invalid_argument; allocates everything; sends nothing.
        Stack(Ip& ip, loop::TimerWheel& wheel, const TcpConfig& config);
        Stack(const Stack&)            = delete;
        Stack& operator=(const Stack&) = delete;
        Stack(Stack&&)                 = delete;
        Stack& operator=(Stack&&)      = delete;
        /// Cancels every timer and returns every held packet; the IP brick and the wheel outlive it.
        ~Stack();

        /// Records the stamp, publishes deferred retry hints, then processes each datagram. Every packet is moved
        /// out or released. An empty span is valid.
        void process(std::span<net::Datagram<Packet>> segments, core::TimePoint now) noexcept;
        /// Removes one pending notification and returns its snapshot; nothing when none is pending.
        [[nodiscard]] std::optional<EventType> poll_event() noexcept;
        /// Sends the ordinary ACKs and window updates that waited, one pure ACK per connection.
        void flush(core::TimePoint now) noexcept;

        [[nodiscard]] std::expected<void, ListenError> listen(std::uint16_t port) noexcept;
        /// Connections already open stay open.
        void unlisten(std::uint16_t port) noexcept;
        /// A SynSent connection the caller owns; its SYN is queued, or waits for ARP on the timer.
        [[nodiscard]] std::expected<ConnectionType*, ConnectError> connect(Endpoint peer, core::TimePoint now) noexcept;

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
        [[nodiscard]] bool listening(std::uint16_t port) const noexcept;
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

        // tcp_receive.hpp
        void on_datagram(Datagram& datagram) noexcept;
        [[nodiscard]] bool checksum_ok(Datagram& datagram) const noexcept;
        [[nodiscard]] std::uint32_t
        base_hash_of(const Datagram& datagram, const Segment& segment, FlowKey key) const noexcept;
        void passive_open(Datagram& datagram, const Segment& segment, FlowKey key) noexcept;
        void receive_syn_sent(ConnectionType& c, Datagram& datagram, Segment segment) noexcept;
        void receive_synchronized(ConnectionType& c, Datagram& datagram, Segment segment) noexcept;
        [[nodiscard]] bool accept_payload(ConnectionType& c, Datagram& datagram, const Segment& segment) noexcept;
        void apply_ack_policy(ConnectionType& c, std::size_t accepted) noexcept;
        void receive_fin(ConnectionType& c) noexcept;
        void update_peer_window(ConnectionType& c, const wire::TcpHeader& header) noexcept;
        void reply_reset(const Datagram& datagram, const Segment& segment) noexcept;

        // tcp_transmit.hpp
        [[nodiscard]] std::expected<void, net::SendError> send_segment(
            ConnectionType& c, Packet&& packet, Sequence sequence, wire::TcpFlags flags, bool with_mss) noexcept;
        [[nodiscard]] std::expected<void, net::SendError>
        send_control(ConnectionType& c, wire::TcpFlags flags, Sequence sequence) noexcept;
        [[nodiscard]] bool send_pure_ack(ConnectionType& c) noexcept;
        void queue_ack(ConnectionType& c) noexcept;
        void ack_now(ConnectionType& c) noexcept;
        void mark_retry(ConnectionType& c) noexcept;
        [[nodiscard]] std::size_t writable(const ConnectionType& c) const noexcept;
        [[nodiscard]] std::optional<std::span<std::byte>> prepare(ConnectionType& c,
                                                                  std::size_t count) noexcept;  // Task 8
        [[nodiscard]] bool commit(ConnectionType& c, std::size_t count) noexcept;               // Task 8
        void consume(ConnectionType& c, std::size_t count) noexcept;                            // Task 7

        // tcp_control.hpp
        void raise(ConnectionType& c, stream::Event event) noexcept;
        [[nodiscard]] std::optional<std::uint32_t> take_slot() noexcept;
        void free_slot(ConnectionType& c) noexcept;
        void return_chain(ConnectionType& c) noexcept;
        [[nodiscard]] Sequence draw_isn() noexcept;
        [[nodiscard]] std::expected<std::uint16_t, ConnectError> choose_port(Endpoint peer,
                                                                             std::uint32_t& base_hash) noexcept;
        void arm_retry(ConnectionType& c) noexcept;
        void on_timer(ConnectionType& c) noexcept;
        [[nodiscard]] std::expected<void, net::SendError> resend_control(ConnectionType& c) noexcept;
        void time_out(ConnectionType& c) noexcept;
        void set_closed(ConnectionType& c) noexcept;
        void finish(ConnectionType& c) noexcept;
        void maybe_finish(ConnectionType& c) noexcept;
        void close(ConnectionType& c) noexcept;  // Task 9
        void abort(ConnectionType& c) noexcept;
        void release(ConnectionType& c) noexcept;

        [[nodiscard]] static TcpConfig validated(const TcpConfig& config, std::uint16_t max_l4_size);

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

    template <typename Ip>
    TcpConfig Stack<Ip>::validated(const TcpConfig& config, const std::uint16_t max_l4_size) {
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
        // Bounds the longest delay, retry_initial * 2^retries, and the whole schedule, retry_initial * (2^(retries+1) -
        // 1), to a quarter of core::Duration's range, so neither arm_retry's shift nor now + delay can overflow.
        if (config.retries >= 63 ||
            config.retry_initial.count() > (core::Duration::max().count() / 4) >> config.retries) {
            throw std::invalid_argument{"TcpConfig: retry_initial doubled retries times must fit core::Duration"};
        }
        if (max_l4_size <= wire::TcpHeader::size) {
            throw std::invalid_argument{"TcpConfig: the MTU leaves no room for a TCP segment"};
        }
        return config;
    }

    template <typename Ip>
    Stack<Ip>::Stack(Ip& ip, loop::TimerWheel& wheel, const TcpConfig& config)
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

    template <typename Ip>
    Stack<Ip>::~Stack() {
        for (ConnectionType& c : connections_) {
            wheel_->cancel(c.timer_);
            c.prepared_.reset();
            return_chain(c);
        }
    }

    template <typename Ip>
    bool Stack<Ip>::listening(const std::uint16_t port) const noexcept {
        return std::ranges::find(listeners_, port) != listeners_.end();
    }

    template <typename Ip>
    std::expected<void, ListenError> Stack<Ip>::listen(const std::uint16_t port) noexcept {
        if (listening(port)) {
            return std::unexpected{ListenError::InUse};
        }
        if (listeners_.size() == config_.listeners) {
            return std::unexpected{ListenError::TableFull};
        }
        listeners_.push_back(port);  // within the reserved capacity: no allocation
        return {};
    }

    template <typename Ip>
    void Stack<Ip>::unlisten(const std::uint16_t port) noexcept {
        const auto found = std::ranges::find(listeners_, port);
        if (found != listeners_.end()) {
            *found = listeners_.back();
            listeners_.pop_back();
        }
    }

    template <typename Ip>
    void Stack<Ip>::process(const std::span<net::Datagram<Packet>> segments, const core::TimePoint now) noexcept {
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

    template <typename Ip>
    std::optional<ConnectionEvent<Ip>> Stack<Ip>::poll_event() noexcept {
        ConnectionType* c = events_.pop_front();
        if (c == nullptr) {
            return std::nullopt;
        }
        const stream::Events snapshot = c->events_;
        c->events_                    = {};
        return EventType{.connection = c, .events = snapshot};
    }

    template <typename Ip>
    void Stack<Ip>::flush(const core::TimePoint now) noexcept {
        now_ = now;
        for (ConnectionType* c = acks_.front(); c != nullptr;) {
            ConnectionType* next = AckList::next_of(*c);  // a successful send unlinks c
            if (send_pure_ack(*c)) {
                maybe_finish(*c);
            }
            c = next;
        }
    }

    template <typename Ip>
    std::expected<Connection<Ip>*, ConnectError> Stack<Ip>::connect(const Endpoint peer,
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

}  // namespace aloe::tcp

#include <tcp_control.hpp>
#include <tcp_receive.hpp>
#include <tcp_transmit.hpp>
