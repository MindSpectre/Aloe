#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <flow_table.hpp>
#include <read_view.hpp>
#include <stream.hpp>
#include <stream_events.hpp>
#include <tcp_config.hpp>
#include <tcp_fwd.hpp>
#include <tcp_header.hpp>
#include <tcp_node_pool.hpp>
#include <tcp_sequence.hpp>
#include <timer_wheel.hpp>

namespace aloe::tcp {

    /**
     * @brief One TCP connection as a zero-copy stream: a slot of its stack, stable until `release`.
     *
     * Every member runs on the stack's thread and takes its time from the stack's last stamp. The
     * state is owned by the stack; the public members are the stream contract plus the TCP
     * queries. Construction takes the stack's private tag: slots exist only inside a `Stack`.
     */
    template <typename Ip>
    class Connection {
    public:
        using Packet   = typename Ip::Packet;
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
        /// What we send with: the smaller of the peer's option and ours.
        [[nodiscard]] std::uint16_t mss() const noexcept {
            return std::min(peer_mss_, stack_->mss_);
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

        /// `min(mss, peer window - in flight)` in Established or CloseWait with no prepare open, else zero.
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

    private:
        friend class Stack<Ip>;  // forms the pointers to the link members; using them needs no access

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
            assert(!event_link_.linked && !ack_link_.linked && !retry_link_.linked);
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
            prepared_size_   = 0;
            fin_sent_        = false;
            fin_sequence_    = {};
            snd_una_         = {};
            snd_wnd_         = 0;
            peer_mss_        = default_peer_mss;
            wl1_             = {};
            wl2_             = {};
            tries_           = 0;
            unresolved_wait_ = false;
        }

        // Identity and ownership.
        Stack<Ip>* stack_    = nullptr;
        std::uint32_t index_ = 0;
        State state_         = State::Closed;
        bool owned_          = false;  ///< An application holds it: after Accepted, or from connect.
        bool indexed_        = false;  ///< Its key is in the flow table.
        Endpoint local_{};
        Endpoint remote_{};
        FlowKey key_{};
        std::uint32_t base_hash_ = 0;

        // Events and the stack's lists.
        stream::Events events_{};
        detail::Link<Connection> event_link_{};
        detail::Link<Connection> ack_link_{};
        detail::Link<Connection> retry_link_{};
        bool ack_pending_    = false;  ///< An ordinary ACK waits for flush.
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
        bool fin_sent_             = false;
        Sequence fin_sequence_{};

        // Peer feedback: what the ACKs told us.
        Sequence snd_una_{};
        std::uint32_t snd_wnd_  = 0;
        std::uint16_t peer_mss_ = default_peer_mss;
        Sequence wl1_{};
        Sequence wl2_{};

        // Maintenance: the control retransmit timer.
        TimerNode timer_;
        std::uint8_t tries_   = 0;
        bool unresolved_wait_ = false;  ///< The last control send waited for ARP; the next fire is a poll, not a try.
    };

    /// One notification from `poll_event`: the connection and the flags taken from it.
    template <typename Ip>
    struct ConnectionEvent {
        Connection<Ip>* connection = nullptr;  ///< Valid until the owner releases it.
        stream::Events events{};
    };

}  // namespace aloe::tcp
