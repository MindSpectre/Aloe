#pragma once

#include <aloe/core>
#include <aloe/execution>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <utility>

#include <scheduler.hpp>
#include <stream_handle.hpp>
#include <stream_policies.hpp>
#include <stream_wait.hpp>
#include <streams.hpp>

namespace aloe::runtime::detail {

    template <typename Stack, typename Receiver, typename Policy>
    struct WaitOperation;

    template <typename Policy, typename Stack>
    struct PolicyResult {
        using type = Policy::Result;
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
        using Result    = PolicyResult<Policy, Stack>::type;
        using StopToken = execution::ex::stop_token_of_t<execution::ex::env_of_t<Receiver>>;

        struct OnStop {
            WaitOperation* self = nullptr;

            void operator()() noexcept {
                core::force_non_const(this);
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
        using Result         = PolicyResult<Policy, Stack>::type;
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
