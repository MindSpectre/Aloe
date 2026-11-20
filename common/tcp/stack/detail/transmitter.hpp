#pragma once

#include <algorithm>
#include <aloe/core>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/wire>
#include <array>
#include <cassert>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <tuple>
#include <utility>

#include <detail/connection_list.hpp>
#include <detail/control_block.hpp>
#include <detail/node_pool.hpp>
#include <detail/rack.hpp>
#include <detail/receive_window.hpp>
#include <detail/retransmission_queue.hpp>
#include <detail/sack_blocks.hpp>
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

    /// Which edge a segment with an ACK states.
    enum class Edge : std::uint8_t {
        Offer,    ///< What the window offers now (`ReceiveWindow::offer`).
        Restate,  ///< The edge already offered: a duplicate ACK stays a duplicate (RFC 5681).
    };

    /**
     * @brief Everything that leaves a connection, and what guards it: the one send function over the segment
     * writer, the retransmission queue's entries on the shard's send pool, the resend of any entry with the busy rule
     * and the copy fallback and the pass over those marked lost, what is in flight and the window it is held to, the
     * pure ACKs, probes and resets, the timers' arming and what an expiry marks and resends, the delayed ACK's timer,
     * and the application's `writable`, `prepare` and `commit`. It owns the ACK, retry and deferred lists; the
     * Receiver and the Lifecycle call down into it, and it calls neither.
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
        using Reported     = RetransmissionQueue<Packet>::Reported;

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

        /// Off the ACK and retry lists; `drop_queue` takes it off the deferred list.
        void forget(ControlBlock& block) noexcept {
            acks_.remove(block);
            retries_.remove(block);
        }

        /// The resends that found their packet lent: the first loop of `Stack::flush`, the pass again for each
        /// connection one of whose entries waits (`resend_lost`). A pass that sends the last entry that owed a resend
        /// may leave room for new data, with no ACK to say so: the connection gets its Writable hint
        /// (`hint_if_writable_rose`).
        void flush_deferred() noexcept {
            for (ControlBlock* block = deferred_.front(); block != nullptr;) {
                ControlBlock* next       = DeferredList::next_of(*block);  // a resend that leaves may unlink block
                const std::size_t before = writable(*block);
                std::ignore              = resend_lost(*block);
                hint_if_writable_rose(*block, before);
                block = next;
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
         * The entry at `index` again. A copy out is reconciled first. Still lent: the device is asked to reclaim, and
         * if it still holds the packet nothing is written into it and the busy rule decides (`resend_busy`). Back: when
         * SND.UNA passed the entry's first byte, which only the head can need, the acknowledged front is trimmed off
         * (`RetransmissionQueue::trim_to`), and `transmit_entry` writes the headers over the old ones and sends what is
         * left. IP's error, or nothing when the resend left or waits for its packet. A resend that left, in place or as
         * a copy, clears the entry's lost mark, an entry not marked having none to clear, and counts against the rate
         * reduction in recovery (`LossRecovery::on_sent`). One that IP refused does not wait: the next pass or the
         * timer's next expiry tries again. Every call is a try of the entry's mark, so the pass counts no resend
         * twice, whoever tried first.
         */
        [[nodiscard]] std::expected<void, net::SendError> resend(ControlBlock& block,
                                                                 const std::uint32_t index) noexcept {
            assert(index != no_node && "a resend of no entry");
            SentNode& node = send_pool_.node(index);
            block.queue.mark_tried(send_pool_, index);
            block.queue.reconcile(send_pool_, index);  // the queue owns every change to an entry's length
            if (node.packet.lent() && context_.ip.queue().reclaim() > 0) {
                block.queue.reconcile(send_pool_, index);  // what the device let go of may be either packet
            }
            std::expected<void, net::SendError> sent{};
            if (node.packet.lent()) {
                sent = resend_busy(block, index);
            } else {
                if (block.send.una.after(node.first)) {
                    block.queue.trim_to(send_pool_, index, block.send.una);
                }
                // The packet is back: whatever IP answers, this resend is not waiting any more.
                block.queue.clear_waiting(send_pool_, index);
                sent = transmit_entry(block, index);
                if (sent) {
                    node.deferrals = 0;
                }
            }
            if (sent && !node.waiting) {  // it left, in place or as a copy
                block.queue.clear_lost(send_pool_, index);
                block.recovery.on_sent(unacknowledged(block, node));
            }
            settle_deferred(block);
            return sent;
        }

        /**
         * The resend pass, Linux's `tcp_xmit_retransmit_queue`: the entries marked lost, lowest sequence first, each
         * through `resend`, until none is marked, the window allows no more, or one could not leave, refused or
         * waiting for its packet; one that waits keeps the connection on the deferred list, and the flush runs the
         * pass again. RFC 8985 9.3 holds a resend to the congestion window like new data: nothing leaves once what is
         * in flight matches the window (`LossRecovery::may_send`), and an entry marked lost is not in flight until its
         * resend leaves. The window gates the pass once the policy has one, at Established: a handshake segment a
         * duplicate SYN or the timer asked for goes whatever it says. With nothing marked lost the pass returns at
         * once, without a walk of the queue (`RetransmissionQueue::first_lost`): an ACK on a path without loss pays
         * one comparison for it.
         *
         * Counts one `fast_retransmits` per marked entry, at the first try since it was marked, whether it left,
         * waited or was refused; a later try of the same mark counts nothing, and neither does the pass's try of an
         * entry the timer's expiry tried first. When the entry that left is the head, the RTO is armed from now, as
         * Linux re-arms it on resending the head, so the timer an earlier send armed does not expire before the
         * resend's ACK can return; unless the reordering timer is pending, which stays, and only the RTO's moment
         * moves (`restart_rto`). Returns the sequence space that left.
         */
        [[nodiscard]] std::uint32_t resend_lost(ControlBlock& block) noexcept {
            std::uint32_t left  = 0;
            std::uint32_t index = block.queue.first_lost(send_pool_);
            while (index != no_node) {
                if (!handshaking(block) && !block.recovery.may_send(in_flight_bytes(block))) {
                    break;  // the window allows no more: the next ACK runs the pass again
                }
                const SentNode& node = send_pool_.node(index);
                if (!node.tried) {
                    ++context_.counters.fast_retransmits;
                }
                if (!resend(block, index) || node.waiting) {
                    break;  // refused, or waiting for its packet: the timer or the flush tries again
                }
                left += unacknowledged(block, node);
                if (index == block.queue.head()) {
                    restart_rto(block);
                }
                index = block.queue.first_lost(send_pool_);  // the resend that left cleared this one's mark
            }
            return left;
        }

        /// Marks the head lost; nothing when the queue is empty or nothing is in flight, nor for a head marked
        /// received (`RetransmissionQueue::mark_lost`), which the timer has.
        void mark_head_lost(ControlBlock& block) noexcept {
            if (in_flight(block)) {
                block.queue.mark_lost(send_pool_, block.queue.head());
            }
        }

        /**
         * The timeout's marks, Linux's `tcp_timeout_mark_lost` and RFC 8985 6.3's `RACK_mark_losses_on_RTO`: the head,
         * and on a connection that uses SACK every other entry not reported received and last sent at least RACK's
         * round trip ago; later ones are left to RACK. Without SACK every entry is lost; the duplicate count is
         * cleared with the loss state (`LossRecovery::on_timeout`).
         *
         * A head still marked received means the peer reneged on what it reported, counted in `sack_reneging`: every
         * received mark is cleared first and everything is lost, so the expiry never resends an entry the scoreboard
         * holds as received. A byte a block had reported, and so counted as delivered, is counted again when a
         * cumulative ACK covers it (`RetransmissionQueue::delivered`), as Linux's `tp->delivered` counts it again once
         * `tcp_timeout_mark_lost` cleared its mark: the peer did discard it, and delivers it again. An entry that never
         * left, a FIN held for the peer's window, is not in flight and is not marked; entries leave in order, so none
         * behind it left either.
         */
        void mark_timeout(ControlBlock& block) noexcept {
            const bool reneged = block.queue.head_reported(send_pool_);
            if (reneged) {
                block.queue.clear_all_reported(send_pool_);
                ++context_.counters.sack_reneging;
            }
            const bool by_age = block.sack.in_use() && !reneged;
            for (std::uint32_t index = block.queue.head(); index != no_node; index = send_pool_.node(index).next) {
                const SentNode& node = send_pool_.node(index);
                if (node.transmits == 0) {
                    break;
                }
                if (by_age && index != block.queue.head() && context_.now - node.sent < block.rack.round_trip()) {
                    continue;  // sent less than a round trip ago: RACK's to judge
                }
                block.queue.mark_lost(send_pool_, index);
            }
        }

        /**
         * An entry was marked lost on a connection that is open, or a resend was found lost again in the loss state:
         * recovery begins, counted. From the loss state too the policy is told of the congestion and sets a new
         * target, as Linux's `tcp_enter_recovery` does there. `delivered` is what the ACK that showed it delivered;
         * zero at a timer.
         *
         * The policy hears of the congestion and sets its target from the flight, SND.NXT - SND.UNA, less what was
         * sent beyond its window: RFC 5681 3.2 step 2 leaves the data limited transmit sent out of that calculation
         * (`LossRecovery::begin_recovery` applies the cap). RFC 9937 6.1's RecoverFS is the whole flight, less the
         * bytes blocks reported received, plus what this ACK delivered, which on a connection that uses SACK is what
         * it newly reported and newly acknowledged; without SACK no block reports anything and a duplicate's segment
         * is an estimate, so RecoverFS is the flight. Then PRR's first step, with what is in flight after the marking:
         * RFC 9937 6.2 with what the ACK delivered, counted no further than RecoverFS; with nothing delivered, at a
         * timer or on an ACK that reported nothing new, no step runs and one segment may leave.
         */
        void begin_recovery(ControlBlock& block, const std::uint32_t delivered) noexcept {
            const std::uint32_t flight = block.send.flight();
            const std::uint16_t mss    = block.mss();
            const std::uint32_t recover_fs =
                block.sack.in_use() ? flight - block.queue.reported_bytes() + delivered : flight;
            block.recovery.begin_recovery(block.send.nxt, flight, recover_fs, mss);
            ++context_.counters.recoveries;
            if (delivered == 0) {
                block.recovery.on_recovery_timer(in_flight_bytes(block), mss);
            } else {
                block.recovery.on_recovery_ack(
                    std::min(delivered, recover_fs), in_flight_bytes(block), /*safe=*/false, mss);
            }
        }

        /// The queue frees what `ack` covers (`RetransmissionQueue::acknowledge`), `each` running for every entry it
        /// frees that no block reported; a resend that waited is moot once its entry is freed, and the connection
        /// leaves the deferred list unless another of its entries still waits. Called before SND.UNA moves.
        template <std::invocable<const SentNode&> Each>
        [[nodiscard]] Freed acknowledge(ControlBlock& block, const Sequence ack, Each&& each) noexcept {
            const Freed freed = block.queue.acknowledge(send_pool_, ack, block.send.una, std::forward<Each>(each));
            settle_deferred(block);
            return freed;
        }

        /// The entries `sack_block` covers whole marked received (`RetransmissionQueue::report`); one whose resend
        /// waited for its packet waits no more.
        template <std::invocable<const SentNode&> Each>
        [[nodiscard]] Reported report(ControlBlock& block, const wire::TcpSackBlock& sack_block, Each&& each) noexcept {
            const Reported reported = block.queue.report(send_pool_, sack_block, std::forward<Each>(each));
            settle_deferred(block);
            return reported;
        }

        /// Whether the option's `first` block is a believed duplicate report (`RetransmissionQueue::duplicate_report`),
        /// against the largest window the peer offered and the connection's MSS.
        [[nodiscard]] bool duplicate_report(ControlBlock& block,
                                            const wire::TcpSackBlock& first,
                                            const std::optional<wire::TcpSackBlock>& second,
                                            const Sequence ack) noexcept {
            return block.queue.duplicate_report(first, second, ack, block.send.max_wnd, block.mss());
        }

        /// `sack_block` names a resent entry a block reported (`RetransmissionQueue::names_reported_resend`).
        [[nodiscard]] bool names_reported_resend(const ControlBlock& block,
                                                 const wire::TcpSackBlock& sack_block) const noexcept {
            return block.queue.names_reported_resend(send_pool_, sack_block);
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

        /// The head is marked received (`RetransmissionQueue::head_reported`): after an ACK that moved SND.UNA, the
        /// peer reneged on what it reported.
        [[nodiscard]] bool head_reported(const ControlBlock& block) const noexcept {
            return block.queue.head_reported(send_pool_);
        }

        /// RACK's detection over the queue (`Rack::detect`) with the connection's reordering window; nothing without
        /// SACK. Counts `rack_marked` and `lost_retransmits`.
        [[nodiscard]] Rack::Detected detect(ControlBlock& block) noexcept {
            if (!block.sack.in_use()) {
                return {};
            }
            const Rack::Detected detected =
                block.rack.detect(block.queue, send_pool_, block.reordering_window(), context_.now);
            context_.counters.rack_marked      += detected.marked;
            context_.counters.lost_retransmits += detected.resends_lost;
            return detected;
        }

        /**
         * What is in flight: the flight less what is reported received, or, without SACK, less one segment per
         * duplicate ACK capped so a hole of one segment stays, and less what is lost and not resent. Linux's
         * `tcp_packets_in_flight` in bytes. The flight is SND.NXT - SND.UNA, so a FIN that waits for the peer's window
         * counts its one byte.
         *
         * With SACK this is RFC 9937 6.2's estimate. Without SACK it departs from it, where the RFC is definite:
         * senders "MUST subtract out: min(RecoverFS, 1 SMSS for each preceding duplicate ACK in the fast recovery
         * episode)". Linux's emulation is followed instead (`tcp_add_reno_sack`, `tcp_remove_reno_sacks`,
         * `tcp_limit_reno_sacked`): the count of duplicates gives up, on an ACK that moves SND.UNA, the segments that
         * ACK covered (`LossRecovery::remove_duplicates`), and what the duplicates stand for and the holes, the lost
         * bytes or one segment at the least, never pass the flight. The RFC's count runs over the whole episode, so
         * after a partial ACK it subtracts again what the advance already took out of the flight and under-reads what
         * is in flight by the segments between the holes; Linux's removal on an advance is exact for those.
         */
        [[nodiscard]] std::uint32_t in_flight_bytes(const ControlBlock& block) const noexcept {
            const std::uint32_t flight    = block.send.flight();
            const std::uint32_t in_flight = block.queue.in_flight_bytes(flight);
            if (block.sack.in_use()) {
                return in_flight;
            }
            const std::uint32_t holes = std::max<std::uint32_t>(block.queue.lost_bytes(), block.mss());
            return in_flight -
                   std::min(block.recovery.duplicate_bytes(block.mss()), flight > holes ? flight - holes : 0);
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
        /// A pure ACK at `snd_nxt`, stating the edge `edge` names.
        [[nodiscard]] bool send_pure_ack(ControlBlock& block, const Edge edge = Edge::Offer) noexcept {
            if (!send_bare_ack(block, block.send.nxt, edge, /*pure=*/true)) {
                return false;
            }
            ++context_.counters.pure_acks_sent;
            return true;
        }

        /// abort's RST, at `snd_nxt` with the ACK, window and timestamps an ordinary segment carries (RFC 7323: should
        /// carry them, as Linux's does): through `Ipv4::send` and forgotten, since nothing of it is kept.
        [[nodiscard]] bool send_reset(ControlBlock& block) noexcept {
            const Sequence next              = block.receive.next();
            const ReceiveWindow::Offer offer = offer_of(block, Edge::Offer);
            const OutgoingSegment reset{
                .header     = wire::TcpHeader{.source_port      = block.local.port,
                                              .destination_port = block.remote.port,
                                              .sequence         = block.send.nxt,
                                              .acknowledgement  = next,
                                              .flags            = wire::TcpFlags{wire::TcpFlag::Rst, wire::TcpFlag::Ack},
                                              .window           = offer.field},
                .timestamps = timestamp_option(block)
            };
            if (!writer_.send_detached(block.remote.address, reset, context_.now)) {
                return false;
            }
            record_offer(block, offer, Edge::Offer, /*pure=*/false);
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
        /// The RTO from now, its moment kept in `rto_at` (`rto_deadline`).
        void arm_retransmit(ControlBlock& block) noexcept {
            arm_rto_at(block, rto_deadline(block, block.rtt.rto()));
        }

        /**
         * The send timer after an ACK or an expiry, the last step of an acknowledgement: the reordering timer for
         * `wait` when it is not zero, never past `rto_at`; else the RTO while something is in flight; else nothing.
         * The tail probe's place is between the two, so that one place chooses the kind.
         *
         * The RTO is where the ACK, the resend pass or a send left it: restarted from now only by what restarts it, an
         * ACK that moved SND.UNA, a resend of the head, a send into an empty queue. A reordering timer no longer
         * wanted, or no timer at all after an expiry, gives way to the RTO at the moment it had, `rto_at`, not to a
         * fresh one. With nothing in flight the timer stays as it is: stopped by the ACK that emptied the queue, or
         * the zero-window probes' or the ARP poll's.
         *
         * No floor under the reordering timer beyond the wheel's tick, which rounds its deadline up: Linux's
         * `tcp_rack_mark_lost` adds 2 ms (`TCP_TIMEOUT_MIN_US`) to the wait, where RFC 8985 leaves the choice.
         */
        void set_timer(ControlBlock& block, const core::Duration wait) noexcept {
            if (wait > core::Duration::zero()) {
                block.timer_kind = TimerKind::Reorder;
                wheel_.arm(block.timer, std::min(context_.now + wait, block.rto_at));
                return;
            }
            if (in_flight(block) && block.timer_kind != TimerKind::Retransmit) {
                arm_rto_at(block, block.rto_at);
            }
        }

        /// Reneging: the RTO kind at `max(srtt / 2, 10 ms)` from now, under the user timeout as the RTO is; Linux's
        /// `tcp_check_sack_reneging`. When it fires with the head still reported, the timeout clears every received
        /// mark (`mark_timeout`).
        void arm_reneging(ControlBlock& block) noexcept {
            const core::Duration half = block.rtt.srtt().value_or(core::Duration::zero()) / 2;
            arm_rto_at(block, rto_deadline(block, std::max<core::Duration>(half, reneging_floor)));
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

        /// The delay starts with the first segment the pending ACK covers and is not extended: arms only an unarmed
        /// timer.
        void arm_ack(ControlBlock& block) noexcept {
            if (!block.ack_timer.armed()) {
                wheel_.arm(block.ack_timer, context_.now + context_.config.delayed_ack);
            }
        }

        /// No delayed ACK pending any more; one branch and no wheel operation when none was.
        void disarm_ack(ControlBlock& block) noexcept {
            if (block.ack_timer.armed()) {
                wheel_.cancel(block.ack_timer);
            }
        }

        /// The delay ran out: the pure ACK now, counted in `delayed_acks_sent`; a refused one waits for the flush.
        [[nodiscard]] bool send_delayed_ack(ControlBlock& block) noexcept {
            if (!send_pure_ack(block)) {
                queue_ack(block);
                return false;
            }
            ++context_.counters.delayed_acks_sent;
            return true;
        }

        /**
         * The connection's timer fired, unarmed by the wheel; its kind says what for. The RTO marks what it holds lost
         * and resends the head, the reordering timer runs RACK's detection again and resends what it marks, the ARP
         * poll tries again the send that never left, the probe timer probes, and each arms what guards the connection
         * next. Says what is left for the caller: nothing, or the end of a connection that the retries, the user
         * timeout or a silent next hop gave up on, which is the Lifecycle's to carry out.
         */
        [[nodiscard]] TimerExpiry on_timer(ControlBlock& block) noexcept {
            const TimerKind kind = std::exchange(block.timer_kind, TimerKind::None);
            assert(kind != TimerKind::None && "the wheel fires only an armed timer");
            switch (kind) {
                case TimerKind::Retransmit:
                    return on_retransmit_timeout(block);
                case TimerKind::Reorder:
                    on_reorder_timeout(block);
                    return TimerExpiry::Handled;
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
        /// The MSS, within what the two windows allow (`room`), in Established or CloseWait with no prepare open, the
        /// queue below the send cap and no entry that owes a resend; else zero.
        [[nodiscard]] std::size_t writable(const ControlBlock& block) const noexcept {
            if (block.prepared || (block.state != State::Established && block.state != State::CloseWait)) {
                return 0;
            }
            if (block.queue.size() >= context_.config.send_segments) {
                return 0;  // the send cap: the ACK that frees an entry raises Writable
            }
            return static_cast<std::size_t>(std::min<std::uint64_t>(block.mss(), room(block)));
        }

        /**
         * A span of `min(count, writable())` bytes inside a fresh packet. The packet belongs to the application until
         * `commit` (with any count) or `release`: the span stays valid through any state change in between. Nothing,
         * without allocating, for a count of zero or nothing writable; an allocation failure counts one
         * `allocation_failures`, marks a retry hint and retains nothing. An offer the congestion window cut short is
         * a send the window held back, whatever is committed of it (`record_held_back`).
         */
        [[nodiscard]] std::optional<std::span<std::byte>> prepare(ControlBlock& block,
                                                                  const std::size_t count) noexcept {
            assert(!block.prepared && "one prepare at a time: commit first");
            if (count == 0) {
                return std::nullopt;
            }
            const std::size_t room = writable(block);
            if (room < std::min<std::size_t>(count, block.mss())) {
                record_held_back(block, count);
            }
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
         * again for what `writable()` allows. No queue node free refuses it with a hint too. A segment that left in
         * recovery counts against the rate reduction (`LossRecovery::on_sent`), and every segment that left is recorded
         * for the window's use (`WindowUse::on_send`), limited when the congestion window leaves less than a segment
         * after it.
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
            block.recovery.on_sent(payload);
            // Limited when the congestion window leaves less than a segment after this send: Linux's "in flight >=
            // cwnd" (`tcp_write_xmit`), which counts segments, stated in bytes. Not `!may_send`: in bytes a window can
            // keep room for part of a segment, and a sender that waits for room for a whole message, with no prepare
            // for the window to cut short, would never be recorded as limited, and its window would never grow again
            // in congestion avoidance. Only the congestion window's room counts, not the peer's.
            const std::uint32_t in_flight = in_flight_bytes(block);
            block.window_use.on_send(
                block.send.una, block.send.nxt, in_flight, block.recovery.room(in_flight) < block.mss());
            ++context_.counters.data_segments_sent;
            return true;
        }

        /// What a segment with an ACK states now.
        [[nodiscard]] ReceiveWindow::Offer offer_of(const ControlBlock& block, const Edge edge) const noexcept {
            const Sequence next = block.receive.next();
            if (edge == Edge::Restate) {
                return block.window.restate(next);
            }
            return block.window.offer(next, block.receive.unread(), context_.slot_free(block));
        }

    private:
        /// A Writable hint at the next process, at most one per marked connection.
        void mark_retry(ControlBlock& block) noexcept {
            if (!retries_.contains(block)) {
                retries_.push_back(block);
            }
        }

        /// `writable()` rose outside an ACK, above `before`, its value before a resend at the flush or at the timer:
        /// that resend sent the last entry that owed one, and new data is offered again (`room`). The stream
        /// contract owes a Writable for every rise; an ACK's is raised as it is processed, this one as the hint a
        /// refused commit leaves, at the next process. Nothing when it did not rise, which is always so on a
        /// connection that cannot write.
        void hint_if_writable_rose(ControlBlock& block, const std::size_t before) noexcept {
            if (writable(block) > before) {
                mark_retry(block);
            }
        }

        using AckList      = ConnectionList<ControlBlock, &ControlBlock::ack_link>;
        using RetryList    = ConnectionList<ControlBlock, &ControlBlock::retry_link>;
        using DeferredList = ConnectionList<ControlBlock, &ControlBlock::deferred_link>;

        /// The shortest wait before a reneging peer's head is resent: Linux's 10 ms (`tcp_check_sack_reneging`).
        static constexpr core::Duration reneging_floor = std::chrono::milliseconds{10};

        /// The deferred list holds exactly the connections one of whose entries waits for its packet
        /// (`RetransmissionQueue::waiting`): settled after anything that may start or end a wait, a resend, a block's
        /// report or a cumulative ACK. Whether a given entry waits is read from the entry, never from the list.
        void settle_deferred(ControlBlock& block) noexcept {
            if (!block.queue.waiting()) {
                deferred_.remove(block);
            } else if (!deferred_.contains(block)) {
                deferred_.push_back(block);
            }
        }

        /// The handshake is under way: the policy takes its first window at Established.
        [[nodiscard]] static bool handshaking(const ControlBlock& block) noexcept {
            return block.state == State::SynSent || block.state == State::SynReceived;
        }

        /// What a resend of `node` carries: the entry from SND.UNA on when SND.UNA lies inside it, where a copy is cut
        /// and to which a resend in place was trimmed.
        [[nodiscard]] static std::uint32_t unacknowledged(const ControlBlock& block, const SentNode& node) noexcept {
            const Sequence from = block.send.una.after(node.first) ? block.send.una : node.first;
            return static_cast<std::uint32_t>(node.first + node.length - from);
        }

        /**
         * What the two windows allow beyond what is out, in 64 bits: the peer's window against the sequence space
         * outstanding, and the congestion window against what is in flight (`LossRecovery::room`), each floored at
         * zero. This is where RFC 3042's limited transmit comes from: a report of data received, or a duplicate ACK,
         * lowers what is in flight and so lets new data out; Linux has no separate allowance either (`tcp_cwnd_test`).
         *
         * Nothing while an entry is marked lost and not resent. Such an entry is not in flight, so its share of the
         * window would otherwise be offered to new data, which would then hold the window against the resend: at the
         * flush, when its packet comes back, and at the next ACK. An entry that owes a resend goes before anything
         * new, lowest sequence first, as Linux runs `tcp_xmit_retransmit_queue` before it sends new data.
         */
        [[nodiscard]] std::uint64_t room(const ControlBlock& block) const noexcept {
            if (block.queue.lost_bytes() > 0) {
                return 0;
            }
            return std::min(peer_room(block), block.recovery.room(in_flight_bytes(block)));
        }

        /// What the peer's window allows beyond the sequence space outstanding, floored at zero.
        [[nodiscard]] static std::uint64_t peer_room(const ControlBlock& block) noexcept {
            const std::uint32_t flight = block.send.flight();
            return block.send.wnd > flight ? block.send.wnd - flight : 0;
        }

        /**
         * A prepare of `count` was offered less than it asked, one segment at most. When the congestion window, and
         * not the peer's, cut it short, its room below what the peer's window and the segment would allow, the send is
         * recorded as one the window limited (`WindowUse::on_send`), with what is in flight before it. A commit of any
         * part of the offer would be recorded as limited too, less than a segment being left after it; this record
         * stands when nothing is committed. Linux's `tcp_write_xmit` sets `is_cwnd_limited` for a send its congestion
         * window held back (`tcp_tso_should_defer`, and a window with no room left) and records it even when nothing
         * left. Only where data may be sent: before Established the policy has no window.
         */
        void record_held_back(ControlBlock& block, const std::size_t count) noexcept {
            if (block.state != State::Established && block.state != State::CloseWait) {
                return;
            }
            const std::uint32_t in_flight = in_flight_bytes(block);
            if (block.recovery.room(in_flight) < std::min<std::uint64_t>({count, block.mss(), peer_room(block)})) {
                block.window_use.on_send(block.send.una, block.send.nxt, in_flight, /*limited=*/true);
            }
        }

        /**
         * A segment with ACK left stating `offer`: the edge is recorded and the ACK that waited is done, unless one of
         * two things it could not carry is still owed, either of which keeps a connection waiting on the ACK list
         * there. A restating ACK does not do the work of an ordinary one that would move the edge: a connection whose
         * ordinary offer now differs from the edge just restated stays, so the flush still sends the update `consume`
         * owes; one whose ordinary offer is the same edge is satisfied, so the flush sends no identical ACK, which the
         * peer would count as a duplicate. A segment that is not a pure ACK carries no SACK option: a connection with
         * blocks to report or a duplicate pending stays, so the flush still sends them on a pure ACK; so does one whose
         * delayed ACK owed them, which waited on its timer and not on the list. Either way the segment acknowledged
         * RCV.NXT, so a delayed ACK pending is done: its timer is cancelled.
         */
        void record_offer(ControlBlock& block,
                          const ReceiveWindow::Offer& offer,
                          const Edge edge,
                          const bool pure) noexcept {
            if (block.window.record(offer)) {
                ++context_.counters.window_updates;
            }
            block.ack_policy.acknowledged();
            const bool delayed = block.ack_timer.armed();
            disarm_ack(block);
            if (edge == Edge::Restate && acks_.contains(block) && offer_of(block, Edge::Offer).edge != offer.edge) {
                return;  // the update is still owed to the flush
            }
            if (!pure && block.sack.pending()) {
                if (delayed) {
                    queue_ack(block);  // the delayed ACK would have carried them
                }
                return;  // the blocks are still owed to the flush, on a pure ACK
            }
            acks_.remove(block);
        }

        /// The window scale a handshake segment offers: in SynSent the stack's shift when the configuration offers
        /// scaling, else the shift the connection uses, when both ends offered it.
        [[nodiscard]] std::optional<std::uint8_t> window_scale_option(const ControlBlock& block) const noexcept {
            if (block.state == State::SynSent) {
                return context_.config.window_scaling ? std::optional{context_.window_shift} : std::nullopt;
            }
            return block.window.scale();
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
         * option block carries the timestamps on every segment of a connection that sends them, and on a handshake
         * segment the MSS, SACK-permitted while the connection offers or uses it, and our window scale: on a SYN when
         * the configuration offers scaling, on a SYN-ACK when both ends do. A resent SYN or SYN-ACK carries the same.
         * The window: a SYN states the stack's `syn_window` and records nothing, a SYN-ACK the connection's budget
         * unscaled (`ReceiveWindow::handshake`), every other segment with ACK what `offer_of` gives for `edge`. A pure
         * ACK carries the SACK option while blocks are held or a duplicate is pending (`SackBlocks::report`): three
         * blocks beside the timestamps, four without. The packet stays with the caller. Only a success records the
         * offer and Last.ACK.sent, clears the pending ACK and counts the report, whose duplicate is then forgotten; a
         * failure, counted by the writer, leaves the packet framed and the report owed.
         */
        [[nodiscard]] std::expected<void, net::SendError> emit(ControlBlock& block,
                                                               Packet& packet,
                                                               const bool framed,
                                                               const Sequence sequence,
                                                               const wire::TcpFlags flags,
                                                               const bool handshake,
                                                               const Edge edge = Edge::Offer,
                                                               const bool pure = false) noexcept {
            const bool with_ack              = flags.has(wire::TcpFlag::Ack);
            const Sequence next              = block.receive.next();
            const ReceiveWindow::Offer offer = !with_ack   ? ReceiveWindow::Offer{.field = context_.syn_window}
                                               : handshake ? block.window.handshake(next)
                                                           : offer_of(block, edge);
            const OutgoingSegment segment{
                .header         = wire::TcpHeader{.source_port      = block.local.port,
                                                  .destination_port = block.remote.port,
                                                  .sequence         = sequence,
                                                  .acknowledgement  = with_ack ? next : Sequence{},
                                                  .flags            = flags,
                                                  .window           = offer.field},
                .mss            = handshake ? std::optional{context_.mss}
                          : std::nullopt,
                .timestamps     = timestamp_option(block),
                .sack_permitted = handshake && block.sack.in_use(),
                .window_scale   = handshake ? window_scale_option(block) : std::nullopt
            };
            const bool reporting = pure && block.sack.pending();
            const std::expected<void, net::SendError> sent =
                reporting ? send_reporting(block, packet, framed, segment)
                          : writer_.send(block.frame, block.remote.address, packet, framed, segment, context_.now);
            if (sent && with_ack) {
                if (reporting) {
                    ++context_.counters.sack_acks_sent;
                    if (block.sack.duplicate_pending()) {
                        ++context_.counters.duplicate_sacks_sent;
                    }
                    block.sack.reported();
                }
                record_offer(block, offer, handshake ? Edge::Offer : edge, pure);
                block.timestamps.sent_ack(next);
            }
            return sent;
        }

        /// `ack`, a pure ACK, sent with the SACK option it owes: three blocks beside the timestamps, four without. The
        /// blocks and the segment that points at them live here, so only an ACK that reports touches their storage and
        /// nothing outlives it.
        [[nodiscard]] std::expected<void, net::SendError>
        send_reporting(ControlBlock& block, Packet& packet, const bool framed, const OutgoingSegment& ack) noexcept {
            std::array<wire::TcpSackBlock, SackBlocks::capacity> blocks{};
            const std::size_t count = block.sack.report(blocks, block.timestamps.enabled() ? 3 : 4);
            OutgoingSegment reporting{ack};
            reporting.sack = std::span<const wire::TcpSackBlock>{blocks}.first(count);
            return writer_.send(block.frame, block.remote.address, packet, framed, reporting, context_.now);
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
                            .app_limited       = room(block) > payload};
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
         * The busy rule, for an entry whose packet the device still holds after a reclaim. A resend at the stamp of the
         * entry's last deferral, while it still waits, is that deferral again, since the device had no time to finish:
         * it goes on waiting and counts nothing. Below `retransmit_defers` deferrals the resend waits for the next
         * flush (`RetransmissionQueue::mark_waiting`), counted once per deferral; at the bound, with no copy out, the
         * copy goes (`send_copy`); with a copy out and both packets lent, it waits again.
         */
        [[nodiscard]] std::expected<void, net::SendError> resend_busy(ControlBlock& block,
                                                                      const std::uint32_t index) noexcept {
            SentNode& node = send_pool_.node(index);
            if (node.waiting && node.deferred_at == context_.now) {
                return {};
            }
            if (node.deferrals < context_.config.retransmit_defers) {
                ++node.deferrals;
            } else if (node.copy.empty()) {
                return send_copy(block, index);
            }
            node.deferred_at = context_.now;
            block.queue.mark_waiting(send_pool_, index);
            ++context_.counters.retransmits_deferred;
            return {};
        }

        /**
         * The copy fallback: a fresh packet with the lent packet's header bytes and the part of its payload not yet
         * acknowledged, read from the lent packet and never written into it (`SegmentWriter::copy`), framed by `emit`
         * from `max(first, snd_una)` and lent as the entry's second packet; counted in `retransmits_copied`. No packet
         * free counts `allocation_failures` and refuses; a refused send drops the copy. Either way the entry stays as
         * it was and waits no more: the next pass or the timer's next expiry tries again.
         */
        [[nodiscard]] std::expected<void, net::SendError> send_copy(ControlBlock& block,
                                                                    const std::uint32_t index) noexcept {
            SentNode& node = send_pool_.node(index);
            assert(node.framed && node.copy.empty() && "one copy at a time, of a frame that left");
            block.queue.clear_waiting(send_pool_, index);
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

        /// A header-only ACK at `sequence`, stating the edge `edge` names, from a fresh packet: lent to the device and
        /// forgotten, never kept. `pure` for the connection's pure ACK, which carries the SACK option it owes; false
        /// for a probe, which owes none. A failure is counted.
        [[nodiscard]] bool
        send_bare_ack(ControlBlock& block, const Sequence sequence, const Edge edge, const bool pure) noexcept {
            std::optional<Packet> packet = context_.ip.allocate();
            if (!packet) {
                ++context_.counters.send_refused;
                return false;
            }
            if (!emit(block, *packet, false, sequence, wire::TcpFlags{wire::TcpFlag::Ack}, false, edge, pure)) {
                return false;  // the packet goes back to the pool
            }
            *packet = Packet{};  // the device frees the buffer when it is done
            return true;
        }

        /// A zero-window probe, Linux's `tcp_xmit_probe_skb`: a pure ACK at `snd_una - 1`, which occupies no sequence
        /// space and which the peer, finding it outside its window, answers with an ACK carrying its window.
        [[nodiscard]] bool send_probe(ControlBlock& block) noexcept {
            if (!send_bare_ack(block, Sequence{block.send.una.value - 1}, Edge::Offer, /*pure=*/false)) {
                return false;
            }
            ++context_.counters.zero_window_probes;
            return true;
        }

        /// The entry the user timeout counts from: the head of the queue once it has left, on a synchronized
        /// connection, with the timeout set. Null otherwise: the handshake is bounded by `retries` alone.
        [[nodiscard]] const SentNode* user_timed_head(const ControlBlock& block) const noexcept {
            if (context_.config.user_timeout <= core::Duration::zero() || handshaking(block) || block.queue.empty()) {
                return nullptr;
            }
            const SentNode& head = send_pool_.node(block.queue.head());
            return head.transmits > 0 ? &head : nullptr;
        }

        /// `after` from now; under a user timeout on a synchronized connection, never later than the head's first
        /// transmission plus the timeout, Linux's `tcp_clamp_rto_to_user_timeout`, and never earlier than now.
        [[nodiscard]] core::TimePoint rto_deadline(const ControlBlock& block,
                                                   const core::Duration after) const noexcept {
            const core::TimePoint deadline = context_.now + after;
            if (const SentNode* head = user_timed_head(block); head != nullptr) {
                return std::min(deadline, std::max(context_.now, head->first_sent + context_.config.user_timeout));
            }
            return deadline;
        }

        /// The RTO kind at `deadline`, which becomes the RTO's moment, `rto_at`.
        void arm_rto_at(ControlBlock& block, const core::TimePoint deadline) noexcept {
            block.rto_at     = deadline;
            block.timer_kind = TimerKind::Retransmit;
            wheel_.arm(block.timer, deadline);
        }

        /// The RTO from now, after a resend of the head; while the reordering timer is pending it stays, and only the
        /// RTO's moment moves, which the reordering timer never passes and gives way to: Linux's
        /// `tcp_xmit_retransmit_queue` leaves a pending `ICSK_TIME_REO_TIMEOUT` alone, and its RTO, rearmed later by
        /// `tcp_rearm_rto`, counts from the head's last transmission.
        void restart_rto(ControlBlock& block) noexcept {
            if (block.timer_kind == TimerKind::Reorder) {
                block.rto_at = rto_deadline(block, block.rtt.rto());
                return;
            }
            arm_retransmit(block);
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
         * The RTO ran out: one retry. First the user timeout, counted from the head's first transmission on a
         * synchronized connection, and then the cap, `retries` in the handshake and `data_retries` after it, may give
         * the connection up instead. Else the expiry marks what it holds lost (`mark_timeout`), puts a synchronized
         * connection in the loss state (`LossRecovery::on_timeout`), where the policy's window restarts at what is in
         * flight after the marking plus one segment, and resends the head, one segment and no more, as Linux's
         * `tcp_retransmit_timer` does; the other entries marked leave by the resend pass as ACKs open the window. The
         * handshake's expiry is not the policy's concern: it takes its first window at Established. Each expiry
         * doubles the RTO up to `rto_max` and re-arms. A resend deferred or refused counts as the retry all the same:
         * a deferred one, marked lost, leaves at the flush (`flush_deferred`), and the next expiry tries again. A
         * first send, of a SYN, SYN-ACK or FIN the device refused before, that finds no next hop arms the ARP poll
         * instead (`transmit_entry`), and the expiry leaves the timer to it: nothing left, so no backoff; the poll arms
         * the RTO once the send leaves, or gives the connection up as `Unresolved`. An expiry whose resend sends the
         * last entry that owed one, a head that had waited or been refused since the expiry before, may leave room
         * for new data: the connection gets its Writable hint (`hint_if_writable_rose`).
         */
        [[nodiscard]] TimerExpiry on_retransmit_timeout(ControlBlock& block) noexcept {
            assert(!block.queue.empty() && "the RTO runs while something is queued");
            if (const SentNode* head = user_timed_head(block);
                head != nullptr && context_.now - head->first_sent >= context_.config.user_timeout) {
                return TimerExpiry::UserTimedOut;
            }
            const bool handshake = handshaking(block);
            if (block.retransmit_count >= (handshake ? context_.config.retries : context_.config.data_retries)) {
                return TimerExpiry::TimedOut;
            }
            ++block.retransmit_count;
            const std::size_t writable_before = writable(block);
            mark_timeout(block);
            if (!handshake) {
                block.recovery.on_timeout(block.send.una,
                                          block.send.nxt,
                                          block.send.flight(),
                                          in_flight_bytes(block),
                                          block.mss(),
                                          block.retransmit_count == 1);
            }
            std::ignore = resend(block, block.queue.head());
            ++context_.counters.retransmits;
            hint_if_writable_rose(block, writable_before);
            if (block.timer_kind == TimerKind::ArpPoll) {
                return TimerExpiry::Handled;  // a first send that found no next hop: the poll retries it
            }
            block.rtt.back_off(context_.config.rto_max);
            arm_retransmit(block);
            return TimerExpiry::Handled;
        }

        /**
         * The reordering timer ran out, Linux's `tcp_rack_reo_timeout`: detection runs again (`detect`), counted in
         * `reorder_timeouts`. When it marked something on a connection not in recovery, recovery begins there, from
         * the loss state too, which sets a new target, with no ACK to step PRR: one segment may leave
         * (`begin_recovery` with nothing delivered). In recovery the episode that is open goes on, and one segment may
         * leave (`LossRecovery::on_recovery_timer`). Then the resend pass, and the RTO at the moment it had
         * (`set_timer`): entries still waiting are left to the next ACK. An expiry is no retry: it moves neither the
         * retry count nor the backoff, and the user timeout bounds it through the RTO's moment, which it never passes.
         * A resend that clears the last lost mark may leave room for new data: the connection gets its Writable hint
         * (`hint_if_writable_rose`).
         *
         * Where this differs from `tcp_rack_reo_timeout`: entering recovery, Linux steps its rate reduction as if one
         * segment were delivered, which allows the same one segment; in recovery it steps nothing and its window
         * stands, where here one segment may leave all the same, so an expiry that marks the episode's next loss lets
         * its resend out without waiting for an ACK.
         */
        void on_reorder_timeout(ControlBlock& block) noexcept {
            ++context_.counters.reorder_timeouts;
            const std::size_t writable_before = writable(block);
            if (const Rack::Detected detected = detect(block); detected.marked > 0) {
                if (!block.recovery.in_recovery()) {
                    begin_recovery(block, 0);
                } else {
                    block.recovery.on_recovery_timer(in_flight_bytes(block), block.mss());
                }
            }
            std::ignore = resend_lost(block);
            hint_if_writable_rose(block, writable_before);
            set_timer(block, core::Duration::zero());
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
        DeferredList deferred_ = {};  ///< Exactly the connections one of whose entries waits for its packet.
        SegmentWriter<Ip> writer_;    ///< Counts into the context's counters.
    };

}  // namespace aloe::tcp::detail
