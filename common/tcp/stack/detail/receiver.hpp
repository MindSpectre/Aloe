#pragma once

#include <aloe/device>
#include <aloe/net>
#include <aloe/stream>
#include <aloe/wire>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>
#include <utility>

#include <detail/ack_policy.hpp>
#include <detail/ack_processor.hpp>
#include <detail/control_block.hpp>
#include <detail/lifecycle.hpp>
#include <detail/reassembly_queue.hpp>
#include <detail/receive_window.hpp>
#include <detail/stack_context.hpp>
#include <detail/transmitter.hpp>
#include <flow_table.hpp>
#include <tcp_config.hpp>

namespace aloe::tcp::detail {

    /**
     * @brief What a datagram does to a connection: the header and checksum, the lookup, the handshake, RFC 9293's
     * order on a synchronized connection (PAWS, the sequence check, RST, ACK, payload, FIN), the hold of a segment
     * above a gap and the join when it fills, and `consume`. It owns no state; it drives the block's receive side and
     * reassembly queue, hands an acceptable ACK to the AckProcessor, leaves every change of the connection's state to
     * the Lifecycle and sends through the Transmitter, in place, as each step of the procedure asks.
     */
    template <typename Ip, typename CongestionControl>
    class Receiver {
    public:
        using Packet       = Ip::Packet;
        using ControlBlock = detail::ControlBlock<Ip, CongestionControl>;
        using Context      = StackContext<Ip, CongestionControl>;
        using Lifecycle    = detail::Lifecycle<Ip, CongestionControl>;
        using AckProcessor = detail::AckProcessor<Ip, CongestionControl>;
        using Transmitter  = detail::Transmitter<Ip, CongestionControl>;
        using Datagram     = net::Datagram<Packet>;
        using Sequence     = wire::TcpSequence;

        Receiver(Context& context, Lifecycle& lifecycle, AckProcessor& acks, Transmitter& transmitter) noexcept
            : context_{context},
              lifecycle_{lifecycle},
              acks_{acks},
              transmitter_{transmitter} {
        }

        /// One datagram IP sorted out, at the context's stamp; the packet is moved out when it is held.
        void on_datagram(Datagram& datagram) noexcept {
            ++context_.counters.segments_received;
            const std::span<std::byte> l4               = datagram.l4();
            const std::optional<wire::TcpHeader> header = wire::TcpHeader::parse(l4);
            if (!header) {
                ++context_.counters.dropped_bad_header;
                return;
            }
            Segment segment{.header         = *header,
                            .payload_offset = header->data_offset,
                            .payload_length = static_cast<std::uint16_t>(l4.size() - header->data_offset),
                            .mss            = std::nullopt,
                            .base_hash      = 0,
                            .ts_value       = std::nullopt,
                            .ts_echo        = std::nullopt,
                            .window_scale   = std::nullopt,
                            .sack_permitted = false};
            if (header->data_offset > wire::TcpHeader::size) {
                const auto options = wire::TcpOptions::parse(std::span<const std::byte>{l4}.subspan(
                    wire::TcpHeader::size, header->data_offset - wire::TcpHeader::size));
                if (!options) {
                    ++context_.counters.dropped_bad_header;
                    return;
                }
                if (header->flags.has(wire::TcpFlag::Syn)) {
                    // RFC 9293 and RFC 7323: these options mean something only on a SYN.
                    segment.mss            = options->mss;
                    segment.window_scale   = options->window_scale;
                    segment.sack_permitted = options->sack_permitted;
                }
                segment.ts_value = options->timestamp_value;
                segment.ts_echo  = options->timestamp_echo;
            }
            if (!checksum_ok(datagram)) {
                ++context_.counters.dropped_bad_checksum;
                return;
            }
            const FlowKey key{
                .remote = datagram.source, .remote_port = header->source_port, .local_port = header->destination_port};
            segment.base_hash =
                lifecycle_.table().base_hash(context_.ip.queue().steering(),
                                             device::FlowTuple{.source           = datagram.source,
                                                               .destination      = datagram.destination,
                                                               .source_port      = header->source_port,
                                                               .destination_port = header->destination_port,
                                                               .protocol         = wire::Ipv4Protocol::Tcp},
                                             datagram.packet.rx().rss_hash);
            const std::optional<std::uint32_t> found = lifecycle_.table().find(key, segment.base_hash);
            if (!found) {
                const bool opening = header->flags.has(wire::TcpFlag::Syn) && !header->flags.has(wire::TcpFlag::Ack) &&
                                     !header->flags.has(wire::TcpFlag::Rst);
                if (opening && lifecycle_.table().listening(header->destination_port)) {
                    passive_open(datagram, segment);
                    return;
                }
                ++context_.counters.dropped_no_connection;
                if (!header->flags.has(wire::TcpFlag::Rst)) {
                    transmitter_.reply_reset(datagram.source, segment.header, segment.length());
                }
                return;
            }
            switch (ControlBlock& block = context_.blocks[*found]; block.state) {
                case State::Closed:  // still indexed because not released
                    ++context_.counters.dropped_closed;
                    if (!header->flags.has(wire::TcpFlag::Rst)) {
                        transmitter_.reply_reset(datagram.source, segment.header, segment.length());
                    }
                    return;
                case State::SynSent:
                    receive_syn_sent(block, datagram, segment);
                    return;
                default:
                    receive_synchronized(block, datagram, segment);
                    return;
            }
        }

        /// Advances the held payload's front by `count` bytes, freeing packets as they empty; a window that at least
        /// doubles queues an update (`ReceiveWindow::update_due`).
        void consume(ControlBlock& block, const std::size_t count) noexcept {
            block.receive.consume(context_.pool, count);
            const bool receiving =
                block.state == State::Established || block.state == State::FinWait1 || block.state == State::FinWait2;
            if (!receiving) {
                return;  // the peer sends no more data, or the connection is closed: no offer to make
            }
            if (block.window.update_due(block.receive.next(), block.receive.unread(), slot_free(block))) {
                transmitter_.queue_ack(block);  // an ordinary window update, including the reopening of a zero window
            }
        }

    private:
        /// What the sequence check made of a segment.
        enum class Sequenced : std::uint8_t {
            Refused,   ///< Dropped, counted and answered: it goes no further.
            InOrder,   ///< At RCV.NXT, trimmed to it, or occupying no sequence space inside the window.
            AboveGap,  ///< Payload or a FIN above RCV.NXT, inside the window.
        };

        /// What the receive path parsed from one datagram.
        struct Segment {
            wire::TcpHeader header{};
            std::uint16_t payload_offset = 0;  ///< From the start of `l4()`: the data offset.
            std::uint16_t payload_length = 0;
            std::optional<std::uint16_t> mss;  ///< From the options of a SYN.
            std::uint32_t base_hash = 0;
            std::optional<std::uint32_t> ts_value{};     ///< RFC 7323 TSval, when the segment carries the option.
            std::optional<std::uint32_t> ts_echo{};      ///< RFC 7323 TSecr, with it.
            std::optional<std::uint8_t> window_scale{};  ///< From the options of a SYN, as written.
            bool sack_permitted = false;                 ///< From the options of a SYN.

            /// Sequence space the segment occupies: payload, SYN and FIN.
            [[nodiscard]] std::uint32_t length() const noexcept {
                return payload_length + (header.flags.has(wire::TcpFlag::Syn) ? 1U : 0U) +
                       (header.flags.has(wire::TcpFlag::Fin) ? 1U : 0U);
            }
        };

        /// The connection holds fewer packets than its cap: the edge may move.
        [[nodiscard]] bool slot_free(const ControlBlock& block) const noexcept {
            return block.held_segments() < context_.config.receive_segments;
        }

        /// Opens the receive side once the handshake settled the peer's MSS and the options: RCV.NXT, then the window
        /// with the budget counted in `mss()`. `scaling` is whether both ends offered window scaling: the window then
        /// uses our shift, and the budget is what the field states under it.
        void open_receive(ControlBlock& block, const Sequence next, const bool scaling) noexcept {
            block.receive.open(next);
            const std::optional<std::uint8_t> scale = scaling ? std::optional{context_.window_shift} : std::nullopt;
            const std::uint32_t budget              = ReceiveWindow::budget_of(context_.config.receive_segments,
                                                                               block.mss(),
                                                                               context_.config.receive_window,
                                                                               scale.value_or(std::uint8_t{0}));
            block.window.open(next, budget, block.mss(), scale);
        }

        /// The peer's SYN or SYN-ACK answered the options: SACK-permitted is in use when both ends sent it. The peer's
        /// window scale when both ends offered scaling, at most 14 (RFC 7323); nothing when scaling is not in use.
        [[nodiscard]] std::optional<std::uint8_t> answer_options(ControlBlock& block,
                                                                 const Segment& segment) const noexcept {
            block.sack.answer(segment.sack_permitted);
            const bool scaling = context_.config.window_scaling && segment.window_scale.has_value();
            if (!scaling) {
                return std::nullopt;
            }
            return std::min(*segment.window_scale, wire::TcpOptions::max_window_scale);
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

        void passive_open(Datagram& datagram, const Segment& segment) noexcept {
            if (transmitter_.nodes_available() == 0) {
                ++context_.counters.dropped_no_node;  // no retransmission-queue node for the SYN-ACK
                return;
            }
            ControlBlock* opened = lifecycle_.accept_slot(
                Endpoint{.address = context_.ip.address(), .port = segment.header.destination_port},
                Endpoint{.address = datagram.source, .port = segment.header.source_port},
                segment.base_hash);
            if (opened == nullptr) {
                ++context_.counters.dropped_table_full;
                return;
            }
            ControlBlock& block = *opened;
            block.send.peer_mss = segment.mss.value_or(default_peer_mss);
            block.send.wnd      = segment.header.window;
            block.send.wl1      = segment.header.sequence;
            block.send.wl2      = Sequence{};
            block.timestamps.answer(segment.ts_value, context_.now);
            const std::optional<std::uint8_t> peer_shift = answer_options(block, segment);
            block.send.peer_shift                        = peer_shift.value_or(std::uint8_t{0});
            // `mss()` is final: the window opens, and the SYN-ACK carries its first offer.
            open_receive(block, segment.header.sequence + 1, peer_shift.has_value());
            block.timestamps.sent_ack(block.receive.next());  // what the SYN-ACK acknowledges, as Linux's rcv_wup
            // Arms the timer on a send or an ARP wait; a refused send leaves the SYN-ACK to the RTO.
            std::ignore = transmitter_.queue_control(
                block, wire::TcpFlags{wire::TcpFlag::Syn, wire::TcpFlag::Ack}, block.send.iss);
            if (!block.timer.armed()) {
                transmitter_.arm_retransmit(block);
            }
        }

        void receive_syn_sent(ControlBlock& block, Datagram& datagram, Segment segment) noexcept {
            const wire::TcpHeader& header = segment.header;
            if (header.flags.has(wire::TcpFlag::Ack) && header.acknowledgement != block.send.nxt) {
                ++context_.counters.dropped_unexpected;  // not an answer to our SYN
                return;
            }
            if (header.flags.has(wire::TcpFlag::Rst)) {
                if (!header.flags.has(wire::TcpFlag::Ack)) {
                    ++context_.counters.dropped_unexpected;
                    return;
                }
                lifecycle_.peer_reset(block);
                return;
            }
            if (!header.flags.has(wire::TcpFlag::Syn) || !header.flags.has(wire::TcpFlag::Ack)) {
                ++context_.counters.dropped_unexpected;  // a simultaneous open is out of scope
                return;
            }
            block.timestamps.answer(segment.ts_value, context_.now);  // our offer, answered
            const std::optional<std::uint8_t> peer_shift = answer_options(block, segment);
            // Frees the SYN's entry: a sample, and the timer stopped with the queue empty.
            acks_.handshake_acknowledged(block, header.acknowledgement, segment.ts_echo);
            block.send.peer_mss = segment.mss.value_or(default_peer_mss);
            block.send.synchronize(header);  // the SYN-ACK's field, unscaled
            lifecycle_.connected(block);
            // Established with the peer's MSS stored: `mss()` is final, and our first ACK carries the first offer.
            open_receive(block, header.sequence + 1, peer_shift.has_value());
            block.timestamps.sent_ack(block.receive.next());  // as Linux's rcv_wup: data on the SYN-ACK may update it
            acks_.update_probing(block);  // a SYN-ACK offering no window starts probing as any ACK does
            // Deferred: a commit on the Connected event completes the handshake with data.
            transmitter_.queue_ack(block);
            if (segment.payload_length > 0 || header.flags.has(wire::TcpFlag::Fin)) {
                segment.header.sequence = block.receive.next();
                segment.header.flags    = wire::TcpFlags::from_raw(
                    static_cast<std::uint8_t>(header.flags.raw() & ~std::to_underlying(wire::TcpFlag::Syn)));
                receive_synchronized(block, datagram, segment);
            }
            // The peer's shift applies from the next segment: the ACK step above read this one's field, which a
            // SYN-ACK states unscaled (RFC 7323), as the window update.
            block.send.peer_shift = peer_shift.value_or(std::uint8_t{0});
        }

        /// A segment on a synchronized connection, in RFC 9293's order; each step may end it.
        void receive_synchronized(ControlBlock& block, Datagram& datagram, Segment segment) noexcept {
            const bool rst         = segment.header.flags.has(wire::TcpFlag::Rst);
            const Sequence arrived = segment.header.sequence;  // SEG.SEQ before any trim, for the TS.Recent rule

            // 0. PAWS (RFC 7323), before the sequence check as Linux's tcp_validate_incoming: a timestamp older than
            // TS.Recent is an old duplicate, acknowledged at once and dropped. A RST is exempt; a segment without the
            // option goes on, as Linux lets it.
            if (block.timestamps.rejects(segment.ts_value, rst, context_.now)) {
                ++context_.counters.dropped_paws;
                ack_now(block, Edge::Restate);
                return;
            }

            // 1. Sequence check against [rcv_nxt, the offered edge).
            const Sequenced sequenced = accept_sequence(block, segment);
            if (sequenced == Sequenced::Refused) {
                return;
            }
            block.timestamps.update(segment.ts_value, arrived, context_.now);  // acceptable, trimmed or not

            // 2. RST, and a SYN, which a synchronized connection does not take.
            if (rst) {
                lifecycle_.peer_reset(block);
                return;
            }
            if (segment.header.flags.has(wire::TcpFlag::Syn)) {
                ++context_.counters.dropped_unexpected;  // a SYN inside the window of a synchronized connection
                return;
            }

            // 3. ACK, whatever the segment's place: one above a gap moves the send side like any other.
            if (!receive_ack(block, segment)) {
                return;
            }
            if (sequenced == Sequenced::AboveGap) {
                receive_above_gap(block, datagram, segment);
                return;
            }

            // 4. Payload.
            if (segment.payload_length > 0 && !receive_payload(block, datagram, segment)) {
                return;
            }

            // 5. FIN, in order: its sequence is rcv_nxt after the payload.
            if (segment.header.flags.has(wire::TcpFlag::Fin)) {
                if (block.state == State::CloseWait || block.state == State::Closing || block.state == State::LastAck) {
                    // A second FIN: the peer's sequence space ended at the first.
                    ++context_.counters.dropped_unexpected;
                    ack_now(block, Edge::Restate);
                    return;
                }
                receive_fin(block);
            }
        }

        /**
         * The sequence check against `[rcv_nxt, offered edge)`. A segment entirely before RCV.NXT or past the edge is
         * dropped with an immediate ACK that restates the edge, unless it is a RST. One that overlaps RCV.NXT loses its
         * old prefix, a SYN with it. One whose payload or FIN starts above RCV.NXT inside the window is above a gap; a
         * segment that occupies no sequence space never is. A duplicate SYN in SynReceived gets the SYN-ACK again, from
         * its entry.
         */
        [[nodiscard]] Sequenced accept_sequence(ControlBlock& block, Segment& segment) noexcept {
            wire::TcpHeader& header    = segment.header;
            const bool rst             = header.flags.has(wire::TcpFlag::Rst);
            const Sequence next        = block.receive.next();
            const Sequence edge        = block.window.edge();
            const std::uint32_t window = block.window.window(next);
            const Sequence sequence    = header.sequence;
            const Sequence end         = sequence + segment.length();
            const auto drop            = [this, &block, rst](std::uint64_t& counter) {
                ++counter;
                if (!rst) {
                    ack_now(block, Edge::Restate);
                }
                return Sequenced::Refused;
            };
            if (segment.length() == 0) {
                const bool acceptable =
                    window == 0 ? sequence == next : !sequence.before(next) && sequence.before(edge);
                return acceptable ? Sequenced::InOrder : drop(context_.counters.dropped_out_of_window);
            }
            if (!end.after(next)) {  // entirely before rcv_nxt
                if (block.state == State::SynReceived && header.flags.has(wire::TcpFlag::Syn) &&
                    !header.flags.has(wire::TcpFlag::Ack)) {
                    // A duplicate SYN: the SYN-ACK again, from its entry.
                    std::ignore = transmitter_.resend_head(block);
                    return Sequenced::Refused;
                }
                return drop(context_.counters.dropped_duplicate);
            }
            if (sequence.after(next)) {  // a gap before it
                if (window == 0 || end.after(edge)) {
                    return drop(context_.counters.dropped_out_of_window);
                }
                return Sequenced::AboveGap;
            }
            if (sequence.before(next)) {  // overlap: the old prefix is already ours
                std::uint32_t trim = next.value - sequence.value;
                if (header.flags.has(wire::TcpFlag::Syn)) {
                    header.flags = wire::TcpFlags::from_raw(
                        static_cast<std::uint8_t>(header.flags.raw() & ~std::to_underlying(wire::TcpFlag::Syn)));
                    --trim;
                }
                assert(trim <= segment.payload_length);
                segment.payload_offset = static_cast<std::uint16_t>(segment.payload_offset + trim);
                segment.payload_length = static_cast<std::uint16_t>(segment.payload_length - trim);
                header.sequence        = next;
            }
            if (window == 0 || end.after(edge)) {
                return drop(context_.counters.dropped_out_of_window);
            }
            return Sequenced::InOrder;
        }

        /**
         * The ACK. In SynReceived it completes the passive open. What an acceptable ACK does to the send side is the
         * AckProcessor's (`AckProcessor::on_ack`); this step publishes what it reports, `Acked` and `Writable`, and
         * moves the close on once our FIN is acknowledged. False when the segment goes no further: no ACK, an ACK of
         * what was never sent, or a connection that finished.
         */
        [[nodiscard]] bool receive_ack(ControlBlock& block, const Segment& segment) noexcept {
            const wire::TcpHeader& header = segment.header;
            if (!header.flags.has(wire::TcpFlag::Ack)) {
                ++context_.counters.dropped_unexpected;
                return false;
            }
            const Sequence ack = header.acknowledgement;
            if (block.state == State::SynReceived) {
                if (ack != block.send.nxt) {
                    ++context_.counters.dropped_unexpected;
                    return false;
                }
                // Frees the SYN-ACK's entry: a sample, and the timer stopped with the queue empty.
                acks_.handshake_acknowledged(block, ack, segment.ts_echo);
                lifecycle_.accepted(block);
                // The same segment may carry data or a FIN: keep going.
            }
            if (ack.after(block.send.nxt)) {
                ++context_.counters.dropped_unexpected;  // acknowledges what was never sent
                ack_now(block, Edge::Restate);
                return false;
            }
            const typename AckProcessor::Outcome outcome =
                acks_.on_ack(block, header, segment.payload_length, segment.ts_echo);
            if (outcome.advanced) {
                lifecycle_.raise(block, stream::Event::Acked);
                if (block.send.fin_sent && ack == block.send.nxt) {
                    lifecycle_.fin_acknowledged(block);
                }
            }
            if (outcome.writable) {
                lifecycle_.raise(block, stream::Event::Writable);
            }
            lifecycle_.maybe_finish(block);
            return block.state != State::Closed;
        }

        /**
         * In-order payload, held for the application and acknowledged by the ACK policy. Refused, with an immediate
         * ACK that reports the last byte accepted and restates the edge, past the peer's FIN, past the per-connection
         * cap of held packets or with the shard's pool empty; the FIN behind refused data is not consumed. A connection
         * that holds something above a gap takes `receive_below_held` instead. False when refused.
         */
        [[nodiscard]] bool receive_payload(ControlBlock& block, Datagram& datagram, const Segment& segment) noexcept {
            if (block.receive.closed()) {  // the peer's sequence space ended at its FIN, whatever state we are in
                ++context_.counters.dropped_unexpected;
                ack_now(block, Edge::Restate);
                return false;
            }
            if (block.reassembly.holds()) {  // the one question a clean stream asks
                return receive_below_held(block, datagram, segment);
            }
            if (!slot_free(block)) {
                ++context_.counters.dropped_no_slot;
                ack_now(block, Edge::Restate);
                return false;
            }
            const auto offset =
                static_cast<std::uint16_t>(datagram.l3_offset + datagram.l3_length + segment.payload_offset);
            if (!block.receive.hold(context_.pool, datagram.packet, offset, segment.payload_length)) {
                ++context_.counters.dropped_no_node;
                ack_now(block, Edge::Restate);
                return false;
            }
            lifecycle_.raise(block, stream::Event::Readable);
            // A full-sized segment is the connection's `mss()`: the peer's MSS or ours, less the timestamp option.
            if (block.ack_policy.on_data(segment.payload_length, block.mss(), block.receive.next()) ==
                AckPolicy::Ack::Now) {
                ack_now(block);
            } else {
                transmitter_.queue_ack(block);
            }
            return true;
        }

        /**
         * In-order payload on a connection that holds something above a gap. Data that ends past a remembered FIN is
         * refused as data after the peer's FIN is, so RCV.NXT never passes it. At the cap or with the pool empty the
         * segment takes the node of the highest segment held above (`make_room`); with none above it, it is refused as
         * on a clean stream. Once held, the run that now follows RCV.NXT joins, `Readable` covers all of it and the ACK
         * goes at once; a segment that carries the peer's FIN does not join, since its FIN step releases what is held
         * above. False when refused.
         */
        [[nodiscard]] bool
        receive_below_held(ControlBlock& block, Datagram& datagram, const Segment& segment) noexcept {
            const Sequence next                    = block.receive.next();
            const std::optional<Sequence> peer_fin = block.reassembly.fin();
            if (peer_fin && (next + segment.payload_length).after(*peer_fin)) {
                ++context_.counters.dropped_unexpected;
                ack_now(block, Edge::Restate);
                return false;
            }
            if (!slot_free(block) && !make_room(block, next)) {
                ++context_.counters.dropped_no_slot;
                ack_now(block, Edge::Restate);
                return false;
            }
            const auto offset =
                static_cast<std::uint16_t>(datagram.l3_offset + datagram.l3_length + segment.payload_offset);
            const auto hold = [this, &block, &datagram, &segment, offset] {
                return block.receive.hold(context_.pool, datagram.packet, offset, segment.payload_length);
            };
            if (!hold() && (!make_room(block, next) || !hold())) {
                ++context_.counters.dropped_no_node;
                ack_now(block, Edge::Restate);
                return false;
            }
            lifecycle_.raise(block, stream::Event::Readable);  // once, for the segment and any run it joins
            if (segment.header.flags.has(wire::TcpFlag::Fin)) {
                if (block.ack_policy.on_data(segment.payload_length, block.mss(), block.receive.next()) ==
                    AckPolicy::Ack::Now) {
                    ack_now(block);
                } else {
                    transmitter_.queue_ack(block);
                }
                return true;
            }
            join(block);
            // The policy still counts the segment and clears a remembered gap end RCV.NXT reached; the answer is now.
            std::ignore = block.ack_policy.on_data(segment.payload_length, block.mss(), block.receive.next());
            if (block.receive.closed()) {
                // The join reached a remembered FIN, whose step sent its ACK or queued it: a queued one goes now. The
                // ACK that left may have ended the connection.
                if (block.state != State::Closed && transmitter_.ack_pending(block)) {
                    ack_now(block);
                }
                return true;
            }
            ack_now(block);
            return true;
        }

        /**
         * The payload and FIN of a segment above a gap, after its RST, SYN and ACK were dealt with: the payload goes on
         * the reassembly queue, the FIN is remembered, and the segment is answered at once with the edge restated.
         * Data after the peer's FIN, or at or past a remembered one, is refused. At the cap or with the pool empty the
         * segment takes the highest held node when it starts below it (`make_room`); otherwise it is refused, and its
         * end is remembered so the data that fills the gap is acknowledged at once.
         */
        void receive_above_gap(ControlBlock& block, Datagram& datagram, const Segment& segment) noexcept {
            const Sequence sequence                = segment.header.sequence;
            const Sequence data_end                = sequence + segment.payload_length;
            const std::optional<Sequence> peer_fin = block.reassembly.fin();
            if (block.receive.closed() || (peer_fin && data_end.after(*peer_fin))) {
                ++context_.counters.dropped_unexpected;
                ack_now(block, Edge::Restate);
                return;
            }
            const auto refuse = [this, &block, &segment, sequence] {
                ++context_.counters.dropped_out_of_order;
                block.ack_policy.gap(sequence + segment.length());
                ack_now(block, Edge::Restate);
            };
            bool held = segment.payload_length == 0;
            if (segment.payload_length > 0) {
                // At the cap or with the pool empty the queue first places what needs no new node: a duplicate, or a
                // newcomer that takes the slot of a node it replaces. Only a newcomer that needs one asks `make_room`.
                const bool room = slot_free(block) && context_.pool.available() > 0;
                const auto offset =
                    static_cast<std::uint16_t>(datagram.l3_offset + datagram.l3_length + segment.payload_offset);
                const auto insert = [this, &block, &datagram, &segment, offset, sequence](const bool fresh_node) {
                    return block.reassembly
                        .insert(context_.pool, datagram.packet, offset, segment.payload_length, sequence, fresh_node)
                        .placed;
                };
                auto placed = insert(room);
                if (placed == ReassemblyQueue<Packet>::Placed::NoNode && !room && make_room(block, sequence)) {
                    placed = insert(true);
                }
                switch (placed) {
                    case ReassemblyQueue<Packet>::Placed::Held:
                        ++context_.counters.out_of_order_held;
                        held = true;
                        break;
                    case ReassemblyQueue<Packet>::Placed::Duplicate:
                        ++context_.counters.dropped_duplicate;  // the packet stays the caller's and goes as dropped
                        break;
                    case ReassemblyQueue<Packet>::Placed::NoNode:
                        refuse();
                        return;
                }
            }
            // A FIN below a remembered one replaces it; one above it was refused with the data past it, above.
            if (held && segment.header.flags.has(wire::TcpFlag::Fin)) {
                block.reassembly.remember_fin(data_end);
            }
            ack_now(block, Edge::Restate);
        }

        /// After an in-order segment was held with something held above: the run that now follows RCV.NXT joins the
        /// in-order chain, and a remembered FIN it reaches is taken.
        void join(ControlBlock& block) noexcept {
            auto run = block.reassembly.take_run(context_.pool, block.receive.next());
            block.receive.append(context_.pool, run.chain, run.bytes);
            if (block.reassembly.fin() == block.receive.next()) {
                receive_fin(block);
            }
        }

        /// The connection is at its cap, or the pool is empty, and a segment that starts at `first` needs a node: the
        /// lower sequence wins. True when the highest out-of-order node was released for it. The one place that
        /// decides what gives way at the cap.
        [[nodiscard]] bool make_room(ControlBlock& block, const Sequence first) noexcept {
            const std::optional<wire::TcpSackBlock> highest = block.reassembly.highest();
            if (!highest || !highest->first.after(first)) {
                return false;
            }
            const wire::TcpSackBlock released = block.reassembly.release_highest(context_.pool);
            ++context_.counters.out_of_order_released;
            block.ack_policy.gap(released.end);  // as a refused segment's end: the data below it is acked at once
            return true;
        }

        /// The peer's FIN, in order: the receive side ends, what is held above it is released, the Lifecycle moves
        /// the state on and says when the ACK goes; flush retries an immediate one that was refused.
        void receive_fin(ControlBlock& block) noexcept {
            block.receive.take_fin();
            block.reassembly.release(context_.pool);  // nothing above the peer's FIN is kept
            if (lifecycle_.peer_closed(block) == AckPolicy::Ack::Now) {
                ack_now(block);
            } else {
                transmitter_.queue_ack(block);
            }
        }

        /// An immediate ACK, stating the edge `edge` names: queued through IP before the next segment is processed; a
        /// refusal leaves an ordinary ACK pending for flush. When the close only waited for this ACK, Closed follows
        /// it, whichever segment prompted it.
        void ack_now(ControlBlock& block, const Edge edge = Edge::Offer) noexcept {
            if (transmitter_.send_pure_ack(block, edge)) {
                lifecycle_.maybe_finish(block);
            } else {
                transmitter_.queue_ack(block);
            }
        }

        Context& context_;
        Lifecycle& lifecycle_;
        AckProcessor& acks_;
        Transmitter& transmitter_;
    };

}  // namespace aloe::tcp::detail
