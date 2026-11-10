#pragma once

#include <algorithm>
#include <aloe/core>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/wire>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <tuple>
#include <utility>

#include <detail/connection_list.hpp>
#include <detail/control_block.hpp>
#include <detail/retransmission_queue.hpp>
#include <detail/segment_writer.hpp>
#include <detail/sent_segment_pool.hpp>
#include <detail/stack_context.hpp>
#include <detail/zero_window_probe.hpp>
#include <tcp_config.hpp>

namespace aloe::tcp::detail {

    /// What an expiry of a connection's timer leaves for the Lifecycle (`Transmitter::on_timer`).
    enum class TimerExpiry : std::uint8_t {
        Handled,       ///< Resent, probed or polled, and re-armed as it needs: nothing is left.
        TimedOut,      ///< The retry cap ran out: the connection ends `TimedOut`.
        UserTimedOut,  ///< The user timeout ran out: the same end, counted apart.
        Unresolved,    ///< The next hop never answered ARP: the connection ends `Unresolved`.
    };

    /**
     * @brief Everything that leaves a connection, and what guards it: the one send function over the segment
     * writer, the retransmission queue's entries on the shard's send pool, the resends with the busy rule and the
     * copy fallback, the pure ACKs, probes and resets, the timers' arming and what an expiry resends, and the
     * application's `writable`, `prepare` and `commit`. It owns the ACK, retry and deferred lists; the Receiver and the
     * Lifecycle call down into it, and it calls neither.
     */
    template <typename Ip, typename CongestionControl>
    class Transmitter {
    public:
        using Packet       = Ip::Packet;
        using ControlBlock = detail::ControlBlock<Ip, CongestionControl>;
        using Context      = StackContext<Ip, CongestionControl>;
        using Sequence     = wire::TcpSequence;
        using SentNode     = SentSegment<Packet>;
        using Freed        = RetransmissionQueue<Packet>::Freed;

        Transmitter(Context& context, loop::TimerWheel& wheel)
            : context_{context},
              wheel_{wheel},
              send_pool_{context.config.send_pool},
              writer_{context.ip, context.counters} {
        }

        // The lists.
        /// An ordinary ACK: on the ACK list, sent by flush unless a segment carrying an ACK leaves first.
        void queue_ack(ControlBlock& block) noexcept {
            if (!acks_.contains(block)) {
                acks_.push_back(block);
            }
        }

        /// The next connection with a retry hint, off the list; null when none.
        [[nodiscard]] ControlBlock* pop_retry() noexcept {
            return retries_.pop_front();
        }

        /// The ACK list for `Stack::flush`'s pass: the first waiting connection, and the next after one.
        [[nodiscard]] ControlBlock* ack_front() const noexcept {
            return acks_.front();
        }

        [[nodiscard]] static ControlBlock* ack_next(ControlBlock& block) noexcept {
            return AckList::next_of(block);
        }

        /// An ordinary ACK waits for flush.
        [[nodiscard]] bool ack_pending(const ControlBlock& block) const noexcept {
            return acks_.contains(block);
        }

        /// A resend waits for its packet.
        [[nodiscard]] bool deferred(const ControlBlock& block) const noexcept {
            return deferred_.contains(block);
        }

        /// Off the ACK and retry lists; `drop_queue` takes it off the deferred list.
        void forget(ControlBlock& block) noexcept {
            acks_.remove(block);
            retries_.remove(block);
        }

        /// The resends that found their packet lent: the first loop of `Stack::flush`.
        void flush_deferred() noexcept {
            for (ControlBlock* block = deferred_.front(); block != nullptr;) {
                ControlBlock* next = DeferredList::next_of(*block);  // a resend that leaves unlinks block
                std::ignore        = resend_head(*block);
                block              = next;
            }
        }

        // The queue.
        /// Queues a SYN or SYN-ACK at `sequence` and transmits it. The entry stays when the send fails, for the
        /// timer; `Refused`, counted, when no node is free.
        [[nodiscard]] std::expected<void, net::SendError>
        queue_control(ControlBlock& block, const wire::TcpFlags flags, const Sequence sequence) noexcept {
            const std::optional<std::uint32_t> entry = enqueue(block, Packet{}, 0, flags, sequence, 1);
            if (!entry) {
                ++context_.counters.send_refused;
                return std::unexpected{net::SendError::Refused};
            }
            return transmit_entry(block, *entry);
        }

        /// The FIN's entry, on the node Established set aside for it; the stack's own, so the send cap does not hold
        /// it.
        [[nodiscard]] std::optional<std::uint32_t> enqueue_fin(ControlBlock& block, const Sequence fin) noexcept {
            return block.queue.push_fin(
                send_pool_, entry(block, Packet{}, 0, wire::TcpFlags{wire::TcpFlag::Fin, wire::TcpFlag::Ack}, fin, 1));
        }

        /**
         * One transmission of a queued entry through `emit`; its packet must be back from the device. A SYN, SYN-ACK
         * or FIN entry without a packet takes one from IP first. A success stamps the entry and counts a control
         * segment for a SYN or FIN; either way the packet holds its frame from then on. The first transmission of the
         * only entry, a segment sent into an empty queue, arms the timer: the RTO when it left, the ARP poll when a
         * SYN, SYN-ACK or FIN found no next hop. A data segment that found none is not kept: `commit` refuses it.
         */
        [[nodiscard]] std::expected<void, net::SendError> transmit_entry(ControlBlock& block,
                                                                         const std::uint32_t index) noexcept {
            SentNode& node = send_pool_.node(index);
            assert(!node.packet.lent() && "a queued packet is written only once the device gave it back");
            const bool first_into_empty = node.transmits == 0 && index == block.queue.head() && block.queue.size() == 1;
            if (node.packet.empty()) {
                std::optional<Packet> fresh = context_.ip.allocate();
                if (!fresh) {
                    ++context_.counters.send_refused;
                    return std::unexpected{net::SendError::Refused};
                }
                node.packet = std::move(*fresh);
            }
            const std::expected<void, net::SendError> sent =
                emit(block, node.packet, node.framed, node.first, node.flags, node.flags.has(wire::TcpFlag::Syn));
            node.framed = true;
            if (!sent) {
                if (first_into_empty && node.payload == 0 && sent.error() == net::SendError::Unresolved) {
                    arm_poll(block);
                }
                return sent;
            }
            record_transmission(block, node);
            if (first_into_empty) {
                arm_retransmit(block);
            }
            return {};
        }

        /**
         * The head entry again. A copy out is reconciled first. Still lent: the device is asked to reclaim, and if it
         * still holds the packet nothing is written into it and the busy rule decides (`resend_busy`). Back: when
         * SND.UNA passed the entry's first byte, the acknowledged front is trimmed off (`SentSegment::trim_to`), and
         * `transmit_entry` sends what is left. IP's error, or nothing when the resend left or was deferred. A send that
         * failed leaves the deferred list too: the timer's next expiry tries again.
         */
        [[nodiscard]] std::expected<void, net::SendError> resend_head(ControlBlock& block) noexcept {
            assert(!block.queue.empty() && "a resend with nothing queued");
            SentNode& node = send_pool_.node(block.queue.head());
            node.reconcile();
            if (node.packet.lent() && context_.ip.queue().reclaim() > 0) {
                node.reconcile();  // what the device let go of may be either packet
            }
            if (node.packet.lent()) {
                return resend_busy(block, node);
            }
            if (block.send.una.after(node.first)) {
                node.trim_to(block.send.una);
            }
            deferred_.remove(block);  // the packet is back: whatever IP answers, this resend is not waiting any more
            const std::expected<void, net::SendError> sent = transmit_entry(block, block.queue.head());
            if (sent) {
                node.deferrals = 0;
            }
            return sent;
        }

        /// The queue frees what `ack` covers (`RetransmissionQueue::acknowledge`); a resend that waited on the deferred
        /// list is moot once its entry is freed. Called before SND.UNA moves.
        [[nodiscard]] Freed acknowledge(ControlBlock& block, const Sequence ack) noexcept {
            const Freed freed = block.queue.acknowledge(send_pool_, ack, block.send.una);
            if (freed.length > 0) {
                deferred_.remove(block);
            }
            return freed;
        }

        /// Established: one node of the pool is set aside for the connection's FIN. The ACK that brought the
        /// connection here freed the handshake's node, so one is free to set aside.
        void reserve_fin_node(ControlBlock& block) noexcept {
            [[maybe_unused]] const bool reserved = block.queue.reserve_fin(send_pool_);
            assert(reserved && "the handshake's node just came back");
        }

        /// The retransmission queue dropped (`RetransmissionQueue::drop`), and the connection off the deferred list.
        void drop_queue(ControlBlock& block) noexcept {
            block.queue.drop(send_pool_);
            deferred_.remove(block);
        }

        /// Whether a segment the peer can acknowledge is out (`RetransmissionQueue::in_flight`).
        [[nodiscard]] bool in_flight(const ControlBlock& block) const noexcept {
            return block.queue.in_flight(send_pool_);
        }

        /// The head entry; the queue is not empty, asserted.
        [[nodiscard]] const SentNode& head(const ControlBlock& block) const noexcept {
            assert(!block.queue.empty() && "the head of an empty queue");
            return send_pool_.node(block.queue.head());
        }

        [[nodiscard]] std::size_t nodes_available() const noexcept {
            return send_pool_.available();
        }

        /// For tests and monitoring: the entry at `position`, 0 the head.
        [[nodiscard]] const SentNode& sent_segment(const ControlBlock& block,
                                                   const std::size_t position) const noexcept {
            return block.queue.at(send_pool_, position);
        }

        // The sends that keep nothing.
        /// A pure ACK at `snd_nxt`.
        [[nodiscard]] bool send_pure_ack(ControlBlock& block) noexcept {
            if (!send_bare_ack(block, block.send.nxt)) {
                return false;
            }
            ++context_.counters.pure_acks_sent;
            return true;
        }

        /// abort's RST, at `snd_nxt` with the ACK, window and timestamps an ordinary segment carries (RFC 7323: should
        /// carry them, as Linux's does): through `Ipv4::send` and forgotten, since nothing of it is kept.
        [[nodiscard]] bool send_reset(ControlBlock& block) noexcept {
            const Sequence next  = block.receive.next();
            const Sequence offer = offer_of(block);
            const OutgoingSegment reset{
                .header     = wire::TcpHeader{.source_port      = block.local.port,
                                              .destination_port = block.remote.port,
                                              .sequence         = block.send.nxt,
                                              .acknowledgement  = next,
                                              .flags            = wire::TcpFlags{wire::TcpFlag::Rst, wire::TcpFlag::Ack},
                                              .window           = static_cast<std::uint16_t>(offer.value - next.value)},
                .timestamps = timestamp_option(block)
            };
            if (!writer_.send_detached(block.remote.address, reset, context_.now)) {
                return false;
            }
            record_offer(block, offer);
            ++context_.counters.resets_sent;
            return true;
        }

        /// RFC 793's reset for a segment with no connection: with ACK, sequence at its acknowledgement;
        /// without, sequence zero and acknowledgement past everything it occupied.
        void reply_reset(const wire::Ipv4Address source,
                         const wire::TcpHeader& header,
                         const std::uint32_t length) noexcept {
            OutgoingSegment reply{
                .header =
                    wire::TcpHeader{.source_port = header.destination_port, .destination_port = header.source_port}
            };
            if (header.flags.has(wire::TcpFlag::Ack)) {
                reply.header.sequence = header.acknowledgement;
                reply.header.flags    = wire::TcpFlags{wire::TcpFlag::Rst};
            } else {
                reply.header.acknowledgement = header.sequence + length;
                reply.header.flags           = wire::TcpFlags{wire::TcpFlag::Rst, wire::TcpFlag::Ack};
            }
            if (writer_.send_detached(source, reply, context_.now)) {
                ++context_.counters.resets_sent;
            }
        }

        // The timers.
        /// The RTO from now; under a user timeout on a synchronized connection, never later than the head's first
        /// transmission plus the timeout, Linux's `tcp_clamp_rto_to_user_timeout`, and never earlier than now.
        void arm_retransmit(ControlBlock& block) noexcept {
            core::TimePoint deadline = context_.now + block.rtt.rto();
            if (const SentNode* head = user_timed_head(block); head != nullptr) {
                deadline = std::min(deadline, std::max(context_.now, head->first_sent + context_.config.user_timeout));
            }
            block.timer_kind = TimerKind::Retransmit;
            wheel_.arm(block.timer, deadline);
        }

        /// The peer's window is zero with nothing in flight: the first probe one RTO away.
        void start_probing(ControlBlock& block) noexcept {
            block.probe.start(block.rtt.rto());
            arm_probe(block);
        }

        void disarm(ControlBlock& block) noexcept {
            wheel_.cancel(block.timer);
            block.timer_kind = TimerKind::None;
        }

        /**
         * The connection's timer fired, unarmed by the wheel; its kind says what for. The RTO resends the head, the
         * ARP poll tries again the send that never left, the probe timer probes, and each arms what guards the
         * connection next. Says what is left for the caller: nothing, or the end of a connection that the retries,
         * the user timeout or a silent next hop gave up on, which is the Lifecycle's to carry out.
         */
        [[nodiscard]] TimerExpiry on_timer(ControlBlock& block) noexcept {
            const TimerKind kind = std::exchange(block.timer_kind, TimerKind::None);
            assert(kind != TimerKind::None && "the wheel fires only an armed timer");
            switch (kind) {
                case TimerKind::Retransmit:
                    return on_retransmit_timeout(block);
                case TimerKind::ArpPoll:
                    return on_arp_poll(block);
                case TimerKind::Probe:
                    return on_probe_timeout(block);
                case TimerKind::None:
                    break;
            }
            return TimerExpiry::Handled;
        }

        // The application.
        /// The MSS, within what the windows allow beyond the flight (`LossRecovery::room`), in Established or
        /// CloseWait with no prepare open and the queue below the send cap; else zero.
        [[nodiscard]] std::size_t writable(const ControlBlock& block) const noexcept {
            if (block.prepared || (block.state != State::Established && block.state != State::CloseWait)) {
                return 0;
            }
            if (block.queue.size() >= context_.config.send_segments) {
                return 0;  // the send cap: the ACK that frees an entry raises Writable
            }
            return static_cast<std::size_t>(std::min<std::uint64_t>(block.mss(), block.recovery.room(block.send)));
        }

        /**
         * A span of `min(count, writable())` bytes inside a fresh packet. The packet belongs to the application until
         * `commit` (with any count) or `release`: the span stays valid through any state change in between. Nothing,
         * without allocating, for a count of zero or nothing writable; an allocation failure counts one
         * `allocation_failures`, marks a retry hint and retains nothing.
         */
        [[nodiscard]] std::optional<std::span<std::byte>> prepare(ControlBlock& block,
                                                                  const std::size_t count) noexcept {
            assert(!block.prepared && "one prepare at a time: commit first");
            if (count == 0) {
                return std::nullopt;
            }
            const std::size_t room = writable(block);
            if (room == 0) {
                return std::nullopt;  // nothing allocates when nothing is writable
            }
            const std::size_t size       = std::min(count, room);
            std::optional<Packet> packet = context_.ip.allocate();
            if (!packet) {
                ++context_.counters.allocation_failures;
                mark_retry(block);  // the pool may have packets by the next process
                return std::nullopt;
            }
            const std::optional<std::span<std::byte>> span = packet->append(size);
            assert(span && "size is at most the MSS and a packet holds a whole frame");
            if (!span) {
                ++context_.counters.allocation_failures;  // unreachable by construction; counted in a release build
                mark_retry(block);
                return std::nullopt;
            }
            // The span points into the block, which does not move.
            block.prepared = typename ControlBlock::Preparation{.packet = std::move(*packet), .size = size};
            return span;
        }

        /**
         * One PSH+ACK segment of the first `count` prepared bytes; `commit(0)` discards the preparation. A segment into
         * an empty queue more than an RTO after the last data left tells the policy of the idle period first. A refusal
         * returns the packet to the pool, leaves every number as it was, marks a retry hint and returns false. A
         * connection that left Established and CloseWait since the prepare drops the packet and refuses the commit
         * with no hint: the events say why. A window that no longer covers `count`, because the peer retracted its
         * right edge or a loss shrank the congestion window since the prepare, refuses the commit with a hint: prepare
         * again for what `writable()` allows. No queue node free refuses it with a hint too.
         */
        [[nodiscard]] bool commit(ControlBlock& block, const std::size_t count) noexcept {
            assert(block.prepared && "commit without a prepare");
            if (!block.prepared) {
                ++context_.counters.commits_refused;  // a contract violation, refused in a release build
                return false;
            }
            assert(count <= block.prepared->size && "commit more than was prepared");
            Packet packet              = std::move(block.prepared->packet);
            const std::size_t prepared = block.prepared->size;
            block.prepared.reset();
            if (count == 0) {
                return true;  // discarded: `packet` goes back to the pool; nothing was refused
            }
            if (block.state != State::Established && block.state != State::CloseWait) {
                ++context_.counters.commits_refused;  // closed since the prepare: no hint, the events say why
                return false;
            }
            if (count > writable(block)) {  // the preparation is released, so this is the credit the peer offers now
                // RFC 1122: a sender copes with a shrunk window by not sending into it.
                ++context_.counters.commits_refused;
                mark_retry(block);
                return false;
            }
            packet.trim_back(prepared - count);
            if (block.queue.idle(context_.now, block.rtt.rto())) {
                block.recovery.on_idle(block.mss());  // RFC 5681's restart after an idle RTO
            }
            const auto payload                       = static_cast<std::uint16_t>(count);
            const std::uint32_t previous_tail        = block.queue.tail();
            const std::optional<std::uint32_t> entry = enqueue(block,
                                                               std::move(packet),
                                                               payload,
                                                               wire::TcpFlags{wire::TcpFlag::Ack, wire::TcpFlag::Psh},
                                                               block.send.nxt,
                                                               payload);
            if (!entry) {
                ++context_.counters.allocation_failures;  // no queue node free: the packet goes back to the pool
                mark_retry(block);
                return false;
            }
            if (!transmit_entry(block, *entry)) {
                block.queue.pop_back(send_pool_, previous_tail);  // the packet goes back with the node
                mark_retry(block);                                // emit counted the refusal
                return false;
            }
            block.send.nxt = block.send.nxt + static_cast<std::uint32_t>(count);
            ++context_.counters.data_segments_sent;
            return true;
        }

        /// The right edge an ACK offers now (`ReceiveQueue::offer`).
        [[nodiscard]] Sequence offer_of(const ControlBlock& block) const noexcept {
            return block.receive.offer(context_.byte_budget, context_.config.receive_segments);
        }

    private:
        /// A Writable hint at the next process, at most one per marked connection.
        void mark_retry(ControlBlock& block) noexcept {
            if (!retries_.contains(block)) {
                retries_.push_back(block);
            }
        }

        using AckList      = ConnectionList<ControlBlock, &ControlBlock::ack_link>;
        using RetryList    = ConnectionList<ControlBlock, &ControlBlock::retry_link>;
        using DeferredList = ConnectionList<ControlBlock, &ControlBlock::deferred_link>;

        /// A segment with ACK left offering `offer`: the edge is recorded and the ACK that waited is done.
        void record_offer(ControlBlock& block, const Sequence offer) noexcept {
            if (block.receive.record_offer(offer)) {
                ++context_.counters.window_updates;
            }
            acks_.remove(block);
            block.ack_policy.acknowledged();
        }

        /// The timestamp option a segment of the connection carries now: the clock's value and TS.Recent as the echo.
        [[nodiscard]] std::optional<TimestampOption> timestamp_option(const ControlBlock& block) const noexcept {
            if (!block.timestamps.enabled()) {
                return std::nullopt;
            }
            return TimestampOption{.value = block.timestamps.value(context_.now), .echo = block.timestamps.echo()};
        }

        /**
         * The one send function: every segment of a connection leaves here, kept or not, framed by the writer. The
         * option block carries the MSS on a handshake segment and the timestamps on every segment of a connection
         * that sends them. A segment with ACK offers the receive window (`offer_of`). The packet stays with the
         * caller. Only a success records the offer and Last.ACK.sent and clears the pending ACK; a failure, counted
         * by the writer, leaves the packet framed.
         */
        [[nodiscard]] std::expected<void, net::SendError> emit(ControlBlock& block,
                                                               Packet& packet,
                                                               const bool framed,
                                                               const Sequence sequence,
                                                               const wire::TcpFlags flags,
                                                               const bool handshake) noexcept {
            const bool with_ack  = flags.has(wire::TcpFlag::Ack);
            const Sequence next  = block.receive.next();
            const Sequence offer = with_ack ? offer_of(block) : block.receive.offered();
            const OutgoingSegment segment{
                .header     = wire::TcpHeader{.source_port      = block.local.port,
                                              .destination_port = block.remote.port,
                                              .sequence         = sequence,
                                              .acknowledgement  = with_ack ? next : Sequence{},
                                              .flags            = flags,
                                              .window = with_ack ? static_cast<std::uint16_t>(offer.value - next.value)
                                                                 : static_cast<std::uint16_t>(context_.byte_budget)},
                .mss        = handshake ? std::optional{context_.mss}
                      : std::nullopt,
                .timestamps = timestamp_option(block)
            };
            const std::expected<void, net::SendError> sent =
                writer_.send(block.frame, block.remote.address, packet, framed, segment, context_.now);
            if (sent && with_ack) {
                record_offer(block, offer);
                block.timestamps.sent_ack(next);
            }
            return sent;
        }

        /// A queue entry for a segment about to leave, with what a rate sample reads: the delivered count, and whether
        /// the windows allowed more than it carries.
        [[nodiscard]] SentNode entry(const ControlBlock& block,
                                     Packet&& packet,
                                     const std::uint16_t payload,
                                     const wire::TcpFlags flags,
                                     const Sequence first,
                                     const std::uint32_t length) const noexcept {
            return SentNode{.packet            = std::move(packet),
                            .first             = first,
                            .length            = length,
                            .payload           = payload,
                            .flags             = flags,
                            .delivered_at_send = block.queue.delivered(),
                            .app_limited       = block.recovery.room(block.send) > payload};
        }

        /**
         * An entry for a segment about to leave, linked at the tail of the queue. `packet` may be empty for a SYN or
         * SYN-ACK, which `transmit_entry` gives one. Nothing, the packet back to its pool, when no node is free or a
         * data segment would pass the send cap. The FIN's entry is `enqueue_fin`.
         */
        [[nodiscard]] std::optional<std::uint32_t> enqueue(ControlBlock& block,
                                                           Packet&& packet,
                                                           const std::uint16_t payload,
                                                           const wire::TcpFlags flags,
                                                           const Sequence first,
                                                           const std::uint32_t length) noexcept {
            if (payload > 0 && block.queue.size() >= context_.config.send_segments) {
                return std::nullopt;
            }
            return block.queue.push(send_pool_, entry(block, std::move(packet), payload, flags, first, length));
        }

        /// A transmission of the entry left, from its packet or as a copy: stamped, on the queue too when it carries
        /// payload (the idle restart's clock), counted as a control segment for a SYN or FIN, and the ARP poll's count
        /// cleared.
        void record_transmission(ControlBlock& block, SentNode& node) noexcept {
            block.queue.sent(node, context_.now);
            if (node.control()) {
                ++context_.counters.control_segments_sent;
            }
            block.unresolved_polls = 0;
        }

        /**
         * The busy rule, for a head whose packet the device still holds after a reclaim. A resend at the stamp of the
         * last deferral is that deferral again, since the device had no time to finish: it stays on the deferred list
         * and counts nothing. Below `retransmit_defers` deferrals the resend waits for the next flush, counted once
         * per deferral; at the bound, with no copy out, the copy goes (`send_copy`); with a copy out and both packets
         * lent, it waits again.
         */
        [[nodiscard]] std::expected<void, net::SendError> resend_busy(ControlBlock& block, SentNode& node) noexcept {
            if (deferred_.contains(block) && node.deferred_at == context_.now) {
                return {};
            }
            if (node.deferrals < context_.config.retransmit_defers) {
                ++node.deferrals;
            } else if (node.copy.empty()) {
                return send_copy(block, node);
            }
            node.deferred_at = context_.now;
            if (!deferred_.contains(block)) {
                deferred_.push_back(block);
            }
            ++context_.counters.retransmits_deferred;
            return {};
        }

        /**
         * The copy fallback: a fresh packet with the lent packet's header bytes and the part of its payload not yet
         * acknowledged, read from the lent packet and never written into it (`SegmentWriter::copy`), framed by `emit`
         * from `max(first, snd_una)` and lent as the entry's second packet; counted in `retransmits_copied`. No packet
         * free counts `allocation_failures` and refuses; a refused send drops the copy. Either way the entry stays as
         * it was and the connection leaves the deferred list: the timer's next expiry tries again.
         */
        [[nodiscard]] std::expected<void, net::SendError> send_copy(ControlBlock& block, SentNode& node) noexcept {
            assert(node.framed && node.copy.empty() && "one copy at a time, of a frame that left");
            deferred_.remove(block);
            const Sequence first    = block.send.una.after(node.first) ? block.send.una : node.first;
            const auto acknowledged = static_cast<std::uint32_t>(first - node.first);
            assert((acknowledged == 0 || acknowledged < node.payload) && "only data is acknowledged in part");
            const auto payload            = static_cast<std::uint16_t>(node.payload - acknowledged);
            const std::size_t header_size = node.packet.size() - node.payload;
            std::optional<Packet> copy    = writer_.copy(node.packet, header_size, acknowledged, payload);
            if (!copy) {
                ++context_.counters.allocation_failures;
                return std::unexpected{net::SendError::Refused};
            }
            if (const std::expected<void, net::SendError> sent =
                    emit(block, *copy, true, first, node.flags, node.flags.has(wire::TcpFlag::Syn));
                !sent) {
                return sent;  // the copy goes back to the pool
            }
            node.copy         = std::move(*copy);
            node.copy_first   = first;
            node.copy_payload = payload;
            node.deferrals    = 0;
            record_transmission(block, node);
            ++context_.counters.retransmits_copied;
            return {};
        }

        /// A header-only ACK at `sequence` from a fresh packet: lent to the device and forgotten, never kept. A failure
        /// is counted.
        [[nodiscard]] bool send_bare_ack(ControlBlock& block, const Sequence sequence) noexcept {
            std::optional<Packet> packet = context_.ip.allocate();
            if (!packet) {
                ++context_.counters.send_refused;
                return false;
            }
            if (!emit(block, *packet, false, sequence, wire::TcpFlags{wire::TcpFlag::Ack}, false)) {
                return false;  // the packet goes back to the pool
            }
            *packet = Packet{};  // the device frees the buffer when it is done
            return true;
        }

        /// A zero-window probe, Linux's `tcp_xmit_probe_skb`: a pure ACK at `snd_una - 1`, which occupies no sequence
        /// space and which the peer, finding it outside its window, answers with an ACK carrying its window.
        [[nodiscard]] bool send_probe(ControlBlock& block) noexcept {
            if (!send_bare_ack(block, Sequence{block.send.una.value - 1})) {
                return false;
            }
            ++context_.counters.zero_window_probes;
            return true;
        }

        /// The entry the user timeout counts from: the head of the queue once it has left, on a synchronized
        /// connection, with the timeout set. Null otherwise: the handshake is bounded by `retries` alone.
        [[nodiscard]] const SentNode* user_timed_head(const ControlBlock& block) const noexcept {
            if (context_.config.user_timeout <= core::Duration::zero() || block.state == State::SynSent ||
                block.state == State::SynReceived || block.queue.empty()) {
                return nullptr;
            }
            const SentNode& head = send_pool_.node(block.queue.head());
            return head.transmits > 0 ? &head : nullptr;
        }

        /// A SYN, SYN-ACK or FIN whose first send found no next hop: tried again every `unresolved_retry`, as no retry.
        void arm_poll(ControlBlock& block) noexcept {
            block.timer_kind = TimerKind::ArpPoll;
            wheel_.arm(block.timer, context_.now + context_.config.unresolved_retry);
        }

        /// The next probe (`ZeroWindowProbe::deadline`).
        void arm_probe(ControlBlock& block) noexcept {
            block.timer_kind = TimerKind::Probe;
            wheel_.arm(block.timer, block.probe.deadline(context_.now, context_.config.user_timeout));
        }

        /**
         * The RTO ran out: one retry, the head of the queue again. First the user timeout, counted from the head's
         * first transmission on a synchronized connection, and then the cap, `retries` in the handshake and
         * `data_retries` after it, may give the connection up instead. The first expiry since an ACK of new data
         * starts the loss episode (`LossRecovery::on_timeout`); each expiry doubles the RTO up to `rto_max` and
         * re-arms. A resend deferred or refused counts as the retry all the same: the next expiry tries again. A
         * first send, of a SYN, SYN-ACK or FIN the device refused before, that finds no next hop arms the ARP poll
         * instead (`transmit_entry`), and the expiry leaves the timer to it: nothing left, so no backoff; the poll
         * arms the RTO once the send leaves, or gives the connection up as `Unresolved`.
         */
        [[nodiscard]] TimerExpiry on_retransmit_timeout(ControlBlock& block) noexcept {
            assert(!block.queue.empty() && "the RTO runs while something is queued");
            if (const SentNode* head = user_timed_head(block);
                head != nullptr && context_.now - head->first_sent >= context_.config.user_timeout) {
                return TimerExpiry::UserTimedOut;
            }
            if (const bool handshake = block.state == State::SynSent || block.state == State::SynReceived;
                block.retransmit_count >= (handshake ? context_.config.retries : context_.config.data_retries)) {
                return TimerExpiry::TimedOut;
            }
            if (++block.retransmit_count == 1) {
                block.recovery.on_timeout(block.send, block.mss());
            }
            std::ignore = resend_head(block);
            ++context_.counters.retransmits;
            if (block.timer_kind == TimerKind::ArpPoll) {
                return TimerExpiry::Handled;  // a first send that found no next hop: the poll retries it
            }
            block.rtt.back_off(context_.config.rto_max);
            arm_retransmit(block);
            return TimerExpiry::Handled;
        }

        /// The ARP poll: the SYN, SYN-ACK or FIN that never left is tried again, as no retry. Once it leaves, or the
        /// device refuses it, the RTO takes over; a next hop still silent after `unresolved_polls` polls gives the
        /// connection up as `Unresolved`, the bound Linux's ARP probes give.
        [[nodiscard]] TimerExpiry on_arp_poll(ControlBlock& block) noexcept {
            assert(!block.queue.empty() && "the poll retries an entry that never left");
            if (const std::expected<void, net::SendError> sent = transmit_entry(block, block.queue.head());
                sent || sent.error() != net::SendError::Unresolved) {
                arm_retransmit(block);
                return TimerExpiry::Handled;
            }
            if (++block.unresolved_polls >= context_.config.unresolved_polls) {
                return TimerExpiry::Unresolved;  // the next hop is not going to answer
            }
            arm_poll(block);
            return TimerExpiry::Handled;
        }

        /// The probe timer ran out with the peer's window still zero (`ZeroWindowProbe::expire`): the user timeout or
        /// `data_retries` unanswered probes give the connection up; else one probe goes, a refused one counted all
        /// the same, and the next is armed.
        [[nodiscard]] TimerExpiry on_probe_timeout(ControlBlock& block) noexcept {
            assert(!in_flight(block) && block.send.wnd == 0 &&
                   (!block.queue.empty() || block.state == State::Established || block.state == State::CloseWait) &&
                   "a probe runs only while the window is zero and nothing is in flight");
            switch (block.probe.expire(
                context_.now, context_.config.user_timeout, context_.config.data_retries, context_.config.rto_max)) {
                case ZeroWindowProbe::Expiry::UserTimedOut:
                    return TimerExpiry::UserTimedOut;
                case ZeroWindowProbe::Expiry::TimedOut:
                    return TimerExpiry::TimedOut;
                case ZeroWindowProbe::Expiry::Probe:
                    break;
            }
            std::ignore = send_probe(block);  // a refusal was counted by the send
            arm_probe(block);
            return TimerExpiry::Handled;
        }

        Context& context_;
        loop::TimerWheel& wheel_;
        SentSegmentPool<Packet> send_pool_;
        AckList acks_          = {};
        RetryList retries_     = {};
        DeferredList deferred_ = {};  ///< Connections whose head resend waits for its packet.
        SegmentWriter<Ip> writer_;    ///< Counts into the context's counters.
    };

}  // namespace aloe::tcp::detail
