#pragma once

#include <algorithm>
#include <aloe/core>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <congestion_control.hpp>
#include <flow_table.hpp>
#include <frame_template.hpp>
#include <read_view.hpp>
#include <stream.hpp>
#include <stream_events.hpp>
#include <tcp_config.hpp>
#include <tcp_fwd.hpp>
#include <tcp_header.hpp>
#include <tcp_node_pool.hpp>
#include <tcp_options.hpp>
#include <tcp_sequence.hpp>
#include <timer_wheel.hpp>

namespace aloe::tcp {

    namespace detail {

        /// What the connection's one wheel timer does when it fires.
        enum class TimerKind : std::uint8_t {
            None,        ///< Not armed.
            Retransmit,  ///< The RTO: the head of the retransmission queue goes again.
            ArpPoll,     ///< A SYN, SYN-ACK or FIN whose first send found no next hop: the send is tried again.
            Probe,       ///< The peer's window is zero and nothing is in flight: a zero-window probe goes.
        };

    }  // namespace detail

    /**
     * @brief One TCP connection as a zero-copy stream: a slot of its stack, stable until `release`.
     *
     * Every member runs on the stack's thread and takes its time from the stack's last stamp. The
     * state is owned by the stack; the public members are the stream contract plus the TCP
     * queries. Construction takes the stack's private tag: slots exist only inside a `Stack`.
     */
    template <typename Ip, typename CongestionControl>
    class Connection {
    public:
        using Packet   = Ip::Packet;
        using Sequence = wire::TcpSequence;
        using View     = ReadView<Packet>;

    private:
        struct PrivateTag {};

    public:
        explicit Connection(PrivateTag) noexcept
            : timer_{this} {
        }

        Connection(const Connection&)            = delete;
        Connection& operator=(const Connection&) = delete;
        Connection(Connection&&)                 = delete;
        Connection& operator=(Connection&&)      = delete;
        ~Connection()                            = default;

        [[nodiscard]] std::uint32_t index() const noexcept {
            return index_;
        }
        [[nodiscard]] State state() const noexcept {
            return state_;
        }
        [[nodiscard]] Endpoint local() const noexcept {
            return local_;
        }
        [[nodiscard]] Endpoint remote() const noexcept {
            return remote_;
        }
        /// Undrained flags; inspection does not clear them.
        [[nodiscard]] stream::Events events() const noexcept {
            return events_;
        }
        /// The payload a segment carries: the smaller of the peer's MSS option and ours, less the timestamp option
        /// once the connection sends it. The SYN's MSS option keeps the full value.
        [[nodiscard]] std::uint16_t mss() const noexcept {
            const std::uint16_t negotiated = std::min(peer_mss_, stack_->mss_);
            const std::uint16_t options =
                timestamps() ? static_cast<std::uint16_t>(wire::TcpOptions::timestamps_size) : std::uint16_t{0};
            return static_cast<std::uint16_t>(negotiated - std::min(negotiated, options));
        }
        /// RFC 7323 timestamps were negotiated: both ends offered them in the handshake and the config allows them.
        [[nodiscard]] bool timestamps() const noexcept {
            return send_timestamps_ && state_ != State::SynSent;
        }

        /// The unconsumed payloads of the held packets, in order. Invalidated by `consume` and the next `process`.
        [[nodiscard]] View unread() const noexcept {
            return View{stack_->pool_, chain_head_, unread_bytes_};
        }
        /// `count` at most `unread().size()`, asserted. Frees packets as they empty; may reopen the window.
        void consume(const std::size_t count) noexcept {
            stack_->consume(*this, count);
        }
        [[nodiscard]] bool peer_closed() const noexcept {
            return peer_closed_;
        }

        /// `min(mss, min(peer window, congestion window + limited transmit's allowance) - in flight)` in Established or
        /// CloseWait with no prepare open and the queue below the send cap, else zero.
        [[nodiscard]] std::size_t writable() const noexcept {
            return stack_->writable(*this);
        }
        /// A span of at most `writable()` bytes inside a fresh packet, or nothing. One prepare at a time.
        [[nodiscard]] std::optional<std::span<std::byte>> prepare(const std::size_t count) noexcept {
            return stack_->prepare(*this, count);
        }
        /// Seals the prepared segment with `count` bytes and queues it. False: not sent, bytes not accepted.
        [[nodiscard]] bool commit(const std::size_t count) noexcept {
            return stack_->commit(*this, count);
        }
        /// `stream::send`: prepare, copy, commit per segment; the bytes accepted.
        [[nodiscard]] std::size_t send(const std::span<const std::byte> bytes) noexcept {
            return stream::send(*this, bytes);
        }
        /// The sequence after the last committed byte.
        [[nodiscard]] Sequence committed() const noexcept {
            return snd_nxt_;
        }
        /// Everything before it reached the peer.
        [[nodiscard]] Sequence acknowledged() const noexcept {
            return snd_una_;
        }
        [[nodiscard]] std::size_t unacknowledged() const noexcept {
            return snd_nxt_.value - snd_una_.value;
        }
        /// Segments in the retransmission queue: sent, or waiting to be, and not yet acknowledged whole.
        [[nodiscard]] std::size_t queued_segments() const noexcept {
            return queue_count_;
        }
        /// Payload bytes the peer acknowledged, counted as each ACK advances `acknowledged()`.
        [[nodiscard]] std::uint64_t delivered() const noexcept {
            return delivered_;
        }

        /// FIN after what was committed; no more sends. Nothing outside Established and CloseWait.
        void close() noexcept {
            stack_->close(*this);
        }
        /// RST now, Closed raised.
        void abort() noexcept {
            stack_->abort(*this);
        }
        /// Frees the slot, aborting first unless Closed. The owner's last call; the pointer is invalid after.
        void release() noexcept {
            stack_->release(*this);
        }

        /// The congestion window the policy allows, in bytes.
        [[nodiscard]] std::uint32_t congestion_window() const noexcept {
            return CongestionControl::window(congestion_);
        }
        /// For tests and monitoring: the policy's per-connection state.
        [[nodiscard]] const CongestionControl::State& congestion_state() const noexcept {
            return congestion_;
        }
        /// Fast recovery: entered on the third duplicate ACK, left on the ACK past `recover` or a timeout.
        [[nodiscard]] bool in_recovery() const noexcept {
            return in_recovery_;
        }
        /// Duplicate ACKs since the last ACK of new data, saturating.
        [[nodiscard]] std::uint8_t duplicate_acks() const noexcept {
            return dupacks_;
        }
        /// RFC 6298's smoothed round-trip time; nothing before the first sample.
        [[nodiscard]] std::optional<core::Duration> srtt() const noexcept {
            return srtt_;
        }
        /// The retransmission timeout now, the backoff included.
        [[nodiscard]] core::Duration rto() const noexcept {
            return rto_;
        }
        /// Expiries of the retransmission timer since the last ACK of new data, a resend deferred or refused included.
        [[nodiscard]] std::uint8_t retransmit_count() const noexcept {
            return retransmit_count_;
        }
        /// The peer's window is zero with nothing in flight: the timer sends zero-window probes until it opens.
        [[nodiscard]] bool probing() const noexcept {
            return timer_kind_ == detail::TimerKind::Probe;
        }

    private:
        friend class Stack<Ip,
                           CongestionControl>;  // forms the pointers to the link members; using them needs no access

        /// The wheel's node, carrying the way back to its connection.
        struct TimerNode : loop::Timer {
            explicit TimerNode(Connection* owner) noexcept
                : loop::Timer{&Connection::on_timer},
                  connection{owner} {
            }

            Connection* connection = nullptr;
        };

        static void on_timer(loop::Timer& timer) noexcept {
            Connection& self = *static_cast<TimerNode&>(timer).connection;
            self.stack_->on_timer(self);
        }

        /// Back to the state of a fresh slot; the stack, index and timer trampoline stay.
        void clear() noexcept {
            assert(!timer_.armed());
            state_     = State::Closed;
            owned_     = false;
            indexed_   = false;
            local_     = {};
            remote_    = {};
            key_       = {};
            base_hash_ = 0;
            events_    = {};
            assert(!event_link_.linked && !ack_link_.linked && !retry_link_.linked && !deferred_link_.linked);
            assert(queue_head_ == detail::no_node && queue_tail_ == detail::no_node && queue_count_ == 0 &&
                   "the stack drops the queue first");
            ack_pending_    = false;
            finish_pending_ = false;
            rcv_nxt_        = {};
            rcv_adv_        = {};
            unread_bytes_   = 0;
            recovery_end_.reset();
            peer_closed_             = false;
            full_segments_since_ack_ = 0;
            chain_head_              = detail::no_node;
            chain_tail_              = detail::no_node;
            chain_count_             = 0;
            iss_                     = {};
            snd_nxt_                 = {};
            prepared_.reset();
            prepared_size_    = 0;
            fin_sent_         = false;
            fin_reserved_     = false;
            template_         = {};
            last_data_send_   = std::nullopt;
            snd_una_          = {};
            delivered_        = 0;
            delivered_time_   = {};
            snd_wnd_          = 0;
            peer_mss_         = default_peer_mss;
            wl1_              = {};
            wl2_              = {};
            dupacks_          = 0;
            recover_          = {};
            in_recovery_      = false;
            loss_episode_     = false;
            limited_transmit_ = 0;
            srtt_             = std::nullopt;
            rttvar_           = {};
            rto_              = {};
            retransmit_count_ = 0;
            timer_kind_       = detail::TimerKind::None;
            unresolved_polls_ = 0;
            probes_out_       = 0;
            probe_started_    = std::nullopt;
            probe_interval_   = {};
            congestion_       = {};
            send_timestamps_  = false;
            ts_offset_        = 0;
            ts_recent_        = 0;
            ts_recent_stamp_  = {};
            ts_recent_valid_  = false;
            last_ack_sent_    = {};
        }

        // Identity and ownership.
        Stack<Ip, CongestionControl>* stack_ = nullptr;
        std::uint32_t index_                 = 0;
        State state_                         = State::Closed;
        bool owned_                          = false;  ///< An application holds it: after Accepted, or from connect.
        bool indexed_                        = false;  ///< Its key is in the flow table.
        Endpoint local_{};
        Endpoint remote_{};
        FlowKey key_{};
        std::uint32_t base_hash_ = 0;

        // Events and the stack's lists.
        stream::Events events_{};
        detail::Link<Connection> event_link_{};
        detail::Link<Connection> ack_link_{};
        detail::Link<Connection> retry_link_{};
        detail::Link<Connection> deferred_link_{};  ///< A resend found its packet lent; the next flush tries again.
        bool ack_pending_    = false;               ///< An ordinary ACK waits for flush.
        bool finish_pending_ = false;  ///< The protocol is done; Closed is raised once the last ACK is queued.

        // Receive: acceptance, consumption and acknowledgement state.
        Sequence rcv_nxt_{};
        Sequence rcv_adv_{};  ///< The right edge already offered to the peer; never retreats.
        std::size_t unread_bytes_ = 0;
        std::optional<Sequence> recovery_end_{};  ///< The furthest end of data dropped above a gap.
        bool peer_closed_                     = false;
        std::uint8_t full_segments_since_ack_ = 0;  ///< Saturates at two.
        std::uint32_t chain_head_             = detail::no_node;
        std::uint32_t chain_tail_             = detail::no_node;
        std::uint32_t chain_count_            = 0;

        // Transmit: preparation and outgoing sequence allocation.
        Sequence iss_{};
        Sequence snd_nxt_{};
        std::optional<Packet> prepared_{};
        std::size_t prepared_size_ = 0;
        bool fin_sent_             = false;  ///< The FIN took its number, `snd_nxt - 1`; its entry carries it.
        bool fin_reserved_         = false;  ///< A node of the send pool is set aside for the FIN, since Established.

        // The retransmission queue: every segment that occupies sequence space, oldest first, and the frame template.
        std::uint32_t queue_head_  = detail::no_node;
        std::uint32_t queue_tail_  = detail::no_node;
        std::uint32_t queue_count_ = 0;
        detail::FrameTemplate template_{};
        std::optional<core::TimePoint> last_data_send_{};  ///< The last transmission with payload; nothing before one.

        // Peer feedback: what the ACKs told us.
        Sequence snd_una_{};
        std::uint32_t snd_wnd_  = 0;
        std::uint16_t peer_mss_ = default_peer_mss;
        Sequence wl1_{};
        Sequence wl2_{};
        std::uint64_t delivered_ = 0;       ///< Payload bytes acknowledged: what each ACK covers less SYN and FIN.
        core::TimePoint delivered_time_{};  ///< The ACK that last advanced `delivered_`.

        // Loss recovery (RFC 5681, RFC 3042, RFC 6582): the duplicate count, limited transmit's allowance, the recover
        // point, the ISS at the open, and which episode the connection is in.
        std::uint8_t dupacks_ = 0;  ///< Duplicate ACKs since the last ACK of new data; saturates.
        /// RFC 6582's `recover`: the last sequence sent as an episode began; outside an episode, one before `snd_una`
        /// once an ACK of new data moved it, so the serial comparison never sees it half the space behind.
        Sequence recover_{};
        bool in_recovery_  = false;           ///< Fast recovery, from the third duplicate to the ACK past `recover_`.
        bool loss_episode_ = false;           ///< After a timeout: each ACK short of `recover_` resends the new head.
        std::uint32_t limited_transmit_ = 0;  ///< Bytes beyond the windows the first two duplicates allow; RFC 3042.

        // Maintenance: the retransmission timer and the round-trip estimator.
        TimerNode timer_;
        detail::TimerKind timer_kind_ = detail::TimerKind::None;  ///< What `timer_` does when it fires.
        std::optional<core::Duration> srtt_{};                    ///< Empty until the first sample.
        core::Duration rttvar_{};
        core::Duration rto_{};  ///< `rto_initial` until the first sample; doubled by each expiry up to `rto_max`.
        std::uint8_t retransmit_count_  = 0;  ///< Expiries since the last ACK of new data, deferred and refused too.
        std::uint16_t unresolved_polls_ = 0;  ///< Polls since the last send that left; the config bounds them.
        std::uint8_t probes_out_ = 0;  ///< Zero-window probes since the last acceptable ACK; `data_retries` caps.
        std::optional<core::TimePoint> probe_started_{};  ///< The first unanswered probe: the user timeout's start.
        core::Duration probe_interval_{};  ///< The next probe's delay: the RTO, doubled per probe up to `rto_max`.
        CongestionControl::State congestion_{};  ///< The policy's state; reset where the connection is Established.

        // Timestamps (RFC 7323): the offer, the clock's offset, the echo and what PAWS compares against.
        bool send_timestamps_ = false;  ///< In SynSent the SYN's offer; from the handshake on, the negotiated option.
        std::uint32_t ts_offset_ = 0;   ///< Added to the stack's millisecond clock; drawn per connection.
        std::uint32_t ts_recent_ = 0;   ///< TS.Recent: the value the next segment echoes.
        core::TimePoint ts_recent_stamp_{};  ///< When TS.Recent was last set; PAWS ignores it after 24 days.
        bool ts_recent_valid_ = false;       ///< TS.Recent holds a value from the peer.
        Sequence last_ack_sent_{};  ///< Last.ACK.sent: the acknowledgement the last ACK-bearing segment carried.
    };

    /// One notification from `poll_event`: the connection and the flags taken from it.
    template <typename Ip, typename CongestionControl>
    struct ConnectionEvent {
        Connection<Ip, CongestionControl>* connection = nullptr;  ///< Valid until the owner releases it.
        stream::Events events{};
    };

}  // namespace aloe::tcp
