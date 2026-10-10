#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>

#include <tcp_stack.hpp>

// The transmit path of tcp::Stack: every segment leaves through send_segment; pure ACKs and their policy.
namespace aloe::tcp {

    /**
     * Prepends the header (and the MSS option on a SYN) to `packet`, which holds the payload, and hands
     * it to IP. A segment with ACK offers the receive window: the candidate edge `rcv_nxt + (B - unread)`
     * when it is later than the current edge and a packet slot is free, else the current edge. Only a
     * successful send records the offer and clears the pending ACK; a refusal returns the packet to the
     * pool and leaves every number as it was.
     */
    template <typename Ip>
    std::expected<void, net::SendError> Stack<Ip>::send_segment(ConnectionType& c,
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
            const Sequence candidate = c.rcv_nxt_ + (byte_budget_ - static_cast<std::uint32_t>(c.unread_bytes_));
            if (candidate.after(c.rcv_adv_) && c.chain_count_ < config_.receive_segments) {
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
            packet = Packet{};  // NOLINT(bugprone-use-after-move): IP moves only on success; the packet is ours to drop
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

    template <typename Ip>
    std::expected<void, net::SendError>
    Stack<Ip>::send_control(ConnectionType& c, const wire::TcpFlags flags, const Sequence sequence) noexcept {
        std::optional<Packet> packet = ip_->allocate();
        if (!packet) {
            ++counters_.send_refused;
            return std::unexpected{net::SendError::Refused};
        }
        return send_segment(c, std::move(*packet), sequence, flags, flags.has(wire::TcpFlag::Syn));
    }

    template <typename Ip>
    bool Stack<Ip>::send_pure_ack(ConnectionType& c) noexcept {
        if (send_control(c, wire::TcpFlags{wire::TcpFlag::Ack}, c.snd_nxt_)) {
            ++counters_.pure_acks_sent;
            return true;
        }
        return false;
    }

    /// An ordinary ACK: marked, sent by flush unless a data segment carries it first.
    template <typename Ip>
    void Stack<Ip>::queue_ack(ConnectionType& c) noexcept {
        c.ack_pending_ = true;
        if (!acks_.contains(c)) {
            acks_.push_back(c);
        }
    }

    /// An immediate ACK: queued through IP before the next segment is processed; a refusal leaves it pending.
    template <typename Ip>
    void Stack<Ip>::ack_now(ConnectionType& c) noexcept {
        if (!send_pure_ack(c)) {
            queue_ack(c);
        }
    }

    /// A Writable hint at the next process, at most one per marked connection.
    template <typename Ip>
    void Stack<Ip>::mark_retry(ConnectionType& c) noexcept {
        if (!retries_.contains(c)) {
            retries_.push_back(c);
        }
    }

    template <typename Ip>
    std::size_t Stack<Ip>::writable(const ConnectionType& c) const noexcept {
        if (c.prepared_ || (c.state_ != State::Established && c.state_ != State::CloseWait)) {
            return 0;
        }
        const std::uint32_t in_flight = c.snd_nxt_.value - c.snd_una_.value;
        const std::uint32_t usable    = c.snd_wnd_ > in_flight ? c.snd_wnd_ - in_flight : 0;  // no unsigned underflow
        return std::min<std::size_t>(c.mss(), usable);
    }

    /// Advances the chain's front by `count` bytes, freeing packets as they empty; a reopened edge queues an update.
    template <typename Ip>
    void Stack<Ip>::consume(ConnectionType& c, const std::size_t count) noexcept {
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
        const Sequence candidate = c.rcv_nxt_ + (byte_budget_ - static_cast<std::uint32_t>(c.unread_bytes_));
        if (candidate.after(c.rcv_adv_) && c.chain_count_ < config_.receive_segments) {
            queue_ack(c);  // an ordinary window update, including the reopening of a zero window
        }
    }

    /**
     * A span of `min(count, writable())` bytes inside a fresh packet, which the connection holds until `commit`.
     * Nothing, without allocating, for a count of zero or nothing writable; an allocation failure marks a retry
     * hint and retains nothing.
     */
    template <typename Ip>
    std::optional<std::span<std::byte>> Stack<Ip>::prepare(ConnectionType& c, const std::size_t count) noexcept {
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
            mark_retry(c);  // the pool may have packets by the next process
            return std::nullopt;
        }
        const std::optional<std::span<std::byte>> span = packet->append(size);
        if (!span) {
            mark_retry(c);  // cannot happen: size is at most the MSS and a packet holds a whole frame
            return std::nullopt;
        }
        c.prepared_      = std::move(*packet);  // the span points into the block, which does not move
        c.prepared_size_ = size;
        return span;
    }

    /**
     * One PSH+ACK segment of the first `count` prepared bytes; `commit(0)` discards the preparation. A refusal
     * returns the packet to the pool, leaves every number as it was, marks a retry hint and returns false. A
     * connection that left Established and CloseWait since the prepare (its preparation already dropped by
     * `set_closed`) refuses the commit with no hint: the events say why.
     */
    template <typename Ip>
    bool Stack<Ip>::commit(ConnectionType& c, const std::size_t count) noexcept {
        const bool sendable = c.state_ == State::Established || c.state_ == State::CloseWait;
        if (!c.prepared_) {
            assert(!sendable && "commit without a prepare");
            if (count == 0) {
                return true;  // nothing left to discard
            }
            ++counters_.commits_refused;
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
        if (!sendable) {
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

}  // namespace aloe::tcp
