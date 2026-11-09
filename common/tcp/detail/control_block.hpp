#pragma once

#include <algorithm>
#include <aloe/loop>
#include <aloe/stream>
#include <aloe/wire>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <detail/ack_policy.hpp>
#include <detail/frame_template.hpp>
#include <detail/loss_recovery.hpp>
#include <detail/receive_queue.hpp>
#include <detail/retransmission_queue.hpp>
#include <detail/rtt_estimator.hpp>
#include <detail/send_sequence.hpp>
#include <detail/tcp_timestamps.hpp>
#include <detail/zero_window_probe.hpp>
#include <flow_table.hpp>
#include <tcp_config.hpp>
#include <tcp_fwd.hpp>

namespace aloe::tcp::detail {

    /// What the connection's one wheel timer does when it fires.
    enum class TimerKind : std::uint8_t {
        None,        ///< Not armed.
        Retransmit,  ///< The RTO: the head of the retransmission queue goes again.
        ArpPoll,     ///< A SYN, SYN-ACK or FIN whose first send found no next hop: the send is tried again.
        Probe,       ///< The peer's window is zero and nothing is in flight: a zero-window probe goes.
    };

    /**
     * @brief One connection's state, RFC 9293's transmission control block: the identity, the events and the
     * list links, the protocol's parts one per concern, and the one wheel timer.
     *
     * Plain single-thread memory that the stack's three parts read and write on the shard's thread; nothing in it
     * is shared across threads. `tcp::Connection` is the public handle over one block; the stack owns the blocks in
     * a deque, one per slot, and maps a block back to its handle through `index`.
     */
    template <typename Ip, typename CongestionControl>
    struct ControlBlock {
        using Packet = Ip::Packet;

        /// An open preparation: the packet the application writes into and the bytes it may commit.
        struct Preparation {
            Packet packet{};
            std::size_t size = 0;
        };

        /// The wheel's node, carrying the way back to its block.
        struct TimerNode : loop::Timer {
            TimerNode(const loop::Timer::Function callback, ControlBlock& owner) noexcept
                : loop::Timer{callback},
                  block{owner} {
            }

            ControlBlock& block;
        };

        /// `fire` is the stack's trampoline; `slot` the block's index in the stack's deque; `stack_mss` the stack's.
        ControlBlock(Stack<Ip, CongestionControl>& owner,
                     const loop::Timer::Function fire,
                     const std::uint32_t slot,
                     const std::uint16_t stack_mss) noexcept
            : stack{owner},
              index{slot},
              local_mss{stack_mss},
              timer{fire, *this} {
        }

        ControlBlock(const ControlBlock&)            = delete;
        ControlBlock& operator=(const ControlBlock&) = delete;
        ControlBlock(ControlBlock&&)                 = delete;
        ControlBlock& operator=(ControlBlock&&)      = delete;
        ~ControlBlock()                              = default;

        /// The payload a segment carries: the smaller of the peer's MSS option and ours, less the timestamp option
        /// once the connection sends it. The SYN's MSS option keeps the full value.
        [[nodiscard]] std::uint16_t mss() const noexcept {
            const std::uint16_t negotiated = std::min(send.peer_mss, local_mss);
            const std::uint16_t options    = timestamps_negotiated()
                                                 ? static_cast<std::uint16_t>(wire::TcpOptions::timestamps_size)
                                                 : std::uint16_t{0};
            return static_cast<std::uint16_t>(negotiated - std::min(negotiated, options));
        }
        /// RFC 7323 timestamps were negotiated: both ends offered them in the handshake and the config allows them.
        [[nodiscard]] bool timestamps_negotiated() const noexcept {
            return timestamps.enabled() && state != State::SynSent;
        }
        /// The peer's window is zero with nothing in flight: the timer sends zero-window probes until it opens.
        [[nodiscard]] bool probing() const noexcept {
            return timer_kind == TimerKind::Probe;
        }

        /// Back to the state of a fresh slot; the stack, the index, the MSS and the timer trampoline stay. The stack
        /// unlinks the block from its lists, returns the held packets and drops the queue first.
        void clear() noexcept {
            assert(!timer.armed());
            assert(!event_link.linked && !ack_link.linked && !retry_link.linked && !deferred_link.linked);
            assert(receive.segments() == 0 && queue.empty() && "the stack returns the packets first");
            state            = State::Closed;
            owned            = false;
            indexed          = false;
            local            = {};
            remote           = {};
            key              = {};
            base_hash        = 0;
            frame            = {};
            events           = {};
            finish_pending   = false;
            receive          = {};
            ack_policy       = {};
            send             = {};
            prepared         = std::nullopt;
            queue            = {};
            recovery         = {};
            rtt              = {};
            probe            = {};
            timestamps       = {};
            timer_kind       = TimerKind::None;
            retransmit_count = 0;
            unresolved_polls = 0;
        }

        // Identity and ownership.
        Stack<Ip, CongestionControl>& stack;
        std::uint32_t index     = 0;
        std::uint16_t local_mss = 0;  ///< The stack's MSS: `max_l4_size() - 20`.
        State state             = State::Closed;
        bool owned              = false;  ///< An application holds it: after Accepted, or from connect.
        bool indexed            = false;  ///< Its key is in the flow table.
        Endpoint local{};
        Endpoint remote{};
        FlowKey key{};
        std::uint32_t base_hash = 0;
        FrameTemplate frame{};  ///< The link and network headers of every segment to the peer.

        // Events and the stack's lists; being on the ACK list is having an ordinary ACK wait for flush.
        stream::Events events{};
        Link<ControlBlock> event_link{};
        Link<ControlBlock> ack_link{};
        Link<ControlBlock> retry_link{};
        Link<ControlBlock> deferred_link{};  ///< A resend found its packet lent; the next flush tries again.
        bool finish_pending = false;         ///< The protocol is done; Closed is raised once the last ACK is queued.

        // The protocol's state, one part per concern.
        ReceiveQueue<Packet> receive{};
        AckPolicy ack_policy{};
        SendSequence send{};
        std::optional<Preparation> prepared{};
        RetransmissionQueue<Packet> queue{};
        LossRecovery<CongestionControl> recovery{};
        RttEstimator rtt{};
        ZeroWindowProbe probe{};
        Timestamps timestamps{};

        // The one wheel timer: the RTO, the ARP poll or the probe.
        TimerNode timer;
        TimerKind timer_kind           = TimerKind::None;  ///< What `timer` does when it fires.
        std::uint8_t retransmit_count  = 0;  ///< Expiries since the last ACK of new data, deferred and refused too.
        std::uint16_t unresolved_polls = 0;  ///< Polls since the last send that left; the config bounds them.
    };

}  // namespace aloe::tcp::detail
