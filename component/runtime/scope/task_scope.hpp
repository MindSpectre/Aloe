#pragma once

#include <aloe/core>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

#include <counters.hpp>

namespace aloe::runtime {

    class TaskScope;

    namespace detail {

        struct EmptyEnv {};

        /// The receiver environment of spawned work: the scope's stop token, then whatever `Env` answers.
        template <typename Env>
        struct ScopeEnv {
            core::ex::inplace_stop_token token;
            Env env;

            [[nodiscard]] core::ex::inplace_stop_token query(core::ex::get_stop_token_t) const noexcept {
                return token;
            }

            template <typename Query>
                requires requires(const Env& inner, const Query query) { inner.query(query); }
            [[nodiscard]] auto query(const Query query) const noexcept {
                return env.query(query);
            }
        };

        enum class Outcome : std::uint8_t { Value, Stopped };

        template <typename Sender, typename Env>
        struct SpawnOperation;

        template <typename Sender, typename Env>
        struct SpawnReceiver {
            using receiver_concept = core::ex::receiver_t;

            SpawnOperation<Sender, Env>* operation;

            template <typename... Values>
            void set_value(Values&&...) && noexcept;
            void set_error(std::exception_ptr error) && noexcept;
            template <typename Error>
            void set_error(Error&&) && noexcept;
            void set_stopped() && noexcept;
            [[nodiscard]] ScopeEnv<Env> get_env() const noexcept;
        };

        /// The pending join, if any: a node the scope completes when it empties.
        struct JoinBase {
            using Function    = void (*)(JoinBase&) noexcept;
            Function complete = nullptr;
        };

    }  // namespace detail

    /**
     * @brief The per-shard counting scope: spawn senders, count them, stop them, know when they are done.
     *
     * Shaped like the standard's `counting_scope`, `spawn` and `join`, with no atomics because one
     * thread uses it. `spawn` allocates the operation state, the one allocation per task until the
     * arena arrives, and starts it with an environment that answers `get_stop_token` with the
     * scope's token and everything else from the environment the caller passed. Completions are
     * counted; an error completion is logged at Error and does not take the shard down.
     * `request_stop` trips the stop source, and every later spawn starts already stopped. Debug
     * builds assert that every completion arrives on the thread of the first spawn: a task that
     * strayed to another shard (threading contract rule 4) fails here.
     */
    class TaskScope {
    public:
        class JoinSender;

        TaskScope(core::Logger logger, std::uint16_t index, loop::ShardCounters& counters) noexcept;
        TaskScope(const TaskScope&)            = delete;
        TaskScope& operator=(const TaskScope&) = delete;
        TaskScope(TaskScope&&)                 = delete;
        TaskScope& operator=(TaskScope&&)      = delete;
        ~TaskScope();

        /// Shard thread only. `Env` answers the queries the work asks of its environment, such as `get_scheduler`.
        template <core::ex::sender Sender, typename Env = detail::EmptyEnv>
        void spawn(Sender&& sender, Env env = {});

        void request_stop() noexcept;

        [[nodiscard]] bool stop_requested() const noexcept {
            return stop_source_.stop_requested();
        }

        [[nodiscard]] core::ex::inplace_stop_token stop_token() const noexcept {
            return stop_source_.get_token();
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return live_;
        }

        [[nodiscard]] bool empty() const noexcept {
            return live_ == 0;
        }

        /// Completes with `set_value()` when the scope is empty, at once if it already is. One pending join at a time.
        [[nodiscard]] JoinSender join() noexcept;

    private:
        template <typename Sender, typename Env>
        friend struct detail::SpawnOperation;
        template <typename Receiver>
        friend struct JoinOperation;

        /// Debug builds: the caller is on the thread of the first spawn (threading contract rule 4).
        void assert_owner() const noexcept;
        void finished(detail::Outcome outcome) noexcept;
        void failed(std::exception_ptr error) noexcept;
        void one_less() noexcept;
        void wait(detail::JoinBase& join) noexcept;

        core::Logger logger_;
        std::uint16_t index_;
        loop::ShardCounters* counters_;
        core::ex::inplace_stop_source stop_source_;
        std::size_t live_         = 0;
        detail::JoinBase* joiner_ = nullptr;
        std::optional<std::thread::id> owner_;  ///< The thread of the first spawn; every completion must arrive on it.
    };

    namespace detail {

        template <typename Sender, typename Env>
        struct SpawnOperation {
            TaskScope* scope;
            Env env;
            core::ex::connect_result_t<Sender, SpawnReceiver<Sender, Env>> state;

            SpawnOperation(TaskScope* owner, Sender&& sender, Env environment)
                : scope{owner},
                  env{std::move(environment)},
                  state{core::ex::connect(std::move(sender), SpawnReceiver<Sender, Env>{this})} {
            }

            /// Frees the state first, then tells the scope: the scope may complete a join from there.
            void complete(const Outcome outcome) noexcept {
                scope->assert_owner();
                TaskScope* owner = scope;
                delete this;
                owner->finished(outcome);
            }

            void fail(std::exception_ptr error) noexcept {
                scope->assert_owner();
                TaskScope* owner = scope;
                delete this;
                owner->failed(std::move(error));
            }
        };

        template <typename Sender, typename Env>
        template <typename... Values>
        void SpawnReceiver<Sender, Env>::set_value(Values&&...) && noexcept {
            operation->complete(Outcome::Value);
        }

        template <typename Sender, typename Env>
        void SpawnReceiver<Sender, Env>::set_error(std::exception_ptr error) && noexcept {
            operation->fail(std::move(error));
        }

        template <typename Sender, typename Env>
        template <typename Error>
        void SpawnReceiver<Sender, Env>::set_error(Error&&) && noexcept {
            operation->fail(nullptr);
        }

        template <typename Sender, typename Env>
        void SpawnReceiver<Sender, Env>::set_stopped() && noexcept {
            operation->complete(Outcome::Stopped);
        }

        template <typename Sender, typename Env>
        ScopeEnv<Env> SpawnReceiver<Sender, Env>::get_env() const noexcept {
            return ScopeEnv<Env>{operation->scope->stop_token(), operation->env};
        }

    }  // namespace detail

    template <typename Receiver>
    struct JoinOperation : detail::JoinBase {
        TaskScope* scope;
        Receiver receiver;

        JoinOperation(TaskScope* owner, Receiver r) noexcept
            : JoinBase{&JoinOperation::finish},
              scope{owner},
              receiver{std::move(r)} {
        }

        void start() & noexcept {
            if (scope->empty()) {
                core::ex::set_value(std::move(receiver));
            } else {
                scope->wait(*this);
            }
        }

        static void finish(detail::JoinBase& base) noexcept {
            core::ex::set_value(std::move(static_cast<JoinOperation&>(base).receiver));
        }
    };

    class TaskScope::JoinSender {
    public:
        using sender_concept        = core::ex::sender_t;
        using completion_signatures = core::ex::completion_signatures<core::ex::set_value_t()>;

        explicit JoinSender(TaskScope& scope) noexcept
            : scope_{&scope} {
        }

        template <core::ex::receiver Receiver>
        [[nodiscard]] auto connect(Receiver receiver) const noexcept -> JoinOperation<Receiver> {
            return JoinOperation<Receiver>{scope_, std::move(receiver)};
        }

    private:
        TaskScope* scope_;
    };

    inline TaskScope::JoinSender TaskScope::join() noexcept {
        return JoinSender{*this};
    }

    template <core::ex::sender Sender, typename Env>
    void TaskScope::spawn(Sender&& sender, Env env) {
        using Plain     = std::remove_cvref_t<Sender>;
        using Operation = detail::SpawnOperation<Plain, Env>;
        if (!owner_) {
            owner_ = std::this_thread::get_id();  // the shard thread by contract; the test's own thread in unit tests
        }
        Plain owned{std::forward<Sender>(sender)};
        auto* operation = new Operation{this, std::move(owned), std::move(env)};
        ++live_;
        ++counters_->tasks_spawned;
        core::ex::start(operation->state);
    }

}  // namespace aloe::runtime
