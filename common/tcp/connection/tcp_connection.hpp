#pragma once

#include <aloe/core>
#include <aloe/stream>
#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <detail/control_block.hpp>
#include <read_view.hpp>
#include <tcp_config.hpp>
#include <tcp_fwd.hpp>

namespace aloe::tcp {

    /**
     * @brief One TCP connection as a zero-copy stream: the public handle over a slot of its stack, stable until
     * `release`.
     *
     * Every member runs on the stack's thread and takes its time from the stack's last stamp. The
     * state is a `detail::ControlBlock` the stack owns; the public members are the stream contract plus
     * the TCP queries, and each verb forwards to the stack. Construction takes the stack's private tag: handles exist
     * only inside a `Stack`.
     */
    template <typename Ip, typename CongestionControl>
    class Connection {
    public:
        using Packet   = Ip::Packet;
        using Sequence = wire::TcpSequence;
        using View     = ReadView<Packet>;

    private:
        struct PrivateTag {};
        using ControlBlock = detail::ControlBlock<Ip, CongestionControl>;

    public:
        Connection(PrivateTag, ControlBlock& block) noexcept
            : block_{block} {
        }

        Connection(const Connection&)            = delete;
        Connection& operator=(const Connection&) = delete;
        Connection(Connection&&)                 = delete;
        Connection& operator=(Connection&&)      = delete;
        ~Connection()                            = default;

        [[nodiscard]] std::uint32_t index() const noexcept {
            return block_.index;
        }
        [[nodiscard]] State state() const noexcept {
            return block_.state;
        }
        [[nodiscard]] Endpoint local() const noexcept {
            return block_.local;
        }
        [[nodiscard]] Endpoint remote() const noexcept {
            return block_.remote;
        }
        /// Undrained flags; inspection does not clear them.
        [[nodiscard]] stream::Events events() const noexcept {
            return block_.events;
        }
        /// The payload a segment carries: the smaller of the peer's MSS option and ours, less the timestamp option
        /// once the connection sends it. The SYN's MSS option keeps the full value.
        [[nodiscard]] std::uint16_t mss() const noexcept {
            return block_.mss();
        }
        /// RFC 7323 timestamps were negotiated: both ends offered them in the handshake and the config allows them.
        [[nodiscard]] bool timestamps() const noexcept {
            return block_.timestamps_negotiated();
        }

        /// SACK-permitted is in use: both ends sent it in the handshake and the config allows it. In SynSent, whether
        /// the SYN offers it.
        [[nodiscard]] bool sack() const noexcept {
            return block_.sack.in_use();
        }
        /// RFC 7323 window scaling is in use: both ends offered it in the handshake and the config allows it.
        [[nodiscard]] bool window_scaling() const noexcept {
            return block_.window.scale().has_value();
        }
        /// Our window scale on this connection: the stack's shift when scaling is in use, else zero.
        [[nodiscard]] std::uint8_t window_shift() const noexcept {
            return block_.window.shift();
        }
        /// The peer's window scale, at most 14: what its SYN or SYN-ACK offered when scaling is in use, else zero.
        [[nodiscard]] std::uint8_t peer_window_shift() const noexcept {
            return block_.send.peer_shift;
        }

        /// The unconsumed payloads of the held packets, in order. Invalidated by `consume` and the next `process`.
        [[nodiscard]] View unread() const noexcept {
            return block_.stack.unread(block_);
        }
        /// `count` at most `unread().size()`, asserted. Frees packets as they empty; may reopen the window.
        void consume(const std::size_t count) noexcept {
            block_.stack.consume(block_, count);
        }
        [[nodiscard]] bool peer_closed() const noexcept {
            return block_.receive.closed();
        }
        /// Packets held above a gap, waiting for the data that fills it; they count against `receive_segments`.
        [[nodiscard]] std::uint32_t out_of_order_segments() const noexcept {
            return block_.reassembly.segments();
        }
        /// Payload bytes held above a gap; not part of `unread()` until the gap fills.
        [[nodiscard]] std::size_t out_of_order_bytes() const noexcept {
            return block_.reassembly.bytes();
        }
        /// The receive budget the handshake settled: half of what `receive_segments` full segments of `mss()` hold, or
        /// `receive_window`, within what the window field states.
        [[nodiscard]] std::uint32_t receive_budget() const noexcept {
            return block_.window.budget();
        }
        /// The bytes on offer to the peer: from RCV.NXT to the right edge already offered.
        [[nodiscard]] std::uint32_t receive_window() const noexcept {
            return block_.window.window(block_.receive.next());
        }

        /// The MSS, within what the two windows allow: the peer's window less `unacknowledged()`, and the congestion
        /// window less `in_flight()`. In Established or CloseWait with no prepare open, the queue below the send cap
        /// and nothing marked lost that is still to be resent (`lost_bytes()`), else zero.
        [[nodiscard]] std::size_t writable() const noexcept {
            return block_.stack.writable(block_);
        }
        /// A span of at most `writable()` bytes inside a fresh packet, or nothing. One prepare at a time.
        [[nodiscard]] std::optional<std::span<std::byte>> prepare(const std::size_t count) noexcept {
            return block_.stack.prepare(block_, count);
        }
        /// Seals the prepared segment with `count` bytes and queues it. False: not sent, bytes not accepted.
        [[nodiscard]] bool commit(const std::size_t count) noexcept {
            return block_.stack.commit(block_, count);
        }
        /// `stream::send`: prepare, copy, commit per segment; the bytes accepted.
        [[nodiscard]] std::size_t send(const std::span<const std::byte> bytes) noexcept {
            return stream::send(*this, bytes);
        }
        /// The sequence after the last committed byte.
        [[nodiscard]] Sequence committed() const noexcept {
            return block_.send.nxt;
        }
        /// Everything before it reached the peer.
        [[nodiscard]] Sequence acknowledged() const noexcept {
            return block_.send.una;
        }
        [[nodiscard]] std::size_t unacknowledged() const noexcept {
            return block_.send.flight();
        }
        /// Segments in the retransmission queue: sent, or waiting to be, and not yet acknowledged whole.
        [[nodiscard]] std::size_t queued_segments() const noexcept {
            return block_.queue.size();
        }
        /// Payload bytes the peer acknowledged, each once: when a SACK block first reported it, or when an ACK first
        /// advanced `acknowledged()` past it.
        [[nodiscard]] std::uint64_t delivered() const noexcept {
            return block_.queue.delivered();
        }
        /// For tests and monitoring: what is in flight, the number the congestion window is held against:
        /// `unacknowledged()` less what the peer reported received with SACK blocks, or without SACK one segment per
        /// duplicate ACK with a hole of one segment left, and less what is marked lost and not resent since.
        [[nodiscard]] std::uint32_t in_flight() const noexcept {
            return block_.stack.in_flight(block_);
        }
        /// For tests and monitoring: the sequence space the peer reported received with SACK blocks and no cumulative
        /// ACK has covered yet.
        [[nodiscard]] std::uint32_t reported_bytes() const noexcept {
            return block_.queue.reported_bytes();
        }
        /// For tests and monitoring: the sequence space marked lost and not resent since.
        [[nodiscard]] std::uint32_t lost_bytes() const noexcept {
            return block_.queue.lost_bytes();
        }

        /// FIN after what was committed; no more sends. Nothing outside Established and CloseWait.
        void close() noexcept {
            block_.stack.close(block_);
        }
        /// RST now, Closed raised.
        void abort() noexcept {
            block_.stack.abort(block_);
        }
        /// Frees the slot, aborting first unless Closed. The owner's last call; the pointer is invalid after.
        void release() noexcept {
            block_.stack.release(block_);
        }

        /// The congestion window in bytes: the policy's, and in recovery the rate reduction's (RFC 9937) on its way
        /// down to the policy's target.
        [[nodiscard]] std::uint32_t congestion_window() const noexcept {
            return block_.recovery.window();
        }
        /// For tests and monitoring: the policy's per-connection state.
        [[nodiscard]] const CongestionControl::State& congestion_state() const noexcept {
            return block_.recovery.congestion();
        }
        /// Where loss recovery stands: open, in recovery since an entry was marked lost, or in the loss state since
        /// the retransmission timer expired.
        [[nodiscard]] RecoveryState recovery_state() const noexcept {
            return block_.recovery.state();
        }
        /// `recovery_state()` is `Recovery`: from the first entry marked lost to the ACK that covers everything sent
        /// by then, or a timeout.
        [[nodiscard]] bool in_recovery() const noexcept {
            return block_.recovery.in_recovery();
        }
        /// Duplicate ACKs since `acknowledged()` last moved, saturating. In recovery an ACK of new data does not clear
        /// the count; on a connection without SACK it takes back the duplicates it accounts for.
        [[nodiscard]] std::uint8_t duplicate_acks() const noexcept {
            return block_.recovery.duplicates();
        }
        /// RFC 6298's smoothed round-trip time; nothing before the first sample.
        [[nodiscard]] std::optional<core::Duration> srtt() const noexcept {
            return block_.rtt.srtt();
        }
        /// For tests and monitoring: RACK's reordering window as the connection stands now (RFC 8985), on a connection
        /// that uses SACK. Zero while reordering was never seen and the connection is in recovery or the loss state,
        /// or has three or more segments reported received; else a quarter of the minimum round trip times the
        /// multiplier duplicate reports grew, never more than `srtt()`.
        [[nodiscard]] core::Duration reordering_window() const noexcept {
            return block_.reordering_window();
        }
        /// For tests and monitoring: a tail probe is out, from its send until a cumulative ACK reaches its end (RFC
        /// 8985 7.3), on a connection that uses SACK. While one is, no other probe goes.
        [[nodiscard]] bool tail_probe_in_flight() const noexcept {
            return block_.probe_state.in_flight();
        }
        /// For tests and monitoring: whether the last tail probe was needless or repaired a loss is not known yet (RFC
        /// 8985 7.4); an ACK that lands on the probe's end leaves it open, so it usually outlives the flight.
        [[nodiscard]] bool tail_probe_verdict_open() const noexcept {
            return block_.probe_state.verdict_open();
        }
        /// The retransmission timeout now, the backoff included.
        [[nodiscard]] core::Duration rto() const noexcept {
            return block_.rtt.rto();
        }
        /// Expiries of the retransmission timer since the last ACK of new data, a resend deferred or refused included;
        /// those that found the peer's window zero after the handshake count with the zero-window probes instead.
        [[nodiscard]] std::uint8_t retransmit_count() const noexcept {
            return block_.retransmit_count;
        }
        /// The peer's window is zero with nothing in flight: the timer sends zero-window probes until it opens.
        [[nodiscard]] bool probing() const noexcept {
            return block_.probing();
        }

    private:
        friend class Stack<Ip, CongestionControl>;  // names PrivateTag, reads block_

        ControlBlock& block_;
    };

    /// One notification from `poll_event`: the connection and the flags taken from it.
    template <typename Ip, typename CongestionControl>
    struct ConnectionEvent {
        Connection<Ip, CongestionControl>* connection = nullptr;  ///< Valid until the owner releases it.
        stream::Events events{};
    };

}  // namespace aloe::tcp
