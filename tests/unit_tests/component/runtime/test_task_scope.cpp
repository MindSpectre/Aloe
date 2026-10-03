#include <aloe/runtime>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <logging_environment.hpp>

namespace {

    // One logging environment per test binary; this file owns it for the Runtime target.
    const auto* const logging = ::testing::AddGlobalTestEnvironment(new aloe::testing::LoggingEnvironment{});

    /// A sender the test completes by hand, which records what its receiver's environment said.
    struct ManualSender {
        using sender_concept = aloe::core::ex::sender_t;
        using completion_signatures =
            aloe::core::ex::completion_signatures<aloe::core::ex::set_value_t(),
                                                  aloe::core::ex::set_error_t(std::exception_ptr),
                                                  aloe::core::ex::set_stopped_t()>;

        struct Handle {
            virtual ~Handle()               = default;
            virtual void value() noexcept   = 0;
            virtual void stopped() noexcept = 0;
            virtual void error() noexcept   = 0;
            aloe::core::ex::inplace_stop_token token;
        };

        std::vector<Handle*>* handles;

        template <aloe::core::ex::receiver Receiver>
        struct Operation : Handle {
            Receiver receiver;
            std::vector<Handle*>* handles;

            Operation(Receiver r, std::vector<Handle*>* h)
                : receiver{std::move(r)},
                  handles{h} {
            }

            void start() & noexcept {
                token = aloe::core::ex::get_stop_token(aloe::core::ex::get_env(receiver));
                if (token.stop_requested()) {
                    aloe::core::ex::set_stopped(std::move(receiver));
                    return;
                }
                handles->push_back(this);
            }

            void value() noexcept override {
                aloe::core::ex::set_value(std::move(receiver));
            }

            void stopped() noexcept override {
                aloe::core::ex::set_stopped(std::move(receiver));
            }

            void error() noexcept override {
                aloe::core::ex::set_error(std::move(receiver), std::make_exception_ptr(std::runtime_error{"boom"}));
            }
        };

        template <aloe::core::ex::receiver Receiver>
        [[nodiscard]] auto connect(Receiver receiver) const -> Operation<Receiver> {
            return Operation<Receiver>{std::move(receiver), handles};
        }
    };

    struct FlagReceiver {
        using receiver_concept = aloe::core::ex::receiver_t;
        bool* flag;

        void set_value() const noexcept {
            *flag = true;
        }
    };

    /// An environment answering one query of its own, to prove the scope forwards what it does not answer itself.
    struct TagEnv {
        int tag;

        struct TagQuery {
            template <typename Env>
            [[nodiscard]] int operator()(const Env& env) const noexcept {
                return env.query(*this);
            }
        };

        [[nodiscard]] int query(TagQuery) const noexcept {
            return tag;
        }
    };

    class TaskScopeTest : public testing::Test {
    protected:
        aloe::runtime::ShardCounters counters_;
        aloe::runtime::TaskScope scope_{aloe::core::logger("aloe.runtime"), 0, counters_};
        std::vector<ManualSender::Handle*> handles_;

        [[nodiscard]] ManualSender manual() noexcept {
            return ManualSender{&handles_};
        }
    };

    /// A sender that records what its receiver's environment answers to the tag query.
    struct Recording {
        using sender_concept        = aloe::core::ex::sender_t;
        using completion_signatures = aloe::core::ex::completion_signatures<aloe::core::ex::set_value_t()>;
        int* seen;

        template <aloe::core::ex::receiver Receiver>
        struct Operation {
            Receiver receiver;
            int* seen;

            void start() & noexcept {
                *seen = TagEnv::TagQuery{}(aloe::core::ex::get_env(receiver));
                aloe::core::ex::set_value(std::move(receiver));
            }
        };

        template <aloe::core::ex::receiver Receiver>
        [[nodiscard]] auto connect(Receiver receiver) const -> Operation<Receiver> {
            return Operation<Receiver>{std::move(receiver), seen};
        }
    };

}  // namespace

TEST_F(TaskScopeTest, InlineCompletionLeavesTheScopeEmptyAndCounted) {
    EXPECT_TRUE(scope_.empty());
    scope_.spawn(aloe::core::ex::just());
    EXPECT_TRUE(scope_.empty());
    EXPECT_EQ(counters_.tasks_spawned, 1);
    EXPECT_EQ(counters_.tasks_completed, 1);
}

TEST_F(TaskScopeTest, PendingWorkKeepsTheScopeOpenUntilItCompletes) {
    scope_.spawn(manual());
    scope_.spawn(manual());
    ASSERT_EQ(handles_.size(), 2);
    EXPECT_EQ(scope_.size(), 2);

    handles_[0]->value();
    EXPECT_EQ(scope_.size(), 1);
    handles_[1]->stopped();
    EXPECT_TRUE(scope_.empty());
    EXPECT_EQ(counters_.tasks_completed, 1);
    EXPECT_EQ(counters_.tasks_stopped, 1);
}

TEST_F(TaskScopeTest, RequestStopReachesRunningWorkAndWorkSpawnedAfterwards) {
    scope_.spawn(manual());
    ASSERT_EQ(handles_.size(), 1);
    EXPECT_FALSE(handles_[0]->token.stop_requested());

    scope_.request_stop();
    EXPECT_TRUE(scope_.stop_requested());
    EXPECT_TRUE(handles_[0]->token.stop_requested()) << "the token handed out earlier sees the stop";
    handles_[0]->stopped();

    scope_.spawn(manual());
    EXPECT_EQ(handles_.size(), 1) << "started with stop already requested, so it completed at once";
    EXPECT_TRUE(scope_.empty());
    EXPECT_EQ(counters_.tasks_stopped, 2);
}

TEST_F(TaskScopeTest, AFailedTaskIsCountedAndTheScopeContinues) {
    scope_.spawn(manual());
    scope_.spawn(aloe::core::ex::just_error(std::make_exception_ptr(std::runtime_error{"boom"})));
    EXPECT_EQ(counters_.tasks_failed, 1);
    EXPECT_EQ(scope_.size(), 1) << "the other task is unaffected";

    handles_[0]->error();
    EXPECT_EQ(counters_.tasks_failed, 2);
    EXPECT_TRUE(scope_.empty());
}

TEST_F(TaskScopeTest, JoinCompletesWhenTheLastTaskEndsOrAtOnceWhenEmpty) {
    bool joined    = false;
    auto immediate = aloe::core::ex::connect(scope_.join(), FlagReceiver{&joined});
    aloe::core::ex::start(immediate);
    EXPECT_TRUE(joined) << "an empty scope joins at once";

    joined = false;
    scope_.spawn(manual());
    scope_.spawn(manual());
    auto waiting = aloe::core::ex::connect(scope_.join(), FlagReceiver{&joined});
    aloe::core::ex::start(waiting);
    EXPECT_FALSE(joined);
    handles_[0]->value();
    EXPECT_FALSE(joined) << "one task still runs";
    handles_[1]->value();
    EXPECT_TRUE(joined);
}

TEST_F(TaskScopeTest, TheEnvironmentPassedToSpawnReachesTheWork) {
    int seen = 0;
    scope_.spawn(Recording{&seen}, TagEnv{.tag = 42});
    EXPECT_EQ(seen, 42);
}
