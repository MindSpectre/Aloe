#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include <checksum.hpp>
#include <ipv4_checksum.hpp>
#include <rss.hpp>
#include <tcp_stack.hpp>

// The receive path of tcp::Stack: header, checksum, lookup, the handshake, the synchronized states.
namespace aloe::tcp {

    template <typename Ip>
    void Stack<Ip>::on_datagram(Datagram& datagram) noexcept {
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
        ConnectionType& c = connections_[*found];
        switch (c.state_) {
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

    template <typename Ip>
    bool Stack<Ip>::checksum_ok(Datagram& datagram) const noexcept {
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

    template <typename Ip>
    std::uint32_t
    Stack<Ip>::base_hash_of(const Datagram& datagram, const Segment& segment, const FlowKey key) const noexcept {
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

    template <typename Ip>
    void Stack<Ip>::passive_open(Datagram& datagram, const Segment& segment, const FlowKey key) noexcept {
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

    template <typename Ip>
    void Stack<Ip>::receive_syn_sent(ConnectionType& c, Datagram& datagram, Segment segment) noexcept {
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

    template <typename Ip>
    void Stack<Ip>::receive_synchronized(ConnectionType& c, Datagram& datagram, Segment segment) noexcept {
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
            receive_fin(c);
        }
    }

    template <typename Ip>
    bool Stack<Ip>::accept_payload(ConnectionType& c, Datagram& datagram, const Segment& segment) noexcept {
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
    template <typename Ip>
    void Stack<Ip>::apply_ack_policy(ConnectionType& c, const std::size_t accepted) noexcept {
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

    template <typename Ip>
    void Stack<Ip>::receive_fin(ConnectionType& c) noexcept {
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
    template <typename Ip>
    void Stack<Ip>::update_peer_window(ConnectionType& c, const wire::TcpHeader& header) noexcept {
        if (c.wl1_.before(header.sequence) || (c.wl1_ == header.sequence && !c.wl2_.after(header.acknowledgement))) {
            c.snd_wnd_ = header.window;
            c.wl1_     = header.sequence;
            c.wl2_     = header.acknowledgement;
        }
    }

    /// RFC 793's reset for a segment with no connection: with ACK, sequence at its acknowledgement;
    /// without, sequence zero and acknowledgement past everything it occupied.
    template <typename Ip>
    void Stack<Ip>::reply_reset(const Datagram& datagram, const Segment& segment) noexcept {
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

}  // namespace aloe::tcp
