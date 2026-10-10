#pragma once

#include <cassert>
#include <cstdint>
#include <expected>
#include <optional>
#include <tuple>

#include <rss.hpp>
#include <tcp_stack.hpp>

// The control path of tcp::Stack: events, slots, the port choice, the retransmit timer, abort and release.
namespace aloe::tcp {

    template <typename Ip>
    void Stack<Ip>::raise(ConnectionType& c, const stream::Event event) noexcept {
        c.events_ |= event;
        if (!events_.contains(c)) {
            events_.push_back(c);
        }
    }

    template <typename Ip>
    std::optional<std::uint32_t> Stack<Ip>::take_slot() noexcept {
        if (free_.empty()) {
            return std::nullopt;
        }
        const std::uint32_t index = free_.back();
        free_.pop_back();
        return index;
    }

    template <typename Ip>
    void Stack<Ip>::return_chain(ConnectionType& c) noexcept {
        for (std::uint32_t index = c.chain_head_; index != detail::no_node;) {
            const std::uint32_t next = pool_.node(index).next;
            pool_.release(index);
            index = next;
        }
        c.chain_head_   = detail::no_node;
        c.chain_tail_   = detail::no_node;
        c.chain_count_  = 0;
        c.unread_bytes_ = 0;
    }

    template <typename Ip>
    void Stack<Ip>::free_slot(ConnectionType& c) noexcept {
        wheel_->cancel(c.timer_);
        if (c.indexed_) {
            std::ignore = table_.erase(c.key_, c.base_hash_);
        }
        return_chain(c);
        c.prepared_.reset();
        events_.remove(c);
        acks_.remove(c);
        retries_.remove(c);
        c.clear();
        free_.push_back(c.index_);
    }

    template <typename Ip>
    wire::TcpSequence Stack<Ip>::draw_isn() noexcept {
        return Sequence{static_cast<std::uint32_t>(isn_engine_())};
    }

    template <typename Ip>
    std::expected<std::uint16_t, ConnectError> Stack<Ip>::choose_port(const Endpoint peer,
                                                                      std::uint32_t& base_hash) noexcept {
        const device::RssDescription& steering = ip_->queue().steering();
        const std::uint16_t mine               = ip_->queue().index();
        device::FlowTuple tuple{.source           = peer.address,  // received orientation: the peer is the source
                                .destination      = ip_->address(),
                                .source_port      = peer.port,
                                .destination_port = 0,
                                .protocol         = wire::Ipv4Protocol::Tcp};
        if (!hardware_hash_) {
            // Steering off, or addresses only: the queue the replies land on does not depend on the port.
            if (device::queue_for(steering, tuple) != mine) {
                return std::unexpected{ConnectError::Unplaceable};
            }
        }
        const std::uint32_t range = static_cast<std::uint32_t>(config_.ephemeral_last - config_.ephemeral_first) + 1U;
        for (std::uint32_t tried = 0; tried < range; ++tried) {
            const std::uint16_t port = cursor_;
            cursor_ =
                cursor_ == config_.ephemeral_last ? config_.ephemeral_first : static_cast<std::uint16_t>(cursor_ + 1);
            if (listening(port)) {
                continue;
            }
            tuple.destination_port = port;
            if (hardware_hash_ && device::queue_for(steering, tuple) != mine) {
                continue;
            }
            const FlowKey key{.remote = peer.address, .remote_port = peer.port, .local_port = port};
            const std::uint32_t hash =
                hardware_hash_ ? device::flow_hash(steering, tuple) : detail::software_flow_hash(key);
            if (table_.find(key, hash)) {
                continue;
            }
            base_hash = hash;
            return port;
        }
        return std::unexpected{ConnectError::NoPort};
    }

    template <typename Ip>
    void Stack<Ip>::arm_retry(ConnectionType& c) noexcept {
        const core::Duration delay =
            c.unresolved_wait_ ? config_.unresolved_retry : config_.retry_initial * (std::int64_t{1} << c.tries_);
        wheel_->arm(c.timer_, now_ + delay);
    }

    template <typename Ip>
    std::expected<void, net::SendError> Stack<Ip>::resend_control(ConnectionType& c) noexcept {
        switch (c.state_) {
            case State::SynSent:
                return send_control(c, wire::TcpFlags{wire::TcpFlag::Syn}, c.iss_);
            case State::SynReceived:
                return send_control(c, wire::TcpFlags{wire::TcpFlag::Syn, wire::TcpFlag::Ack}, c.iss_);
            case State::FinWait1:
            case State::Closing:
            case State::LastAck:
                return send_control(c, wire::TcpFlags{wire::TcpFlag::Fin, wire::TcpFlag::Ack}, c.fin_sequence_);
            default:
                return {};
        }
    }

    /// Called by the wheel with the timer unarmed: a try, or a poll while the next hop is unresolved.
    template <typename Ip>
    void Stack<Ip>::on_timer(ConnectionType& c) noexcept {
        switch (c.state_) {
            case State::SynSent:
            case State::SynReceived:
            case State::FinWait1:
            case State::Closing:
            case State::LastAck:
                break;
            default:
                return;  // nothing to retransmit in this state
        }
        if (!c.unresolved_wait_) {
            if (c.tries_ >= config_.retries) {
                time_out(c);
                return;
            }
            ++c.tries_;
            ++counters_.retransmits;
        }
        const auto sent = resend_control(c);
        if (sent) {
            ++counters_.control_segments_sent;
        }
        c.unresolved_wait_ = !sent && sent.error() == net::SendError::Unresolved;
        arm_retry(c);
    }

    template <typename Ip>
    void Stack<Ip>::time_out(ConnectionType& c) noexcept {
        if (!c.owned_) {
            ++counters_.handshakes_failed;  // a passive open nobody took: freed silently
            free_slot(c);
            return;
        }
        ++counters_.connections_timed_out;
        set_closed(c);
        raise(c, stream::Event::TimedOut);
    }

    /// Closed, with nothing armed, pending or prepared. Held segments stay until release.
    template <typename Ip>
    void Stack<Ip>::set_closed(ConnectionType& c) noexcept {
        c.state_ = State::Closed;
        wheel_->cancel(c.timer_);
        c.unresolved_wait_ = false;
        acks_.remove(c);
        c.ack_pending_    = false;
        c.finish_pending_ = false;
        retries_.remove(c);
        c.prepared_.reset();
        c.prepared_size_ = 0;
    }

    template <typename Ip>
    void Stack<Ip>::finish(ConnectionType& c) noexcept {
        set_closed(c);
        ++counters_.connections_closed;
        raise(c, stream::Event::Closed);
    }

    /// The last ACK the peer is owed must be queued before Closed is published; flush retries the rest.
    template <typename Ip>
    void Stack<Ip>::maybe_finish(ConnectionType& c) noexcept {
        if (c.finish_pending_ && !c.ack_pending_) {
            finish(c);
        }
    }

    template <typename Ip>
    void Stack<Ip>::abort(ConnectionType& c) noexcept {
        if (c.state_ == State::Closed) {
            return;
        }
        c.prepared_.reset();
        c.prepared_size_ = 0;
        if (send_control(c, wire::TcpFlags{wire::TcpFlag::Rst, wire::TcpFlag::Ack}, c.snd_nxt_)) {
            ++counters_.resets_sent;
        }
        set_closed(c);
        raise(c, stream::Event::Closed);
    }

    template <typename Ip>
    void Stack<Ip>::release(ConnectionType& c) noexcept {
        if (c.state_ != State::Closed) {
            abort(c);
        }
        free_slot(c);
    }

}  // namespace aloe::tcp
