#include <algorithm>
#include <aloe/loop>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using TimePoint = aloe::loop::Timer::TimePoint;

    constexpr TimePoint start{};
    constexpr auto resolution = 1ms;

    [[nodiscard]] TimePoint at(const std::chrono::nanoseconds offset) {
        return start + offset;
    }

    struct Fired {
        int id;
        TimePoint now;  ///< The stamp of the `advance` that fired it.
    };

    /// Records itself when fired; can re-arm itself or cancel another timer from inside `fire`.
    struct Probe : aloe::loop::Timer {
        Probe(std::vector<Fired>& record, const TimePoint* now, const int id)
            : Timer{&Probe::execute},
              record_{&record},
              now_{now},
              id_{id} {
        }

        void rearm_once_from_fire(aloe::loop::TimerWheel& wheel, const TimePoint when) noexcept {
            rearm_wheel_    = &wheel;
            rearm_deadline_ = when;
        }

        void cancel_from_fire(aloe::loop::TimerWheel& wheel, Probe& victim) noexcept {
            cancel_wheel_  = &wheel;
            cancel_victim_ = &victim;
        }

        static void execute(aloe::loop::Timer& timer) noexcept {
            auto& self = static_cast<Probe&>(timer);
            self.record_->push_back({self.id_, *self.now_});
            if (self.rearm_wheel_ != nullptr) {
                aloe::loop::TimerWheel* wheel = self.rearm_wheel_;
                self.rearm_wheel_             = nullptr;
                wheel->arm(self, self.rearm_deadline_);
            }
            if (self.cancel_wheel_ != nullptr) {
                self.cancel_wheel_->cancel(*self.cancel_victim_);
                self.cancel_wheel_ = nullptr;
            }
        }

    private:
        std::vector<Fired>* record_;
        const TimePoint* now_;
        int id_;
        aloe::loop::TimerWheel* rearm_wheel_ = nullptr;
        TimePoint rearm_deadline_{};
        aloe::loop::TimerWheel* cancel_wheel_ = nullptr;
        Probe* cancel_victim_                 = nullptr;
    };

    class TimerWheelTest : public testing::Test {
    protected:
        std::vector<Fired> record_;
        TimePoint now_ = start;
        aloe::loop::TimerWheel wheel_{resolution, start};

        std::size_t advance(const TimePoint now) {
            now_ = now;
            return wheel_.advance(now);
        }

        [[nodiscard]] std::vector<int> fired_ids() const {
            std::vector<int> ids;
            ids.reserve(record_.size());
            for (const Fired& fired : record_) {
                ids.push_back(fired.id);
            }
            return ids;
        }
    };

}  // namespace

TEST_F(TimerWheelTest, NeverFiresEarlyAndAtMostOneResolutionLate) {
    Probe exact{record_, &now_, 1};
    Probe between{record_, &now_, 2};
    wheel_.arm(exact, at(5ms));
    wheel_.arm(between, at(5ms + 1us));
    EXPECT_EQ(wheel_.pending(), 2);

    EXPECT_EQ(advance(at(4ms + 999us)), 0) << "before both deadlines";
    EXPECT_EQ(advance(at(5ms)), 1) << "the exact deadline fires on its tick";
    EXPECT_EQ(fired_ids(), (std::vector<int>{1}));
    EXPECT_EQ(advance(at(5ms + 999us)), 0) << "one microsecond past the deadline is still inside tick 5";
    EXPECT_EQ(advance(at(6ms)), 1) << "rounded up to tick 6, one resolution late at most";
    EXPECT_EQ(fired_ids(), (std::vector<int>{1, 2}));
    EXPECT_EQ(wheel_.pending(), 0);
    EXPECT_FALSE(exact.armed());
    EXPECT_FALSE(between.armed());
}

TEST_F(TimerWheelTest, FiresInDeadlineOrderAcrossSlotsAndInArmOrderWithinOne) {
    Probe late{record_, &now_, 30};
    Probe early{record_, &now_, 10};
    Probe middle{record_, &now_, 20};
    Probe same_a{record_, &now_, 1};
    Probe same_b{record_, &now_, 2};
    Probe same_c{record_, &now_, 3};
    wheel_.arm(late, at(30ms));
    wheel_.arm(early, at(10ms));
    wheel_.arm(middle, at(20ms));
    wheel_.arm(same_a, at(20ms));
    wheel_.arm(same_b, at(20ms));
    wheel_.arm(same_c, at(20ms));

    EXPECT_EQ(advance(at(100ms)), 6);
    EXPECT_EQ(fired_ids(), (std::vector<int>{10, 20, 1, 2, 3, 30}));
}

TEST_F(TimerWheelTest, CancelPreventsFiringAndIsIdempotent) {
    Probe kept{record_, &now_, 1};
    Probe cancelled{record_, &now_, 2};
    wheel_.arm(kept, at(10ms));
    wheel_.arm(cancelled, at(10ms));
    wheel_.cancel(cancelled);
    wheel_.cancel(cancelled);
    EXPECT_FALSE(cancelled.armed());
    EXPECT_EQ(wheel_.pending(), 1);

    EXPECT_EQ(advance(at(10ms)), 1);
    EXPECT_EQ(fired_ids(), (std::vector<int>{1}));
}

TEST_F(TimerWheelTest, ReArmingMovesTheDeadline) {
    Probe timer{record_, &now_, 1};
    wheel_.arm(timer, at(10ms));
    wheel_.arm(timer, at(50ms));
    EXPECT_EQ(wheel_.pending(), 1) << "re-arming does not count twice";

    EXPECT_EQ(advance(at(20ms)), 0);
    EXPECT_EQ(advance(at(50ms)), 1);
    EXPECT_EQ(wheel_.pending(), 0);
}

TEST_F(TimerWheelTest, ADeadlineInThePastFiresOnTheNextAdvance) {
    EXPECT_EQ(advance(at(100ms)), 0);
    Probe timer{record_, &now_, 1};
    wheel_.arm(timer, at(50ms));
    EXPECT_TRUE(timer.armed());

    EXPECT_EQ(advance(at(100ms)), 1) << "the same stamp again still fires it";
    EXPECT_EQ(fired_ids(), (std::vector<int>{1}));
}

// A timer that re-arms itself in the past from its own fire runs once per advance.
TEST_F(TimerWheelTest, ReArmFromFireWaitsForTheNextAdvance) {
    Probe timer{record_, &now_, 1};
    timer.rearm_once_from_fire(wheel_, at(0ms));
    wheel_.arm(timer, at(10ms));

    EXPECT_EQ(advance(at(10ms)), 1);
    EXPECT_TRUE(timer.armed()) << "re-armed into the due list";
    EXPECT_EQ(advance(at(10ms)), 1);
    EXPECT_FALSE(timer.armed());
    EXPECT_EQ(fired_ids(), (std::vector<int>{1, 1}));
}

// Cancelling a timer from another timer's fire, in the same slot, skips it cleanly.
TEST_F(TimerWheelTest, CancelFromAnotherTimersFireInTheSameSlotSkipsIt) {
    Probe first{record_, &now_, 1};
    Probe victim{record_, &now_, 2};
    Probe third{record_, &now_, 3};
    first.cancel_from_fire(wheel_, victim);
    wheel_.arm(first, at(10ms));
    wheel_.arm(victim, at(10ms));
    wheel_.arm(third, at(10ms));

    EXPECT_EQ(advance(at(10ms)), 2);
    EXPECT_EQ(fired_ids(), (std::vector<int>{1, 3}));
    EXPECT_FALSE(victim.armed());
    EXPECT_EQ(wheel_.pending(), 0);
}

TEST_F(TimerWheelTest, DeadlinesFarOutCrossLevelsAndFireOnTime) {
    Probe level1{record_, &now_, 1};
    Probe level2{record_, &now_, 2};
    Probe level3{record_, &now_, 3};
    wheel_.arm(level1, at(300ms));                                                   // 256 <= delta < 65536
    wheel_.arm(level2, at(70'000ms));                                                // 65536 <= delta < 2^24
    wheel_.arm(level3, at(std::chrono::milliseconds{(std::int64_t{1} << 24) + 5}));  // beyond 2^24 ticks

    EXPECT_EQ(advance(at(299ms)), 0);
    EXPECT_EQ(advance(at(300ms)), 1);
    EXPECT_EQ(advance(at(69'999ms)), 0);
    EXPECT_EQ(advance(at(70'000ms)), 1);
    EXPECT_EQ(advance(at(std::chrono::milliseconds{(std::int64_t{1} << 24) + 4})), 0);
    EXPECT_EQ(advance(at(std::chrono::milliseconds{(std::int64_t{1} << 24) + 5})), 1);
    EXPECT_EQ(fired_ids(), (std::vector<int>{1, 2, 3}));
}

TEST_F(TimerWheelTest, RandomisedArmsAndCancelsFireOnTheFirstAdvanceThatReachesThem) {
    // A fixed seed is the point: the run must be reproducible.
    std::mt19937 generator{20261002};  // NOLINT(cert-msc51-cpp)
    std::uniform_int_distribution<std::int64_t> deadline_ms{0, 200'000};
    std::uniform_int_distribution<int> coin{0, 9};
    constexpr int count = 400;

    std::vector<std::unique_ptr<Probe>> probes;
    std::vector<TimePoint> deadlines;
    std::vector<bool> cancelled(count, false);
    for (int id = 0; id < count; ++id) {
        probes.push_back(std::make_unique<Probe>(record_, &now_, id));
        deadlines.push_back(at(std::chrono::milliseconds{deadline_ms(generator)}));
        wheel_.arm(*probes.back(), deadlines.back());
    }
    for (int id = 0; id < count; ++id) {
        if (coin(generator) == 0) {
            wheel_.cancel(*probes[static_cast<std::size_t>(id)]);
            cancelled[static_cast<std::size_t>(id)] = true;
        }
    }

    std::uniform_int_distribution<std::int64_t> step_us{1, 3'000'000};
    TimePoint previous = start;
    TimePoint now      = start;
    std::vector<TimePoint> previous_stamp;  // the advance before the one that fired each record
    while (now < at(210'000ms)) {
        previous                  = now;
        now                      += std::chrono::microseconds{step_us(generator)};
        const std::size_t before  = record_.size();
        advance(now);
        for (std::size_t index = before; index < record_.size(); ++index) {
            previous_stamp.push_back(previous);
        }
    }

    std::vector<int> fired = fired_ids();
    std::vector<int> expected;
    for (int id = 0; id < count; ++id) {
        if (!cancelled[static_cast<std::size_t>(id)]) {
            expected.push_back(id);
        }
    }
    std::ranges::sort(fired);
    EXPECT_EQ(fired, expected) << "every armed timer fired once, no cancelled one did";

    for (std::size_t index = 0; index < record_.size(); ++index) {
        const auto id       = static_cast<std::size_t>(record_[index].id);
        const auto deadline = deadlines[id];
        EXPECT_GE(record_[index].now, deadline) << "timer " << id << " fired early";
        EXPECT_LT(previous_stamp[index], deadline + resolution)
            << "timer " << id << " should have fired on the previous advance";
    }
    EXPECT_EQ(wheel_.pending(), 0);
}
