#include <aloe/runtime>
#include <aloe/wire>
#include <chrono>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using TimePoint = aloe::core::TimePoint;

    constexpr TimePoint start{};

    struct Recorder : aloe::loop::Work {
        Recorder(std::vector<int>& record, const int id)
            : Work{&Recorder::execute},
              record_{&record},
              id_{id} {
        }

        /// On run, push a second node onto the current context's run queue.
        void then_push(aloe::loop::Work& other) noexcept {
            follow_up_ = &other;
        }

        static void execute(aloe::loop::Work& work) noexcept {
            auto& self = static_cast<Recorder&>(work);
            self.record_->push_back(self.id_);
            if (self.follow_up_ != nullptr) {
                aloe::runtime::ShardContext::current()->ready().push(*self.follow_up_);
                self.follow_up_ = nullptr;
            }
        }

    private:
        std::vector<int>* record_;
        int id_;
        aloe::loop::Work* follow_up_ = nullptr;
    };

    struct RecordingTimer : aloe::loop::Timer {
        RecordingTimer(std::vector<int>& record, const int id)
            : Timer{&RecordingTimer::execute},
              record_{&record},
              id_{id} {
        }

        static void execute(aloe::loop::Timer& timer) noexcept {
            auto& self = static_cast<RecordingTimer&>(timer);
            self.record_->push_back(self.id_);
        }

    private:
        std::vector<int>* record_;
        int id_;
    };

    class ShardContextTest : public testing::Test {
    protected:
        aloe::runtime::ShardContext context_{{.index = 3}, start};
        std::vector<int> record_;
    };

}  // namespace

TEST_F(ShardContextTest, TimersFireBeforeQueuedWorkAndInboxWorkJoinsTheQueueBehindIt) {
    Recorder queued{record_, 1};
    Recorder arrived{record_, 2};
    RecordingTimer due{record_, 3};
    context_.ready().push(queued);
    context_.inbox().push(arrived);
    context_.timers().arm(due, start + 1ms);

    EXPECT_TRUE(context_.run_once(start + 1ms));
    EXPECT_EQ(record_, (std::vector<int>{3, 1, 2}));
    EXPECT_EQ(context_.counters().inbox_received, 1);
    EXPECT_EQ(context_.counters().timers_fired, 1);
    EXPECT_EQ(context_.counters().work_run, 2);
    EXPECT_EQ(context_.now(), start + 1ms);
    EXPECT_EQ(context_.index(), 3);
}

TEST_F(ShardContextTest, WorkPushedDuringAStepRunsOnTheNextOne) {
    Recorder first{record_, 1};
    Recorder second{record_, 2};
    first.then_push(second);
    context_.ready().push(first);

    EXPECT_TRUE(context_.run_once(start));
    EXPECT_EQ(record_, (std::vector<int>{1}));
    EXPECT_FALSE(context_.ready().empty());
    EXPECT_TRUE(context_.run_once(start));
    EXPECT_EQ(record_, (std::vector<int>{1, 2}));
    EXPECT_FALSE(context_.run_once(start)) << "nothing left to do";
}

TEST_F(ShardContextTest, CurrentIsSetDuringAStepAndRestoredAfterwards) {
    EXPECT_EQ(aloe::runtime::ShardContext::current(), nullptr);
    {
        const aloe::runtime::ShardContext::Current current{context_};
        EXPECT_EQ(aloe::runtime::ShardContext::current(), &context_);
        aloe::runtime::ShardContext other{{.index = 4}, start};
        {
            const aloe::runtime::ShardContext::Current nested{other};
            EXPECT_EQ(aloe::runtime::ShardContext::current(), &other);
        }
        EXPECT_EQ(aloe::runtime::ShardContext::current(), &context_);
    }
    EXPECT_EQ(aloe::runtime::ShardContext::current(), nullptr);
}

TEST_F(ShardContextTest, DrainedNeedsAStopRequestAndEmptyQueuesAndScope) {
    EXPECT_FALSE(context_.drained());
    EXPECT_FALSE(context_.stop_requested());

    Recorder pending{record_, 1};
    context_.inbox().push(pending);
    context_.request_stop();
    EXPECT_TRUE(context_.stop_requested());
    EXPECT_TRUE(context_.scope().stop_requested()) << "stop reaches the scope";
    EXPECT_FALSE(context_.drained()) << "the inbox still holds work";

    EXPECT_TRUE(context_.run_once(start));
    EXPECT_TRUE(context_.drained());

    context_.request_stop();
    EXPECT_TRUE(context_.drained()) << "a second stop request changes nothing";
}

TEST_F(ShardContextTest, AnArmedTimerDoesNotHoldTheShardOpen) {
    RecordingTimer later{record_, 1};
    context_.timers().arm(later, start + 10s);
    context_.request_stop();
    EXPECT_TRUE(context_.drained()) << "a timer with no task behind it belongs to the stack and dies with it";
    context_.timers().cancel(later);
}

namespace {

    struct Counting : aloe::loop::Work {
        Counting() noexcept
            : aloe::loop::Work{&Counting::run_it} {
        }

        static void run_it(aloe::loop::Work& work) noexcept {
            ++static_cast<Counting&>(work).runs;
        }

        int runs = 0;
    };

}  // namespace

TEST_F(ShardContextTest, SetNowRecordsTheStampWithoutRunningAnything) {
    Counting work;
    context_.ready().push(work);
    context_.set_now(start + 5ms);
    EXPECT_EQ(context_.now(), start + 5ms);
    EXPECT_EQ(work.runs, 0);
    EXPECT_EQ(context_.counters().work_run, 0U);
    EXPECT_TRUE(context_.run_once(start + 5ms)) << "the queued node is still there";
    EXPECT_EQ(work.runs, 1);
}

TEST_F(ShardContextTest, ControlPostAcceptedBeforeDrainOrRejectedAfterClosure) {
    Counting first;
    Counting second;
    EXPECT_FALSE(context_.try_finish()) << "no stop requested: nothing to finish, no lock taken";
    EXPECT_TRUE(context_.post_control(first));
    EXPECT_FALSE(context_.drained());
    EXPECT_TRUE(context_.run_once(start));
    EXPECT_EQ(first.runs, 1);
    {
        const aloe::runtime::ShardContext::Current current{context_};
        context_.request_stop();
    }
    EXPECT_TRUE(context_.post_control(second)) << "stop requested but not yet finished: still accepted";
    EXPECT_FALSE(context_.try_finish()) << "the accepted node keeps the context open";
    EXPECT_TRUE(context_.run_once(start));
    EXPECT_EQ(second.runs, 1);
    EXPECT_TRUE(context_.try_finish());
    Counting late;
    EXPECT_FALSE(context_.post_control(late)) << "closed: the node stays the caller's";
    EXPECT_EQ(late.runs, 0);
    EXPECT_FALSE(context_.run_once(start));
    EXPECT_EQ(late.runs, 0) << "nothing reached the inbox";
}

TEST_F(ShardContextTest, ArpSinkDeliversOnTheOwnerWithTheStamp) {
    struct Learned {
        aloe::wire::Ipv4Address address{};
        aloe::wire::MacAddress mac{};
        TimePoint at{};
        int calls = 0;
    } learned{};
    context_.set_arp_sink({.object = &learned,
                           .learn  = [](void* object,
                                        const aloe::wire::Ipv4Address address,
                                        const aloe::wire::MacAddress mac,
                                        const TimePoint now) noexcept {
                               auto& self   = *static_cast<Learned*>(object);
                               self.address = address;
                               self.mac     = mac;
                               self.at      = now;
                               ++self.calls;
                           }});
    context_.set_now(start + 3ms);
    context_.deliver_arp({10, 0, 0, 254}, {0x02, 0, 0, 0, 0, 0xfe});
    EXPECT_EQ(learned.calls, 1);
    EXPECT_EQ(learned.address, (aloe::wire::Ipv4Address{10, 0, 0, 254}));
    EXPECT_EQ(learned.mac, (aloe::wire::MacAddress{0x02, 0, 0, 0, 0, 0xfe}));
    EXPECT_EQ(learned.at, start + 3ms);
    context_.set_arp_sink({});
    context_.deliver_arp({10, 0, 0, 1}, {});
    EXPECT_EQ(learned.calls, 1) << "no sink: discarded";
}
