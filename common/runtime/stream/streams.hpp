#pragma once

#include <aloe/core>
#include <aloe/execution>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <utility>
#include <vector>

#include <shard_context.hpp>
#include <stream_events.hpp>
#include <stream_wait.hpp>
#include <tcp_config.hpp>
#include <timer_wheel.hpp>

namespace aloe::runtime {

    template <typename Stack>
    class Stream;

    namespace detail {
        template <typename Stack, typename Policy>
        struct StreamSender;
        struct ConnectPolicy;
        struct AcceptPolicy;
    }  // namespace detail

    /**
     * @brief The runtime's side of a TCP stack: one wait slot per connection, one per listener,
     * and the wake pass that turns drained events into queued completions.
     *
     * Constructed over a `tcp::Stack` and the shard's context, with every slot and every listener's
     * backlog allocated here. `wake` drains `poll_event()` and, per flag, detaches the matching parked
     * wait and pushes it onto the run queue; it never calls a receiver. `cancel` requests the slot's
     * stop source and aborts; `deadline` arms the slot's timer, whose fire is `cancel`; `release` stops
     * every active wait, clears the slot, rebuilds its stop source and releases the brick's connection.
     */
    template <typename Stack>
    class Streams {
    public:
        using ConnectionType = typename Stack::ConnectionType;
        using StreamType     = Stream<Stack>;
        using Sequence       = typename Stack::Sequence;

        Streams(Stack& stack, ShardContext& shard)
            : tcp_{&stack},
              context_{&shard} {
            for (std::uint32_t index = 0; index < stack.capacity(); ++index) {
                Slot& slot = slots_.emplace_back(this, index);
                slot.stop.emplace();
            }
            listeners_.resize(stack.config().listeners);
            for (Listener& listener : listeners_) {
                listener.backlog.resize(stack.capacity());  // live connections are bounded by the table: never full
            }
        }

        Streams(const Streams&)            = delete;
        Streams& operator=(const Streams&) = delete;
        Streams(Streams&&)                 = delete;
        Streams& operator=(Streams&&)      = delete;

        ~Streams() {
            for (Slot& slot : slots_) {
                context_->timers().cancel(slot.deadline);
                assert(slot.active == nullptr && "a stream operation outlived the shard's scope");
            }
        }

        /// Drains the brick's events; every completion goes onto the run queue, none runs here.
        void wake(core::TimePoint /*now*/) noexcept {
            while (const auto event = tcp_->poll_event()) {
                deliver(*event->connection, event->events);
            }
        }

        /// Requests the slot's stop source and aborts: parked waits complete stopped, later starts stop at once.
        void cancel(const std::uint32_t index) noexcept {
            Slot& slot = slots_[index];
            slot.stop->request_stop();
            tcp_->connection(index).abort();
        }

        void deadline(const std::uint32_t index, const core::TimePoint when) noexcept {
            context_->timers().arm(slots_[index].deadline, when);
        }

        /// The owner's last call for a connection: every active wait is stopped, the slot is clean for reuse.
        void release(const std::uint32_t index) noexcept {
            Slot& slot = slots_[index];
            context_->timers().cancel(slot.deadline);
            while (detail::StreamWait* wait = slot.active) {
                unlink(slot, *wait);
                wait->drop_callbacks(*wait);
                wait->outcome = detail::WaitOutcome{.stopped = true};
                if (wait->phase == detail::StreamWait::Phase::Parked) {
                    slot.waits[kind_index(wait->kind)] = nullptr;
                    wait->phase                        = detail::StreamWait::Phase::Queued;
                    context_->ready().push(*wait);
                }
            }
            slot.waits.fill(nullptr);
            slot.terminal.reset();
            slot.owned = false;
            slot.stop.emplace();  // after every callback on the old source is gone
            tcp_->connection(index).release();
        }

        [[nodiscard]] detail::StreamSender<Stack, detail::AcceptPolicy> accept(std::uint16_t port) noexcept;
        [[nodiscard]] detail::StreamSender<Stack, detail::ConnectPolicy> connect(tcp::Endpoint peer) noexcept;

        [[nodiscard]] Stack& tcp() noexcept {
            return *tcp_;
        }
        [[nodiscard]] ShardContext& context() noexcept {
            return *context_;
        }

        // The operations' side: parking, queueing and the slot's state. Not for applications.

        void park(detail::StreamWait& wait) noexcept {
            Slot& slot                  = slots_[wait.index];
            detail::StreamWait*& parked = slot.waits[kind_index(wait.kind)];
            assert(parked == nullptr && "one parked operation per kind per connection");
            parked     = &wait;
            wait.phase = detail::StreamWait::Phase::Parked;
            if (!wait.active) {
                link(slot, wait);  // a re-parked send is still on the list
            }
        }

        /// Detaches from the kind pointer and pushes onto the run queue; stays on the active list until it runs.
        void queue(detail::StreamWait& wait, const detail::WaitOutcome outcome) noexcept {
            if (wait.kind == detail::WaitKind::Accept) {
                if (Listener* listener = find_listener(static_cast<std::uint16_t>(wait.index))) {
                    if (listener->accept == &wait) {
                        listener->accept = nullptr;
                    }
                }
            } else if (wait.phase == detail::StreamWait::Phase::Parked) {
                slots_[wait.index].waits[kind_index(wait.kind)] = nullptr;
            }
            wait.outcome = outcome;
            wait.phase   = detail::StreamWait::Phase::Queued;
            context_->ready().push(wait);
        }

        /// Unlinks a wait that is about to complete; the completion itself belongs to the operation.
        void retire(detail::StreamWait& wait) noexcept {
            if (wait.active && wait.kind != detail::WaitKind::Accept) {
                unlink(slots_[wait.index], wait);
            }
            wait.phase = detail::StreamWait::Phase::Completed;
        }

        [[nodiscard]] execution::ex::inplace_stop_token slot_token(const std::uint32_t index) const noexcept {
            return slots_[index].stop->get_token();
        }

        [[nodiscard]] std::optional<stream::Error> terminal(const std::uint32_t index) const noexcept {
            return slots_[index].terminal;
        }

        void own(const std::uint32_t index) noexcept {
            slots_[index].owned = true;
        }

        /// The listener for `port`, created through the brick when absent. Refused when someone else listens there.
        [[nodiscard]] std::expected<void, stream::Error> ensure_listener(const std::uint16_t port) noexcept {
            if (find_listener(port) != nullptr) {
                return {};
            }
            if (listener_count_ == listeners_.size()) {
                return std::unexpected{stream::Error::TableFull};
            }
            const auto listened = tcp_->listen(port);
            if (!listened) {
                return std::unexpected{listened.error() == tcp::ListenError::TableFull ? stream::Error::TableFull
                                                                                       : stream::Error::Refused};
            }
            Listener& listener = listeners_[listener_count_++];
            listener.port      = port;
            listener.head      = 0;
            listener.count     = 0;
            return {};
        }

        void park_accept(detail::StreamWait& wait) noexcept {
            Listener* listener = find_listener(static_cast<std::uint16_t>(wait.index));
            assert(listener != nullptr && listener->accept == nullptr && "one accept per listener");
            listener->accept = &wait;
            wait.phase       = detail::StreamWait::Phase::Parked;
        }

        [[nodiscard]] std::optional<std::uint32_t> take_backlog(const std::uint16_t port) noexcept {
            Listener* listener = find_listener(port);
            if (listener == nullptr || listener->count == 0) {
                return std::nullopt;
            }
            const std::uint32_t index = listener->backlog[listener->head];
            listener->head            = (listener->head + 1) % listener->backlog.size();
            --listener->count;
            return index;
        }

    private:
        struct DeadlineTimer : loop::Timer {
            DeadlineTimer(Streams* streams, const std::uint32_t slot) noexcept
                : loop::Timer{&Streams::on_deadline},
                  owner{streams},
                  index{slot} {
            }

            Streams* owner      = nullptr;
            std::uint32_t index = 0;
        };

        struct Slot {
            Slot(Streams* owner, const std::uint32_t index) noexcept
                : deadline{owner, index} {
            }

            std::array<detail::StreamWait*, detail::slot_wait_kinds> waits{};
            detail::StreamWait* active = nullptr;  ///< Parked and queued waits, for release.
            std::optional<execution::ex::inplace_stop_source> stop{};
            std::optional<stream::Error> terminal{};
            DeadlineTimer deadline;
            bool owned = false;
        };

        struct Listener {
            std::uint16_t port         = 0;
            detail::StreamWait* accept = nullptr;
            std::vector<std::uint32_t> backlog{};  ///< A ring of accepted connections nobody has taken, in order.
            std::size_t head  = 0;
            std::size_t count = 0;
        };

        [[nodiscard]] static constexpr std::size_t kind_index(const detail::WaitKind kind) noexcept {
            return static_cast<std::size_t>(kind);
        }

        static void on_deadline(loop::Timer& timer) noexcept {
            auto& self = static_cast<DeadlineTimer&>(timer);
            self.owner->cancel(self.index);
        }

        static void link(Slot& slot, detail::StreamWait& wait) noexcept {
            wait.prev_active = nullptr;
            wait.next_active = slot.active;
            if (slot.active != nullptr) {
                slot.active->prev_active = &wait;
            }
            slot.active = &wait;
            wait.active = true;
        }

        static void unlink(Slot& slot, detail::StreamWait& wait) noexcept {
            if (wait.prev_active != nullptr) {
                wait.prev_active->next_active = wait.next_active;
            } else {
                slot.active = wait.next_active;
            }
            if (wait.next_active != nullptr) {
                wait.next_active->prev_active = wait.prev_active;
            }
            wait.prev_active = nullptr;
            wait.next_active = nullptr;
            wait.active      = false;
        }

        [[nodiscard]] Listener* find_listener(const std::uint16_t port) noexcept {
            for (std::size_t index = 0; index < listener_count_; ++index) {
                if (listeners_[index].port == port) {
                    return &listeners_[index];
                }
            }
            return nullptr;
        }

        static void push_backlog(Listener& listener, const std::uint32_t index) noexcept {
            assert(listener.count < listener.backlog.size() && "more accepted connections than table slots");
            listener.backlog[(listener.head + listener.count) % listener.backlog.size()] = index;
            ++listener.count;
        }

        /// The outcome a terminal event gives a wait of `kind`: data still counts, a normal close satisfies closed().
        [[nodiscard]] static detail::WaitOutcome terminal_outcome(const detail::WaitKind kind,
                                                                  const stream::Error error,
                                                                  const ConnectionType& c,
                                                                  const std::size_t threshold) noexcept {
            switch (kind) {
                case detail::WaitKind::Readable:
                    if (c.unread().size() >= threshold) {
                        return {.value = c.unread().size()};
                    }
                    return {.error = error == stream::Error::Closed ? stream::Error::PeerClosed : error};
                case detail::WaitKind::Acked:
                    if (!c.acknowledged().before(Sequence{static_cast<std::uint32_t>(threshold)})) {
                        return {};
                    }
                    return {.error = error};
                case detail::WaitKind::Connected:
                    return {.error = error == stream::Error::Reset ? stream::Error::Refused : error};
                case detail::WaitKind::Closed:
                    if (error == stream::Error::Closed) {
                        return {};
                    }
                    return {.error = error};
                default:
                    return {.error = error};
            }
        }

        void deliver(ConnectionType& c, const stream::Events events) noexcept {
            Slot& slot = slots_[c.index()];
            if (events.accepted()) {
                Listener* listener = find_listener(c.local().port);
                if (listener == nullptr) {
                    c.release();  // nobody will ever take it: not a listener of ours
                    return;
                }
                if (listener->accept != nullptr) {
                    queue(*listener->accept, {.value = c.index()});
                } else {
                    push_backlog(*listener, c.index());
                }
            }
            if (events.connected()) {
                if (detail::StreamWait* wait = slot.waits[kind_index(detail::WaitKind::Connected)]) {
                    queue(*wait, {.value = c.index()});
                }
            }
            if (events.readable() || events.peer_closed()) {
                if (detail::StreamWait* wait = slot.waits[kind_index(detail::WaitKind::Readable)]) {
                    if (c.unread().size() >= wait->threshold) {
                        queue(*wait, {.value = c.unread().size()});
                    } else if (c.peer_closed()) {
                        queue(*wait, {.error = stream::Error::PeerClosed});
                    }
                }
            }
            if (events.writable()) {
                if (detail::StreamWait* wait = slot.waits[kind_index(detail::WaitKind::Writable)]) {
                    if (c.writable() >= wait->threshold) {
                        queue(*wait, {.value = c.writable()});
                    }
                }
            }
            if (events.acked()) {
                if (detail::StreamWait* wait = slot.waits[kind_index(detail::WaitKind::Acked)]) {
                    if (!c.acknowledged().before(Sequence{static_cast<std::uint32_t>(wait->threshold)})) {
                        queue(*wait, {});
                    }
                }
            }
            std::optional<stream::Error> terminal;
            if (events.reset()) {
                terminal = stream::Error::Reset;
            } else if (events.timed_out()) {
                terminal = stream::Error::TimedOut;
            } else if (events.closed()) {
                terminal = stream::Error::Closed;
            }
            if (terminal) {
                slot.terminal = terminal;
                for (std::size_t kind = 0; kind < detail::slot_wait_kinds; ++kind) {
                    if (detail::StreamWait* wait = slot.waits[kind]) {
                        queue(*wait,
                              terminal_outcome(static_cast<detail::WaitKind>(kind), *terminal, c, wait->threshold));
                    }
                }
            }
        }

        Stack* tcp_            = nullptr;
        ShardContext* context_ = nullptr;
        std::deque<Slot> slots_{};           ///< A slot holds a timer: immovable, so a deque.
        std::vector<Listener> listeners_{};  ///< Sized at construction; the first `listener_count_` are in use.
        std::size_t listener_count_ = 0;
    };

}  // namespace aloe::runtime

#include <stream_handle.hpp>
#include <stream_senders.hpp>
