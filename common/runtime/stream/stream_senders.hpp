#pragma once

#include <aloe/core>
#include <aloe/execution>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>

#include <scheduler.hpp>
#include <stream.hpp>
#include <stream_handle.hpp>
#include <stream_wait.hpp>
#include <streams.hpp>
#include <tcp_config.hpp>

namespace aloe::runtime::detail {

    template <typename Stack, typename Receiver, typename Policy>
    struct WaitOperation;

    template <typename Op>
    using Stack_of = typename Op::StackType;

    /**
     * The policies: what a sender checks when it starts, what it does when woken, and how it turns the
     * saved outcome into its value. `begin` returns an outcome to complete with at once, or nothing
     * to park. `resume` returns true when a woken operation completes, false when it parked again.
     */
    struct ReadablePolicy {
        using Result                   = std::expected<std::size_t, stream::Error>;
        static constexpr WaitKind kind = WaitKind::Readable;

        template <typename Op>
        [[nodiscard]] static std::optional<WaitOutcome> begin(Op& op) noexcept {
            const auto& c = op.owner->tcp().connection(op.index);
            if (c.unread().size() >= op.threshold) {
                return WaitOutcome{.value = c.unread().size()};
            }
            if (const auto error = op.owner->terminal(op.index)) {
                return WaitOutcome{.error = *error == stream::Error::Closed ? stream::Error::PeerClosed : *error};
            }
            if (c.peer_closed()) {
                return WaitOutcome{.error = stream::Error::PeerClosed};
            }
            return std::nullopt;
        }

        template <typename Op>
        [[nodiscard]] static bool resume(Op&) noexcept {
            return true;
        }

        template <typename Op>
        [[nodiscard]] static Result result(const Op& op) noexcept {
            if (op.outcome.error) {
                return std::unexpected{*op.outcome.error};
            }
            return op.outcome.value;
        }
    };

    struct WritablePolicy {
        using Result                   = std::expected<std::size_t, stream::Error>;
        static constexpr WaitKind kind = WaitKind::Writable;

        template <typename Op>
        [[nodiscard]] static std::optional<WaitOutcome> begin(Op& op) noexcept {
            const auto& c = op.owner->tcp().connection(op.index);
            assert(op.threshold <= c.mss() && "writable(n): n is at most the MSS");
            if (const auto error = op.owner->terminal(op.index)) {
                return WaitOutcome{.error = *error};
            }
            if (c.writable() >= op.threshold) {
                return WaitOutcome{.value = c.writable()};
            }
            return std::nullopt;
        }

        template <typename Op>
        [[nodiscard]] static bool resume(Op&) noexcept {
            return true;
        }

        template <typename Op>
        [[nodiscard]] static Result result(const Op& op) noexcept {
            if (op.outcome.error) {
                return std::unexpected{*op.outcome.error};
            }
            return op.outcome.value;
        }
    };

    struct AckedPolicy {
        using Result                   = std::expected<void, stream::Error>;
        static constexpr WaitKind kind = WaitKind::Acked;

        template <typename Op>
        [[nodiscard]] static std::optional<WaitOutcome> begin(Op& op) noexcept {
            const auto& c  = op.owner->tcp().connection(op.index);
            using Sequence = typename Stack_of<Op>::Sequence;
            if (!c.acknowledged().before(Sequence{static_cast<std::uint32_t>(op.threshold)})) {
                return WaitOutcome{};
            }
            if (const auto error = op.owner->terminal(op.index)) {
                return WaitOutcome{.error = *error};
            }
            return std::nullopt;
        }

        template <typename Op>
        [[nodiscard]] static bool resume(Op&) noexcept {
            return true;
        }

        template <typename Op>
        [[nodiscard]] static Result result(const Op& op) noexcept {
            if (op.outcome.error) {
                return std::unexpected{*op.outcome.error};
            }
            return {};
        }
    };

    /// `closed()`, and `close()` when `close_first`: the brick's close, then the wait.
    struct ClosedPolicy {
        using Result                   = std::expected<void, stream::Error>;
        static constexpr WaitKind kind = WaitKind::Closed;

        bool close_first = false;

        template <typename Op>
        [[nodiscard]] std::optional<WaitOutcome> begin(Op& op) const noexcept {
            auto& c = op.owner->tcp().connection(op.index);
            if (close_first) {
                c.close();
            }
            if (const auto error = op.owner->terminal(op.index)) {
                if (*error == stream::Error::Closed) {
                    return WaitOutcome{};
                }
                return WaitOutcome{.error = *error};
            }
            if (c.state() == tcp::State::Closed) {
                // Closed with no terminal recorded: the terminal event is raised and not yet drained, as when a
                // retransmit timer times out inside run_once and the chain that runs next starts this sender.
                // Read it from the undrained flags, mapped as the wake pass maps it for a Closed wait.
                const stream::Events pending = c.events();
                if (pending.timed_out()) {
                    return WaitOutcome{.error = stream::Error::TimedOut};
                }
                if (pending.reset()) {
                    return WaitOutcome{.error = stream::Error::Reset};  // a handed-over stream is past Connected
                }
                if (pending.closed()) {
                    return WaitOutcome{};
                }
                // Nothing pending: every path to Closed raises a terminal event, so the next wake delivers it.
            }
            return std::nullopt;
        }

        template <typename Op>
        [[nodiscard]] static bool resume(Op&) noexcept {
            return true;
        }

        template <typename Op>
        [[nodiscard]] static Result result(const Op& op) noexcept {
            if (op.outcome.error) {
                return std::unexpected{*op.outcome.error};
            }
            return {};
        }
    };

    /// `send(bytes)`: commits what it can, parks on Writable for the rest, keeps the caller's span until done.
    struct SendPolicy {
        using Result                   = std::expected<std::size_t, stream::Error>;
        static constexpr WaitKind kind = WaitKind::Writable;

        std::span<const std::byte> bytes{};
        std::size_t accepted = 0;

        template <typename Op>
        [[nodiscard]] std::optional<WaitOutcome> begin(Op& op) noexcept {
            op.threshold = 1;
            return advance(op);
        }

        template <typename Op>
        [[nodiscard]] bool resume(Op& op) noexcept {
            if (op.outcome.stopped || op.outcome.error) {
                return true;
            }
            const std::optional<WaitOutcome> done = advance(op);
            if (!done) {
                op.owner->park(op);  // a refused retry, or more to send: park again under the same callbacks
                return false;
            }
            op.outcome = *done;
            return true;
        }

        template <typename Op>
        [[nodiscard]] std::optional<WaitOutcome> advance(Op& op) noexcept {
            auto& c = op.owner->tcp().connection(op.index);
            if (const auto error = op.owner->terminal(op.index)) {
                return WaitOutcome{.error = *error};
            }
            accepted += stream::send(c, bytes.subspan(accepted));
            if (accepted == bytes.size()) {
                return WaitOutcome{.value = accepted};
            }
            return std::nullopt;  // a refused prepare or commit, or no credit: wait for the hint, never retry inline
        }

        template <typename Op>
        [[nodiscard]] static Result result(const Op& op) noexcept {
            if (op.outcome.error) {
                return std::unexpected{*op.outcome.error};
            }
            return op.outcome.value;
        }
    };

    [[nodiscard]] constexpr stream::Error to_error(const tcp::ConnectError error) noexcept {
        switch (error) {
            case tcp::ConnectError::TableFull:
                return stream::Error::TableFull;
            case tcp::ConnectError::NoPort:
                return stream::Error::NoPort;
            case tcp::ConnectError::Unplaceable:
                return stream::Error::Unplaceable;
            default:
                return stream::Error::NoRoute;
        }
    }

    /// `connect(peer)`: opens on start and owns the pending connection until it hands it over.
    struct ConnectPolicy {
        template <typename Stack>
        using ResultFor                = std::expected<Stream<Stack>, stream::Error>;
        static constexpr WaitKind kind = WaitKind::Connected;

        tcp::Endpoint peer{};
        bool opened = false;  ///< A SynSent connection this operation owns until it hands it over.

        template <typename Op>
        [[nodiscard]] std::optional<WaitOutcome> begin(Op& op) noexcept {
            auto& stack          = op.owner->tcp();
            const auto connected = stack.connect(peer, op.owner->context().now());
            if (!connected) {
                return WaitOutcome{.error = to_error(connected.error())};
            }
            op.index = (*connected)->index();
            opened   = true;
            return std::nullopt;
        }

        template <typename Op>
        [[nodiscard]] static bool resume(Op&) noexcept {
            return true;
        }

        /// A stopped or failed open releases the connection it still owns; a handed-over one is the stream's.
        template <typename Op>
        void abandon(Op& op) noexcept {
            if (opened) {
                opened = false;
                op.owner->release(op.index);
            }
        }
    };

    /// `accept(port)`: the listener's backlog first, else parks on the listener.
    struct AcceptPolicy {
        template <typename Stack>
        using ResultFor                = std::expected<Stream<Stack>, stream::Error>;
        static constexpr WaitKind kind = WaitKind::Accept;

        std::uint16_t port = 0;

        template <typename Op>
        [[nodiscard]] std::optional<WaitOutcome> begin(Op& op) const noexcept {
            op.index = port;
            if (const auto listening = op.owner->ensure_listener(port); !listening) {
                return WaitOutcome{.error = listening.error()};
            }
            if (const auto ready = op.owner->take_backlog(port)) {
                return WaitOutcome{.value = *ready};
            }
            return std::nullopt;
        }

        template <typename Op>
        [[nodiscard]] static bool resume(Op&) noexcept {
            return true;
        }

        /// A connection taken from the backlog but never handed over goes back to the brick.
        template <typename Op>
        void abandon(Op& op) noexcept {
            if (op.phase == StreamWait::Phase::Queued && !op.outcome.error && !op.outcome.stopped) {
                op.owner->release(static_cast<std::uint32_t>(op.outcome.value));
            }
        }
    };

    template <typename Policy, typename Stack>
    struct PolicyResult {
        using type = typename Policy::Result;
    };

    template <typename Stack>
    struct PolicyResult<ConnectPolicy, Stack> {
        using type = std::expected<Stream<Stack>, stream::Error>;
    };

    template <typename Stack>
    struct PolicyResult<AcceptPolicy, Stack> {
        using type = std::expected<Stream<Stack>, stream::Error>;
    };

    template <typename Policy>
    concept HandsOverConnection = std::same_as<Policy, ConnectPolicy> || std::same_as<Policy, AcceptPolicy>;

    /**
     * The operation state of every stream sender: the wait node, the receiver, the policy, and the
     * two stop callbacks, on the receiver's token (scope shutdown) and on the slot's token (cancel
     * and deadline). Allocates nothing; lives in the caller's frame; completes on the shard.
     */
    template <typename Stack, typename Receiver, typename Policy>
    struct WaitOperation final : StreamWait {
        using StackType = Stack;
        using Result    = typename PolicyResult<Policy, Stack>::type;
        using StopToken = execution::ex::stop_token_of_t<execution::ex::env_of_t<Receiver>>;

        struct OnStop {
            WaitOperation* self = nullptr;

            void operator()() noexcept {
                self->cancel();
            }
        };

        using ReceiverCallback = execution::ex::stop_callback_for_t<StopToken, OnStop>;
        using SlotCallback     = execution::ex::inplace_stop_callback<OnStop>;

        Streams<Stack>* owner = nullptr;
        Receiver receiver;
        Policy policy{};
        std::optional<ReceiverCallback> receiver_callback{};
        std::optional<SlotCallback> slot_callback{};

        WaitOperation(
            Streams<Stack>* o, Receiver r, const Policy& p, const std::uint32_t i, const std::size_t n) noexcept
            : StreamWait{&WaitOperation::complete, &WaitOperation::drop},
              owner{o},
              receiver{std::move(r)},
              policy{p} {
            kind      = Policy::kind;
            index     = i;
            threshold = n;
        }

        WaitOperation(const WaitOperation&)            = delete;
        WaitOperation& operator=(const WaitOperation&) = delete;
        WaitOperation(WaitOperation&&)                 = delete;
        WaitOperation& operator=(WaitOperation&&)      = delete;

        /// A started operation lives until it completes: the slot and the run queue point at it until then.
        ~WaitOperation() {
            assert(phase != Phase::Parked && phase != Phase::Queued &&
                   "a stream operation destroyed before completing");
        }

        void start() & noexcept {
            assert(ShardContext::current() == &owner->context() && "a stream sender starts on its own shard");
            const StopToken token = execution::ex::get_stop_token(execution::ex::get_env(receiver));
            if (token.stop_requested()) {
                execution::ex::set_stopped(std::move(receiver));
                return;
            }
            if constexpr (!HandsOverConnection<Policy>) {
                if (owner->slot_token(index).stop_requested()) {  // cancelled before this operation started
                    execution::ex::set_stopped(std::move(receiver));
                    return;
                }
            }
            if (const std::optional<WaitOutcome> now = policy.begin(*this)) {
                outcome = *now;
                finish();
                return;
            }
            if constexpr (Policy::kind != WaitKind::Accept) {
                // A connect that landed on a slot cancelled and not released.
                if (owner->slot_token(index).stop_requested()) {
                    outcome = WaitOutcome{.stopped = true};
                    finish();
                    return;
                }
                owner->park(*this);
                slot_callback.emplace(owner->slot_token(index), OnStop{this});
            } else {
                owner->park_accept(*this);
            }
            receiver_callback.emplace(token, OnStop{this});
        }

        /**
         * From either token, on the shard thread. Parked: detach from the kind pointer and queue stopped, staying
         * on the active list until it runs, so a release before then still drops its callbacks. Queued: the saved
         * outcome becomes stopped; the node is already in the chain and is not enqueued again. An accepted
         * connection the queued outcome carries goes back to the brick here; a connect's own slot is released
         * when the completion runs, never from inside a stop callback on that slot's source.
         */
        void cancel() noexcept {
            if (phase == Phase::Parked) {
                owner->queue(*this, WaitOutcome{.stopped = true});
            } else if (phase == Phase::Queued) {
                if constexpr (std::same_as<Policy, AcceptPolicy>) {
                    policy.abandon(*this);
                }
                outcome = WaitOutcome{.stopped = true};
            }
        }

        static void complete(loop::Work& work) noexcept {
            auto& self = static_cast<WaitOperation&>(work);
            if (!self.policy.resume(self)) {
                return;  // parked again, under the same callbacks
            }
            self.owner->retire(self);
            self.finish();
        }

        static void drop(StreamWait& wait) noexcept {
            auto& self = static_cast<WaitOperation&>(wait);
            self.receiver_callback.reset();
            self.slot_callback.reset();
        }

        void finish() noexcept {
            drop(*this);
            phase = Phase::Completed;
            if (outcome.stopped) {
                if constexpr (HandsOverConnection<Policy>) {
                    policy.abandon(*this);  // a stopped connect gives its pending connection back
                }
                execution::ex::set_stopped(std::move(receiver));
                return;
            }
            if constexpr (HandsOverConnection<Policy>) {
                if (outcome.error) {
                    // A refused or timed-out open: the slot goes back before the error is reported.
                    policy.abandon(*this);
                    execution::ex::set_value(std::move(receiver), Result{std::unexpected{*outcome.error}});
                } else {
                    execution::ex::set_value(std::move(receiver),
                                             Result{
                                                 Stream<Stack>{*owner, static_cast<std::uint32_t>(outcome.value)}
                    });
                }
            } else {
                execution::ex::set_value(std::move(receiver), Policy::result(*this));
            }
        }
    };

    /// Attributes of a stream sender: it completes on its shard, asynchronously when it parks, and inline from
    /// `start` when the level is already met or the operation is already stopped.
    struct StreamSenderAttributes {
        ShardContext* context = nullptr;

        template <typename Tag>
        [[nodiscard]] Scheduler query(execution::ex::get_completion_scheduler_t<Tag> /*tag*/) const noexcept {
            return Scheduler{*context};
        }

        template <typename Tag>
        [[nodiscard]] static constexpr auto query(execution::get_completion_behavior_t<Tag> /*tag*/) noexcept {
            return execution::completion_behavior::asynchronous_affine |
                   execution::completion_behavior::inline_completion;
        }
    };

    template <typename Stack, typename Policy>
    struct StreamSender {
        using sender_concept = execution::ex::sender_t;
        using Result         = typename PolicyResult<Policy, Stack>::type;
        using completion_signatures =
            execution::ex::completion_signatures<execution::ex::set_value_t(Result), execution::ex::set_stopped_t()>;

        Streams<Stack>* owner = nullptr;
        std::uint32_t index   = 0;
        std::size_t threshold = 0;
        Policy policy{};

        template <execution::ex::receiver Receiver>
        [[nodiscard]] auto connect(Receiver receiver) const noexcept -> WaitOperation<Stack, Receiver, Policy> {
            return WaitOperation<Stack, Receiver, Policy>{owner, std::move(receiver), policy, index, threshold};
        }

        [[nodiscard]] StreamSenderAttributes get_env() const noexcept {
            return StreamSenderAttributes{&owner->context()};
        }
    };

}  // namespace aloe::runtime::detail

namespace aloe::runtime {

    template <typename Stack>
    detail::StreamSender<Stack, detail::AcceptPolicy> Streams<Stack>::accept(const std::uint16_t port) noexcept {
        return {this, port, 0, detail::AcceptPolicy{.port = port}};
    }

    template <typename Stack>
    detail::StreamSender<Stack, detail::ConnectPolicy> Streams<Stack>::connect(const tcp::Endpoint peer) noexcept {
        return {this, 0, 0, detail::ConnectPolicy{.peer = peer}};
    }

}  // namespace aloe::runtime
