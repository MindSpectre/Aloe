#pragma once

#include <algorithm>
#include <aloe/core>
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

#include <congestion_control.hpp>
#include <detail/ack_policy.hpp>
#include <detail/control_block.hpp>
#include <detail/lifecycle.hpp>
#include <detail/loss_recovery.hpp>
#include <detail/stack_context.hpp>
#include <detail/transmitter.hpp>
#include <flow_table.hpp>
#include <tcp_config.hpp>
#include <tcp_fwd.hpp>

namespace aloe::tcp::detail {

    /**
     * @brief What a datagram does to a connection: the header and checksum, the lookup, the handshake, RFC 9293's
     * order on a synchronized connection (PAWS, the sequence check, RST, ACK with loss recovery, payload, FIN), and
     * `consume`. It owns no state; it drives the block's sequence numbers and queues, leaves every change of the
     * connection's state to the Lifecycle and sends through the Transmitter, in place, as each step of the procedure
     * asks.
     */
    template <typename Ip, typename CongestionControl>
    class Receiver {
    public:
        using Packet      = Ip::Packet;
        using Block       = ControlBlock<Ip, CongestionControl>;
        using Context     = StackContext<Ip, CongestionControl>;
        using Lifecycle   = detail::Lifecycle<Ip, CongestionControl>;
        using Transmitter = detail::Transmitter<Ip, CongestionControl>;
        using Datagram    = net::Datagram<Packet>;
        using Sequence    = wire::TcpSequence;

        Receiver(Context& context, Lifecycle& lifecycle, Transmitter& transmitter) noexcept
            : context_{context},
              lifecycle_{lifecycle},
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
                            .ts_echo        = std::nullopt};
            if (header->data_offset > wire::TcpHeader::size) {
                const auto options = wire::TcpOptions::parse(std::span<const std::byte>{l4}.subspan(
                    wire::TcpHeader::size, header->data_offset - wire::TcpHeader::size));
                if (!options) {
                    ++context_.counters.dropped_bad_header;
                    return;
                }
                if (header->flags.has(wire::TcpFlag::Syn)) {
                    segment.mss = options->mss;  // RFC 9293: the option means something only on a SYN
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
            switch (Block& block = context_.blocks[*found]; block.state) {
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

        /// Advances the held payload's front by `count` bytes, freeing packets as they empty; a reopened edge queues
        /// an update.
        void consume(Block& block, const std::size_t count) noexcept {
            block.receive.consume(context_.pool, count);
            const bool receiving =
                block.state == State::Established || block.state == State::FinWait1 || block.state == State::FinWait2;
            if (!receiving) {
                return;  // the peer sends no more data, or the connection is closed: no offer to make
            }
            if (transmitter_.offer_of(block) != block.receive.offered()) {
                transmitter_.queue_ack(block);  // an ordinary window update, including the reopening of a zero window
            }
        }

    private:
        /// What the receive path parsed from one datagram.
        struct Segment {
            wire::TcpHeader header{};
            std::uint16_t payload_offset = 0;  ///< From the start of `l4()`: the data offset.
            std::uint16_t payload_length = 0;
            std::optional<std::uint16_t> mss;  ///< From the options of a SYN.
            std::uint32_t base_hash = 0;
            std::optional<std::uint32_t> ts_value{};  ///< RFC 7323 TSval, when the segment carries the option.
            std::optional<std::uint32_t> ts_echo{};   ///< RFC 7323 TSecr, with it.

            /// Sequence space the segment occupies: payload, SYN and FIN.
            [[nodiscard]] std::uint32_t length() const noexcept {
                return payload_length + (header.flags.has(wire::TcpFlag::Syn) ? 1U : 0U) +
                       (header.flags.has(wire::TcpFlag::Fin) ? 1U : 0U);
            }
        };

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
            Block* opened = lifecycle_.accept_slot(
                Endpoint{.address = context_.ip.address(), .port = segment.header.destination_port},
                Endpoint{.address = datagram.source, .port = segment.header.source_port},
                segment.base_hash);
            if (opened == nullptr) {
                ++context_.counters.dropped_table_full;
                return;
            }
            Block& block = *opened;
            // The initial offer, carried by the SYN-ACK.
            block.receive.open(segment.header.sequence + 1, context_.byte_budget);
            block.send.peer_mss = segment.mss.value_or(default_peer_mss);
            block.send.wnd      = segment.header.window;
            block.send.wl1      = segment.header.sequence;
            block.send.wl2      = Sequence{};
            block.timestamps.answer(segment.ts_value, context_.now);
            block.timestamps.sent_ack(block.receive.next());  // what the SYN-ACK acknowledges, as Linux's rcv_wup
            // Arms the timer on a send or an ARP wait; a refused send leaves the SYN-ACK to the RTO.
            std::ignore = transmitter_.queue_control(
                block, wire::TcpFlags{wire::TcpFlag::Syn, wire::TcpFlag::Ack}, block.send.iss);
            if (!block.timer.armed()) {
                transmitter_.arm_retransmit(block);
            }
        }

        void receive_syn_sent(Block& block, Datagram& datagram, Segment segment) noexcept {
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
            const typename Transmitter::Freed freed =
                transmitter_.acknowledge(block, header.acknowledgement);  // frees the SYN's entry
            block.send.una = header.acknowledgement;
            on_acknowledged(block, freed, segment.ts_echo);  // a sample, and the timer stopped with the queue empty
            // The initial offer, carried by our first ACK.
            block.receive.open(header.sequence + 1, context_.byte_budget);
            block.timestamps.sent_ack(block.receive.next());  // as Linux's rcv_wup: data on the SYN-ACK may update it
            block.send.peer_mss = segment.mss.value_or(default_peer_mss);
            block.send.synchronize(header);
            lifecycle_.connected(block);
            update_probing(block);  // a SYN-ACK offering no window starts probing as any ACK does
            // Deferred: a commit on the Connected event completes the handshake with data.
            transmitter_.queue_ack(block);
            if (segment.payload_length > 0 || header.flags.has(wire::TcpFlag::Fin)) {
                segment.header.sequence = block.receive.next();
                segment.header.flags    = wire::TcpFlags::from_raw(
                    static_cast<std::uint8_t>(header.flags.raw() & ~std::to_underlying(wire::TcpFlag::Syn)));
                receive_synchronized(block, datagram, segment);
            }
        }

        /// A segment on a synchronized connection, in RFC 9293's order; each step may end it.
        void receive_synchronized(Block& block, Datagram& datagram, Segment segment) noexcept {
            const bool rst         = segment.header.flags.has(wire::TcpFlag::Rst);
            const Sequence arrived = segment.header.sequence;  // SEG.SEQ before any trim, for the TS.Recent rule

            // 0. PAWS (RFC 7323), before the sequence check as Linux's tcp_validate_incoming: a timestamp older than
            // TS.Recent is an old duplicate, acknowledged at once and dropped. A RST is exempt; a segment without the
            // option goes on, as Linux lets it.
            if (block.timestamps.rejects(segment.ts_value, rst, context_.now)) {
                ++context_.counters.dropped_paws;
                ack_now(block);
                return;
            }

            // 1. Sequence check against [rcv_nxt, the offered edge).
            if (!accept_sequence(block, segment)) {
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

            // 3. ACK.
            if (!receive_ack(block, segment)) {
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
                    ack_now(block);
                    return;
                }
                receive_fin(block);
            }
        }

        /**
         * The sequence check against `[rcv_nxt, offered edge)`. A segment entirely before RCV.NXT, past a gap or past
         * the edge is dropped with an immediate ACK, unless it is a RST; data dropped past a gap is remembered, so the
         * data that fills it is acknowledged at once. One that overlaps RCV.NXT loses its old prefix, a SYN with it. A
         * duplicate SYN in SynReceived gets the SYN-ACK again, from its entry. False when the segment goes no further.
         */
        [[nodiscard]] bool accept_sequence(Block& block, Segment& segment) noexcept {
            wire::TcpHeader& header    = segment.header;
            const bool rst             = header.flags.has(wire::TcpFlag::Rst);
            const Sequence next        = block.receive.next();
            const Sequence edge        = block.receive.offered();
            const std::uint32_t window = block.receive.window();
            const Sequence sequence    = header.sequence;
            const Sequence end         = sequence + segment.length();
            const auto drop            = [this, &block, rst](std::uint64_t& counter) {
                ++counter;
                if (!rst) {
                    ack_now(block);
                }
                return false;
            };
            if (segment.length() == 0) {
                const bool acceptable =
                    window == 0 ? sequence == next : !sequence.before(next) && sequence.before(edge);
                return acceptable || drop(context_.counters.dropped_out_of_window);
            }
            if (!end.after(next)) {  // entirely before rcv_nxt
                if (block.state == State::SynReceived && header.flags.has(wire::TcpFlag::Syn) &&
                    !header.flags.has(wire::TcpFlag::Ack)) {
                    // A duplicate SYN: the SYN-ACK again, from its entry.
                    std::ignore = transmitter_.resend_head(block);
                    return false;
                }
                return drop(context_.counters.dropped_duplicate);
            }
            if (sequence.after(next)) {  // a gap before it
                block.ack_policy.gap(end);
                return drop(context_.counters.dropped_out_of_order);
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
            return true;
        }

        /**
         * The ACK. In SynReceived it completes the passive open. An ACK of new data frees what it covers, samples the
         * round trip and drives loss recovery (`acknowledge_data`); our FIN acknowledged moves the close on. A
         * duplicate ACK, of something in flight, is judged against the window as it stands, before this segment
         * updates it. RFC 793 updates the window only when SND.UNA <= SEG.ACK <= SND.NXT: a stale ACK carries a stale
         * window. False when the segment goes no further: no ACK, an ACK of what was never sent, or a connection that
         * finished.
         */
        [[nodiscard]] bool receive_ack(Block& block, const Segment& segment) noexcept {
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
                const typename Transmitter::Freed freed =
                    transmitter_.acknowledge(block, ack);  // frees the SYN-ACK's entry
                block.send.una = ack;
                on_acknowledged(block, freed, segment.ts_echo);  // a sample, and the timer stopped with the queue empty
                lifecycle_.accepted(block);
                // The same segment may carry data or a FIN: keep going.
            }
            if (ack.after(block.send.nxt)) {
                ++context_.counters.dropped_unexpected;  // acknowledges what was never sent
                ack_now(block);
                return false;
            }
            const std::size_t writable_before = transmitter_.writable(block);
            const bool duplicate              = segment.payload_length == 0 && !header.flags.has(wire::TcpFlag::Syn) &&
                                                !header.flags.has(wire::TcpFlag::Fin) && ack == block.send.una &&
                                                header.window == block.send.wnd && transmitter_.in_flight(block);
            if (ack.after(block.send.una)) {
                acknowledge_data(block, ack, segment.ts_echo);
                lifecycle_.raise(block, stream::Event::Acked);
                if (block.send.fin_sent && ack == block.send.nxt) {
                    lifecycle_.fin_acknowledged(block);
                }
            } else if (duplicate) {
                on_duplicate_ack(block, ack);
            }
            if (!ack.before(block.send.una)) {
                block.send.update_window(header);
                update_probing(block);  // a window that opened ends probing; the comparison below raises Writable
                send_unsent(block);     // and sends the FIN a close left waiting for it
            }
            if (transmitter_.writable(block) > writable_before) {
                lifecycle_.raise(block, stream::Event::Writable);
            }
            lifecycle_.maybe_finish(block);
            return block.state != State::Closed;
        }

        /**
         * In-order payload, held for the application and acknowledged by the ACK policy. Refused, with an immediate
         * ACK that reports the last byte accepted, past the peer's FIN, past the per-connection cap of held packets
         * or with the shard's pool empty; the FIN behind refused data is not consumed. False when refused.
         */
        [[nodiscard]] bool receive_payload(Block& block, Datagram& datagram, const Segment& segment) noexcept {
            if (block.receive.closed()) {  // the peer's sequence space ended at its FIN, whatever state we are in
                ++context_.counters.dropped_unexpected;
                ack_now(block);
                return false;
            }
            if (block.receive.segments() >= context_.config.receive_segments) {
                ++context_.counters.dropped_no_slot;
                ack_now(block);
                return false;
            }
            const auto offset =
                static_cast<std::uint16_t>(datagram.l3_offset + datagram.l3_length + segment.payload_offset);
            if (!block.receive.hold(context_.pool, datagram.packet, offset, segment.payload_length)) {
                ++context_.counters.dropped_no_node;
                ack_now(block);
                return false;
            }
            lifecycle_.raise(block, stream::Event::Readable);
            // A full-sized segment is our MSS less the timestamp option a timestamped peer puts in every segment.
            const std::size_t full_size = context_.mss - block.timestamps.option_size();
            if (block.ack_policy.on_data(segment.payload_length, full_size, block.receive.next()) ==
                AckPolicy::Ack::Now) {
                ack_now(block);
            } else {
                transmitter_.queue_ack(block);
            }
            return true;
        }

        /// The peer's FIN, in order: the receive side ends, the Lifecycle moves the state on and says when the ACK
        /// goes; flush retries an immediate one that was refused.
        void receive_fin(Block& block) noexcept {
            block.receive.take_fin();
            if (lifecycle_.peer_closed(block) == AckPolicy::Ack::Now) {
                ack_now(block);
            } else {
                transmitter_.queue_ack(block);
            }
        }

        /**
         * The end of an acceptable ACK, after its window update: the peer answered, so the probes' count and stamp
         * clear (`ZeroWindowProbe::answered`). A window that opened ends probing; a zero window with nothing in flight
         * starts it: in Established or CloseWait whether or not the application has data, and in any state while a
         * FIN the window did not cover waits in the queue (`close`). A head waiting for the next hop is the ARP poll's.
         */
        void update_probing(Block& block) noexcept {
            block.probe.answered();
            if (block.probing()) {
                if (block.send.wnd > 0) {
                    transmitter_.disarm(block);
                }
                return;
            }
            if (block.send.wnd == 0 && !transmitter_.in_flight(block) && block.timer_kind != TimerKind::ArpPoll &&
                (!block.queue.empty() || block.state == State::Established || block.state == State::CloseWait)) {
                transmitter_.start_probing(block);
            }
        }

        /// The head that never left, the FIN of a close the peer's window did not cover: sent once the window covers
        /// it, as Linux's `tcp_push_pending_frames` on an ACK. A head waiting for the next hop is the ARP poll's. The
        /// send arms the RTO (`transmit_entry`); a refused one leaves the RTO to try again, as `close` does.
        void send_unsent(Block& block) noexcept {
            if (block.queue.empty() || transmitter_.in_flight(block) || block.timer_kind == TimerKind::ArpPoll) {
                return;
            }
            if (const typename Transmitter::SentNode& head = transmitter_.head(block);
                !block.send.covers(head.first + head.length)) {
                return;
            }
            std::ignore = transmitter_.transmit_entry(block, block.queue.head());
            if (!block.timer.armed()) {
                transmitter_.arm_retransmit(block);
            }
        }

        /// An immediate ACK: queued through IP before the next segment is processed; a refusal leaves it pending. When
        /// the close only waited for this ACK, Closed follows it, whichever segment prompted it.
        void ack_now(Block& block) noexcept {
            if (transmitter_.send_pure_ack(block)) {
                lifecycle_.maybe_finish(block);
            } else {
                transmitter_.queue_ack(block);
            }
        }

        /**
         * SND.UNA moved forward. The retry count clears. The round-trip sample follows Linux's `tcp_clean_rtx_queue`
         * and `tcp_ack_update_rtt`: from the oldest entry the ACK freed, measured from its transmission, when no entry
         * it freed left more than once (Karn's rule); else, on a connection with timestamps, from the ACK's echo, which
         * names the transmission it answers, so the echo on the ACK of a resend ends the backoff too
         * (`Timestamps::sample`). The timer restarts from now while anything is in flight and stops when nothing is,
         * RFC 6298's rule, which covers RFC 6582's re-arm on a partial ACK; a head that never left, the FIN a close
         * holds for the window, is `send_unsent`'s or the probes'. A partial ACK frees nothing, samples only from its
         * echo, and restarts the timer. Returns the sample taken, if any.
         */
        std::optional<core::Duration> on_acknowledged(Block& block,
                                                      const typename Transmitter::Freed& freed,
                                                      const std::optional<std::uint32_t> echo) noexcept {
            block.retransmit_count = 0;
            const std::optional<core::Duration> sample =
                freed.length > 0 && !freed.resent
                    ? std::optional{std::max<core::Duration>(context_.now - freed.sent, core::Duration::zero())}
                    : block.timestamps.sample(echo, context_.now);
            if (sample) {
                block.rtt.sample(*sample, context_.config.rto_min, context_.config.rto_max);
                ++context_.counters.rtt_samples;
            }
            if (transmitter_.in_flight(block)) {
                transmitter_.arm_retransmit(block);
            } else {
                transmitter_.disarm(block);
            }
            return sample;
        }

        /**
         * An ACK of new data on a synchronized connection: the entries it frees, the sample and the timer
         * (`on_acknowledged`), then loss recovery (`LossRecovery::on_ack`), which hears the ACK with what the policy
         * reads and says what to resend: the head as a fast retransmit on a partial ACK in fast recovery, or the new
         * head in the loss episode after a timeout, counted in `retransmits`.
         */
        void acknowledge_data(Block& block, const Sequence ack, const std::optional<std::uint32_t> echo) noexcept {
            const std::uint32_t flight_before = block.send.flight();
            const auto acked                  = static_cast<std::uint32_t>(ack - block.send.una);
            const typename Transmitter::Freed freed =
                transmitter_.acknowledge(block, ack);  // a partial entry stays whole
            block.send.una                          = ack;
            const std::optional<core::Duration> rtt = on_acknowledged(block, freed, echo);
            const AckInfo info{.acked             = acked,
                               .flight            = flight_before,
                               .mss               = block.mss(),
                               .rtt               = rtt,
                               .now               = context_.now,
                               .sent              = freed.newest_sent,
                               .delivered         = block.queue.delivered(),
                               .delivered_at_send = freed.delivered_at_send,
                               .app_limited       = freed.app_limited};
            switch (block.recovery.on_ack(info, ack, block.send)) {
                case Resend::Fast:
                    fast_retransmit(block);
                    break;
                case Resend::Head:
                    if (!block.queue.empty()) {
                        std::ignore = transmitter_.resend_head(block);
                    }
                    ++context_.counters.retransmits;
                    break;
                case Resend::None:
                    break;
            }
        }

        /// RFC 5681's duplicate ACK, counted; the third may start fast recovery (`LossRecovery::on_duplicate`), which
        /// resends the head at once.
        void on_duplicate_ack(Block& block, const Sequence ack) noexcept {
            ++context_.counters.duplicate_acks;
            if (block.recovery.on_duplicate(ack, block.send, block.mss()) == Resend::Fast) {
                fast_retransmit(block);
                ++context_.counters.recoveries;
            }
        }

        /**
         * The head again, driven by the third duplicate or a partial ACK, counted in `fast_retransmits`. When it
         * leaves, in place or as a copy, the RTO restarts from now, as Linux re-arms it on resending the head
         * (`tcp_xmit_retransmit_queue`), so the timer the first send armed does not expire before the resend's ACK can
         * return. A resend deferred, or refused, leaves the timer as it was.
         */
        void fast_retransmit(Block& block) noexcept {
            ++context_.counters.fast_retransmits;
            if (!transmitter_.in_flight(block)) {
                return;  // nothing in flight: the head, if any, waits for the window
            }
            if (transmitter_.resend_head(block) && !transmitter_.deferred(block)) {
                transmitter_.arm_retransmit(block);
            }
        }

        Context& context_;
        Lifecycle& lifecycle_;
        Transmitter& transmitter_;
    };

}  // namespace aloe::tcp::detail
