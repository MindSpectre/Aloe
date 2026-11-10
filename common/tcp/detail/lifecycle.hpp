#pragma once

#include <aloe/core>
#include <aloe/net>
#include <aloe/stream>
#include <aloe/wire>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <tuple>
#include <utility>

#include <detail/ack_policy.hpp>
#include <detail/connection_table.hpp>
#include <detail/control_block.hpp>
#include <detail/isn_generator.hpp>
#include <detail/stack_context.hpp>
#include <detail/transmitter.hpp>
#include <detail/zero_window_probe.hpp>
#include <flow_table.hpp>
#include <tcp_config.hpp>
#include <tcp_fwd.hpp>

namespace aloe::tcp::detail {

    /**
     * @brief A connection's life: the slot taken and bound, the handshake's numbers drawn, every change of state
     * from the open to `Closed`, the events raised, the timer expiries that resend or give up, `close`, `abort` and
     * `release`. It owns the connection table, the ISN generator and the event list, and calls down into the
     * Transmitter. No other part writes a connection's state: the Receiver says what arrived and the Lifecycle
     * moves the connection on.
     */
    template <typename Ip, typename CongestionControl>
    class Lifecycle {
    public:
        using Block       = ControlBlock<Ip, CongestionControl>;
        using Context     = StackContext<Ip, CongestionControl>;
        using Transmitter = detail::Transmitter<Ip, CongestionControl>;
        using Sequence    = wire::TcpSequence;

        Lifecycle(Context& context, Transmitter& transmitter)
            : context_{context},
              transmitter_{transmitter},
              table_{context.config, context.ip.queue().steering()} {
        }

        // The directory.
        [[nodiscard]] std::expected<void, ListenError> listen(const std::uint16_t port) noexcept {
            return table_.listen(port);
        }

        /// Connections already open stay open.
        void unlisten(const std::uint16_t port) noexcept {
            table_.unlisten(port);
        }

        [[nodiscard]] bool listening(const std::uint16_t port) const noexcept {
            return table_.listening(port);
        }

        [[nodiscard]] std::size_t table_size() const noexcept {
            return table_.size();
        }

        /// The table, for the Receiver's lookup and base hash.
        [[nodiscard]] const ConnectionTable& table() const noexcept {
            return table_;
        }

        /// For tests: the next connection opened takes this initial sequence number instead of a drawn one.
        void force_next_isn(const Sequence isn) noexcept {
            isn_.force(isn);
        }

        // Opening.
        /// A SynSent block the caller owns; its SYN is queued, or waits for ARP on the timer.
        [[nodiscard]] std::expected<Block*, ConnectError> connect(const Endpoint peer) noexcept {
            if (!context_.ip.next_hop(peer.address)) {
                return std::unexpected{ConnectError::NoRoute};
            }
            if (transmitter_.nodes_available() == 0) {
                return std::unexpected{ConnectError::TableFull};  // no retransmission-queue node for the SYN
            }
            const std::expected<Placement, ConnectError> placed =
                table_.place(peer, context_.ip.address(), context_.ip.queue().steering(), context_.ip.queue().index());
            if (!placed) {
                return std::unexpected{placed.error()};
            }
            const std::optional<std::uint32_t> slot = table_.take_slot();
            if (!slot) {
                return std::unexpected{ConnectError::TableFull};
            }
            Block& block = context_.blocks[*slot];
            bind_flow(block, Endpoint{.address = context_.ip.address(), .port = placed->port}, peer, placed->base_hash);
            block.state = State::SynSent;
            block.owned = true;
            open(block);
            // Arms the timer on a send or an ARP wait; a refused send leaves the SYN to the RTO.
            std::ignore = transmitter_.queue_control(block, wire::TcpFlags{wire::TcpFlag::Syn}, block.send.iss);
            if (!block.timer.armed()) {
                transmitter_.arm_retransmit(block);
            }
            return &block;
        }

        /// A slot for a SYN a listener took: bound, SynReceived, not owned, opened; null when the table is full.
        [[nodiscard]] Block*
        accept_slot(const Endpoint local, const Endpoint remote, const std::uint32_t base_hash) noexcept {
            const std::optional<std::uint32_t> slot = table_.take_slot();
            if (!slot) {
                return nullptr;
            }
            Block& block = context_.blocks[*slot];
            bind_flow(block, local, remote, base_hash);
            block.state = State::SynReceived;
            block.owned = false;
            open(block);
            return &block;
        }

        /// The active open was answered: Established, counted, `Connected` raised.
        void connected(Block& block) noexcept {
            assert(block.state == State::SynSent && "only a SYN of ours is answered");
            establish(block);
            ++context_.counters.connections_opened;
            raise(block, stream::Event::Connected);
        }

        /// The passive open's SYN-ACK was acknowledged: Established, and the application's from `Accepted` on.
        void accepted(Block& block) noexcept {
            assert(block.state == State::SynReceived && "only a passive open is accepted");
            establish(block);
            block.owned = true;
            ++context_.counters.connections_accepted;
            raise(block, stream::Event::Accepted);
        }

        // Events.
        void raise(Block& block, const stream::Event event) noexcept {
            block.events |= event;
            if (!events_.contains(block)) {
                events_.push_back(block);
            }
        }

        [[nodiscard]] Block* pop_event() noexcept {
            return events_.pop_front();
        }

        [[nodiscard]] std::size_t pending_events() const noexcept {
            return events_.size();
        }

        // Closing.
        /// The last ACK the peer is owed must be queued before Closed is published; flush retries the rest.
        void maybe_finish(Block& block) noexcept {
            if (block.finish_pending && !transmitter_.ack_pending(block)) {
                finish(block);
            }
        }

        /// FIN after what was committed: Established moves to FinWait1, CloseWait to LastAck; elsewhere nothing. An
        /// open preparation stays the application's: its next commit refuses it, as the connection left the sendable
        /// states. The FIN's entry is the node Established set aside, so the pool never refuses it. A FIN the peer's
        /// window does not cover, Linux's `tcp_snd_wnd_test`, waits in the queue, unsent, for the ACK that opens the
        /// window (`send_unsent`), while the probes go on; a Linux peer drops a FIN sent into its zero window. The
        /// congestion window does not gate a FIN, as it does not in Linux.
        void close(Block& block) noexcept {
            switch (block.state) {
                case State::Established:
                    block.state = State::FinWait1;
                    break;
                case State::CloseWait:
                    block.state = State::LastAck;
                    break;
                default:
                    return;
            }
            const Sequence fin  = block.send.nxt;
            const bool idle     = !transmitter_.in_flight(block);  // else the RTO already runs for the data
            block.send.fin_sent = true;
            block.send.nxt      = fin + 1;  // the FIN takes its number whether or not it left: the timer resends it
            const std::optional<std::uint32_t> entry = transmitter_.enqueue_fin(block, fin);
            if (!entry) {
                // Unreachable by construction; a release build resets rather than hangs.
                ++context_.counters.send_refused;
                abort(block);
                return;
            }
            if (!block.send.covers(fin + 1)) {
                if (idle && !block.probing()) {
                    // A zero window with nothing in flight is probed already, as a rule.
                    transmitter_.start_probing(block);
                }
                return;
            }
            std::ignore = transmitter_.transmit_entry(block, *entry);
            if (idle && (!block.timer.armed() || block.probing())) {
                transmitter_.arm_retransmit(block);  // refused: the RTO tries again, and takes the timer from a probe
            }
        }

        /**
         * The peer's FIN arrived in order: `PeerClosed` raised and the state moved on. Says when the FIN's ACK goes:
         * at once where our own close is under way, since the close waits on it, and with the next flush otherwise,
         * where the application's close may carry it.
         */
        [[nodiscard]] AckPolicy::Ack peer_closed(Block& block) noexcept {
            raise(block, stream::Event::PeerClosed);
            switch (block.state) {
                case State::Established:
                    block.state = State::CloseWait;
                    return AckPolicy::Ack::Delayed;
                case State::FinWait1:
                    block.state = State::Closing;
                    return AckPolicy::Ack::Now;
                case State::FinWait2:
                    block.finish_pending = true;  // the terminal ACK goes before Closed is published
                    return AckPolicy::Ack::Now;
                default:
                    return AckPolicy::Ack::Delayed;
            }
        }

        /// Our FIN was acknowledged, so the queue is empty: FinWait1 waits for the peer's FIN in FinWait2; Closing and
        /// LastAck are done, and `Closed` follows once no ACK is owed (`maybe_finish`).
        void fin_acknowledged(Block& block) noexcept {
            switch (block.state) {
                case State::FinWait1:
                    block.state = State::FinWait2;
                    break;
                case State::Closing:
                case State::LastAck:
                    block.finish_pending = true;
                    break;
                default:
                    break;
            }
        }

        /// The peer's RST was accepted: a passive open nobody took is freed silently; any other connection is Closed
        /// with `Reset` raised.
        void peer_reset(Block& block) noexcept {
            if (discard_unowned(block)) {
                return;
            }
            ++context_.counters.connections_reset;
            set_closed(block);
            raise(block, stream::Event::Reset);
        }

        void abort(Block& block) noexcept {
            if (block.state == State::Closed) {
                return;
            }
            // RFC 9293's ABORT in SYN-SENT: the peer holds nothing of ours to take back, so no RST leaves.
            if (block.state != State::SynSent) {
                std::ignore = transmitter_.send_reset(block);
            }
            set_closed(block);
            raise(block, stream::Event::Closed);
        }

        void release(Block& block) noexcept {
            if (block.state != State::Closed) {
                abort(block);
            }
            free_slot(block);
        }

        // The timer.
        /// Called by the wheel through the stack's trampoline with the timer unarmed; its kind says what for.
        void on_timer(Block& block) noexcept {
            const TimerKind kind = std::exchange(block.timer_kind, TimerKind::None);
            assert(kind != TimerKind::None && "the wheel fires only an armed timer");
            switch (kind) {
                case TimerKind::Retransmit:
                    on_retransmit_timeout(block);
                    break;
                case TimerKind::ArpPoll:
                    on_arp_poll(block);
                    break;
                case TimerKind::Probe:
                    on_probe_timeout(block);
                    break;
                case TimerKind::None:
                    break;
            }
        }

    private:
        /// Established: the FIN's node set aside and the policy's first window taken from the negotiated MSS.
        void establish(Block& block) noexcept {
            block.state = State::Established;
            transmitter_.reserve_fin_node(block);
            block.recovery.establish(block.mss());
        }

        void free_slot(Block& block) noexcept {
            // Every live connection is indexed by connect or passive_open; a free slot is not, and pushing its index
            // again would hand one slot to two opens. A release build ignores the second call.
            assert(block.indexed && "release of a free slot");
            if (!block.indexed) {
                return;
            }
            transmitter_.disarm(block);
            std::ignore = table_.erase(block.key, block.base_hash);
            return_chain(block);
            transmitter_.drop_queue(block);
            block.prepared.reset();
            events_.remove(block);
            transmitter_.forget(block);
            block.clear();
            table_.free_slot(block.index);
        }

        /// Closed, with nothing armed, pending or queued: after a terminal event nothing is resent, so the
        /// retransmission queue is dropped, its packets back now or when the device returns them. Held segments stay
        /// until release, and so does an open preparation: the application may still write into its span, and the
        /// next commit refuses it.
        void set_closed(Block& block) noexcept {
            if (block.state == State::SynSent) {
                block.timestamps.withdraw();  // the offer was never answered: nothing was negotiated
            }
            block.state = State::Closed;
            transmitter_.disarm(block);
            block.unresolved_polls = 0;
            transmitter_.forget(block);
            block.finish_pending = false;
            transmitter_.drop_queue(block);  // and off the deferred list
        }

        void finish(Block& block) noexcept {
            set_closed(block);
            ++context_.counters.connections_closed;
            raise(block, stream::Event::Closed);
        }

        /// A passive open nobody took ends silently, whatever ended it: counted and its slot freed. True when `block`
        /// was one, and is gone.
        [[nodiscard]] bool discard_unowned(Block& block) noexcept {
            if (block.owned) {
                return false;
            }
            assert(block.state == State::SynReceived && "a connection is unowned only until its handshake completes");
            ++context_.counters.handshakes_failed;
            free_slot(block);
            return true;
        }

        using EventList = ConnectionList<Block, &Block::event_link>;

        /// The flow's identity, indexed in the table under `base_hash`; the caller found the flow free.
        void
        bind_flow(Block& block, const Endpoint local, const Endpoint remote, const std::uint32_t base_hash) noexcept {
            block.local     = local;
            block.remote    = remote;
            block.key       = FlowKey{.remote = remote.address, .remote_port = remote.port, .local_port = local.port};
            block.base_hash = base_hash;
            [[maybe_unused]] const bool inserted = table_.insert(block.key, block.index, base_hash);
            assert(inserted && "the caller found the flow free");
            block.indexed = true;
        }

        /// Numbers the connection's SYN from a fresh ISN and starts its sender from scratch: RFC 6582's `recover` at
        /// the ISN, no round-trip sample, the initial RTO and no retry; makes the timestamps offer.
        void open(Block& block) noexcept {
            block.send.open(isn_.next());
            block.recovery.open(block.send.iss);
            block.rtt.reset(context_.config.rto_initial);
            // The SYN or SYN-ACK offers; the peer decides.
            block.timestamps.offer(context_.config.timestamps, isn_.offset());
            block.retransmit_count = 0;
        }

        void return_chain(Block& block) noexcept {
            block.receive.release(context_.pool);
        }

        /**
         * The RTO ran out: one retry, the head of the queue again. First the user timeout, counted from the head's
         * first transmission on a synchronized connection, and then the cap, `retries` in the handshake and
         * `data_retries` after it, may end the connection instead. The first expiry since an ACK of new data starts
         * the loss episode (`LossRecovery::on_timeout`); each expiry doubles the RTO up to `rto_max` and re-arms. A
         * resend deferred or refused counts as the retry all the same: the next expiry tries again. A first send, of a
         * SYN, SYN-ACK or FIN the device refused before, that finds no next hop arms the ARP poll instead
         * (`transmit_entry`), and the expiry leaves the timer to it: nothing left, so no backoff; the poll arms the RTO
         * once the send leaves, or ends the connection `Unresolved`.
         */
        void on_retransmit_timeout(Block& block) noexcept {
            assert(!block.queue.empty() && "the RTO runs while something is queued");
            if (const typename Transmitter::SentNode* head = transmitter_.user_timed_head(block);
                head != nullptr && context_.now - head->first_sent >= context_.config.user_timeout) {
                time_out(block, /*by_user_timeout=*/true);
                return;
            }
            if (const bool handshake = block.state == State::SynSent || block.state == State::SynReceived;
                block.retransmit_count >= (handshake ? context_.config.retries : context_.config.data_retries)) {
                time_out(block, /*by_user_timeout=*/false);
                return;
            }
            if (++block.retransmit_count == 1) {
                block.recovery.on_timeout(block.send, block.mss());
            }
            std::ignore = transmitter_.resend_head(block);
            ++context_.counters.retransmits;
            if (block.timer_kind == TimerKind::ArpPoll) {
                return;  // a first send that found no next hop: the poll retries it, and arms the RTO once it leaves
            }
            block.rtt.back_off(context_.config.rto_max);
            transmitter_.arm_retransmit(block);
        }

        /// The ARP poll: the SYN, SYN-ACK or FIN that never left is tried again, as no retry. Once it leaves, or the
        /// device refuses it, the RTO takes over; a next hop still silent after `unresolved_polls` polls ends the
        /// connection with `Unresolved`.
        void on_arp_poll(Block& block) noexcept {
            assert(!block.queue.empty() && "the poll retries an entry that never left");
            if (const std::expected<void, net::SendError> sent = transmitter_.transmit_entry(block, block.queue.head());
                sent || sent.error() != net::SendError::Unresolved) {
                transmitter_.arm_retransmit(block);
                return;
            }
            if (++block.unresolved_polls >= context_.config.unresolved_polls) {
                fail_unresolved(block);  // the bound Linux's ARP probes give: the next hop is not going to answer
                return;
            }
            transmitter_.arm_poll(block);
        }

        /// The probe timer ran out with the peer's window still zero (`ZeroWindowProbe::expire`): the user timeout or
        /// `data_retries` unanswered probes end the connection; else one probe goes, a refused one counted all the
        /// same, and the next is armed.
        void on_probe_timeout(Block& block) noexcept {
            assert(!transmitter_.in_flight(block) && block.send.wnd == 0 &&
                   (!block.queue.empty() || block.state == State::Established || block.state == State::CloseWait) &&
                   "a probe runs only while the window is zero and nothing is in flight");
            switch (block.probe.expire(
                context_.now, context_.config.user_timeout, context_.config.data_retries, context_.config.rto_max)) {
                case ZeroWindowProbe::Expiry::UserTimedOut:
                    time_out(block, /*by_user_timeout=*/true);
                    return;
                case ZeroWindowProbe::Expiry::TimedOut:
                    time_out(block, /*by_user_timeout=*/false);
                    return;
                case ZeroWindowProbe::Expiry::Probe:
                    break;
            }
            std::ignore = transmitter_.send_probe(block);  // a refusal was counted by the send
            transmitter_.arm_probe(block);
        }

        /// The retry cap or the user timeout ran out: `TimedOut` for an owned connection, counted apart when the user
        /// timeout ended it.
        void time_out(Block& block, const bool by_user_timeout) noexcept {
            if (discard_unowned(block)) {
                return;
            }
            ++context_.counters.connections_timed_out;
            if (by_user_timeout) {
                ++context_.counters.connections_user_timed_out;
            }
            set_closed(block);
            raise(block, stream::Event::TimedOut);
        }

        /// The next hop never answered ARP: a failure of its own, told apart from a segment that left and was ignored.
        void fail_unresolved(Block& block) noexcept {
            if (discard_unowned(block)) {
                return;
            }
            ++context_.counters.connections_unresolved;
            set_closed(block);
            raise(block, stream::Event::Unresolved);
        }

        Context& context_;
        Transmitter& transmitter_;
        ConnectionTable table_;
        EventList events_ = {};
        IsnGenerator isn_{};
    };

}  // namespace aloe::tcp::detail
