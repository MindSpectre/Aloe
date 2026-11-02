#pragma once

#include <algorithm>
#include <aloe/core>
#include <aloe/loop>
#include <aloe/stream>
#include <aloe/wire>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <ack_policy.hpp>
#include <flow_table.hpp>
#include <frame_template.hpp>
#include <loss_recovery.hpp>
#include <read_view.hpp>
#include <receive_queue.hpp>
#include <retransmission_queue.hpp>
#include <rtt_estimator.hpp>
#include <send_sequence.hpp>
#include <tcp_config.hpp>
#include <tcp_fwd.hpp>
#include <tcp_timestamps.hpp>
#include <zero_window_probe.hpp>

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
            const std::uint16_t negotiated = std::min(send_.peer_mss, stack_->mss_);
            const std::uint16_t options =
                timestamps() ? static_cast<std::uint16_t>(wire::TcpOptions::timestamps_size) : std::uint16_t{0};
            return static_cast<std::uint16_t>(negotiated - std::min(negotiated, options));
        }
        /// RFC 7323 timestamps were negotiated: both ends offered them in the handshake and the config allows them.
        [[nodiscard]] bool timestamps() const noexcept {
            return timestamps_.enabled() && state_ != State::SynSent;
        }

        /// The unconsumed payloads of the held packets, in order. Invalidated by `consume` and the next `process`.
        [[nodiscard]] View unread() const noexcept {
            return receive_.view(stack_->pool_);
        }
        /// `count` at most `unread().size()`, asserted. Frees packets as they empty; may reopen the window.
        void consume(const std::size_t count) noexcept {
            stack_->consume(*this, count);
        }
        [[nodiscard]] bool peer_closed() const noexcept {
            return receive_.closed();
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
            return send_.nxt;
        }
        /// Everything before it reached the peer.
        [[nodiscard]] Sequence acknowledged() const noexcept {
            return send_.una;
        }
        [[nodiscard]] std::size_t unacknowledged() const noexcept {
            return send_.flight();
        }
        /// Segments in the retransmission queue: sent, or waiting to be, and not yet acknowledged whole.
        [[nodiscard]] std::size_t queued_segments() const noexcept {
            return queue_.size();
        }
        /// Payload bytes the peer acknowledged, counted as each ACK advances `acknowledged()`.
        [[nodiscard]] std::uint64_t delivered() const noexcept {
            return queue_.delivered();
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
            return recovery_.window();
        }
        /// For tests and monitoring: the policy's per-connection state.
        [[nodiscard]] const CongestionControl::State& congestion_state() const noexcept {
            return recovery_.congestion();
        }
        /// Fast recovery: entered on the third duplicate ACK, left on the ACK past `recover` or a timeout.
        [[nodiscard]] bool in_recovery() const noexcept {
            return recovery_.in_recovery();
        }
        /// Duplicate ACKs since the last ACK of new data, saturating.
        [[nodiscard]] std::uint8_t duplicate_acks() const noexcept {
            return recovery_.duplicates();
        }
        /// RFC 6298's smoothed round-trip time; nothing before the first sample.
        [[nodiscard]] std::optional<core::Duration> srtt() const noexcept {
            return rtt_.srtt();
        }
        /// The retransmission timeout now, the backoff included.
        [[nodiscard]] core::Duration rto() const noexcept {
            return rtt_.rto();
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

        /// An open preparation: the packet the application writes into and the bytes it may commit.
        struct Preparation {
            Packet packet{};
            std::size_t size = 0;
        };

        /// Back to the state of a fresh slot; the stack, index and timer trampoline stay. The stack unlinks the
        /// connection from its lists, returns the held packets and drops the queue first.
        void clear() noexcept {
            assert(!timer_.armed());
            assert(!event_link_.linked && !ack_link_.linked && !retry_link_.linked && !deferred_link_.linked);
            assert(receive_.segments() == 0 && queue_.empty() && "the stack returns the packets first");
            state_            = State::Closed;
            owned_            = false;
            indexed_          = false;
            local_            = {};
            remote_           = {};
            key_              = {};
            base_hash_        = 0;
            template_         = {};
            events_           = {};
            finish_pending_   = false;
            receive_          = {};
            ack_policy_       = {};
            send_             = {};
            prepared_         = std::nullopt;
            queue_            = {};
            recovery_         = {};
            rtt_              = {};
            probe_            = {};
            timestamps_       = {};
            timer_kind_       = detail::TimerKind::None;
            retransmit_count_ = 0;
            unresolved_polls_ = 0;
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
        detail::FrameTemplate template_{};  ///< The link and network headers of every segment to the peer.

        // Events and the stack's lists; being on the ACK list is having an ordinary ACK wait for flush.
        stream::Events events_{};
        detail::Link<Connection> event_link_{};
        detail::Link<Connection> ack_link_{};
        detail::Link<Connection> retry_link_{};
        detail::Link<Connection> deferred_link_{};  ///< A resend found its packet lent; the next flush tries again.
        bool finish_pending_ = false;  ///< The protocol is done; Closed is raised once the last ACK is queued.

        // The protocol's state, one part per concern.
        detail::ReceiveQueue<Packet> receive_{};
        detail::AckPolicy ack_policy_{};
        detail::SendSequence send_{};
        std::optional<Preparation> prepared_{};
        detail::RetransmissionQueue<Packet> queue_{};
        detail::LossRecovery<CongestionControl> recovery_{};
        detail::RttEstimator rtt_{};
        detail::ZeroWindowProbe probe_{};
        detail::Timestamps timestamps_{};

        // The one wheel timer: the RTO, the ARP poll or the probe.
        TimerNode timer_;
        detail::TimerKind timer_kind_   = detail::TimerKind::None;  ///< What `timer_` does when it fires.
        std::uint8_t retransmit_count_  = 0;  ///< Expiries since the last ACK of new data, deferred and refused too.
        std::uint16_t unresolved_polls_ = 0;  ///< Polls since the last send that left; the config bounds them.
    };

    /// One notification from `poll_event`: the connection and the flags taken from it.
    template <typename Ip, typename CongestionControl>
    struct ConnectionEvent {
        Connection<Ip, CongestionControl>* connection = nullptr;  ///< Valid until the owner releases it.
        stream::Events events{};
    };

}  // namespace aloe::tcp
