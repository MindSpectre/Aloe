#include <aloe/core>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#include <pthread.h>
#include <sched.h>

namespace {

    using namespace std::chrono_literals;
    using aloe::core::TscClock;
    using aloe::core::TscEvidence;

    // A machine the clock may trust: an invariant TSC the kernel uses, measured at 4.7 GHz over 100 ms.
    TscEvidence trustworthy() {
        return {.invariant_tsc = true, .clocksource = "tsc", .ticks = 470'000'000, .nanoseconds = 100'000'000};
    }

    // Tests that read this machine's TSC run in a build that chose the TSC clock: such a build asserts that the
    // machine qualifies. A build with USE_TSC_CLOCK=OFF, such as CI's, makes no such claim.
    constexpr bool tsc_build = ALOE_EXPECTED_TSC_CLOCK != 0;

    // A TSC stamp and a steady stamp taken together: the TSC read sits between two steady reads at most 20 us
    // apart, so a preemption between the reads cannot skew the pair.
    struct Reading {
        TscClock::time_point tsc{};
        std::chrono::steady_clock::time_point steady{};
    };

    Reading read_both() {
        for (;;) {
            const auto before = std::chrono::steady_clock::now();
            const auto tsc    = TscClock::now();
            const auto after  = std::chrono::steady_clock::now();
            if (after - before <= 20us) {
                return {.tsc = tsc, .steady = before + ((after - before) / 2)};
            }
        }
    }

    // The CPUs this process may run on, which taskset or a container's cpuset can narrow.
    std::vector<unsigned> allowed_cpus() {
        cpu_set_t set;
        CPU_ZERO(&set);
        std::vector<unsigned> cpus;
        if (sched_getaffinity(0, sizeof(set), &set) != 0) {
            return cpus;
        }
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (CPU_ISSET(cpu, &set)) {
                cpus.push_back(static_cast<unsigned>(cpu));
            }
        }
        return cpus;
    }

    bool pin_to(const unsigned cpu) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
    }

    // Puts the calling thread's CPU mask back when the test ends, so the tests after it run unpinned.
    class RestoreAffinity {
    public:
        RestoreAffinity() noexcept {
            CPU_ZERO(&saved_);
            saved_valid_ = pthread_getaffinity_np(pthread_self(), sizeof(saved_), &saved_) == 0;
        }
        RestoreAffinity(const RestoreAffinity&)            = delete;
        RestoreAffinity& operator=(const RestoreAffinity&) = delete;
        RestoreAffinity(RestoreAffinity&&)                 = delete;
        RestoreAffinity& operator=(RestoreAffinity&&)      = delete;
        ~RestoreAffinity() {
            if (saved_valid_) {
                pthread_setaffinity_np(pthread_self(), sizeof(saved_), &saved_);
            }
        }

    private:
        cpu_set_t saved_{};
        bool saved_valid_ = false;
    };

}  // namespace

TEST(TscClock, TrustsAnInvariantTscTheKernelUses) {
    const aloe::core::TscScale scale = TscClock::trust(trustworthy());
    EXPECT_NEAR(static_cast<double>(scale.nanoseconds(4'700'000'000)), 1e9, 1.0);
}

TEST(TscClock, RefusesATscThatIsNotInvariant) {
    TscEvidence evidence   = trustworthy();
    evidence.invariant_tsc = false;
    EXPECT_THROW(std::ignore = TscClock::trust(evidence), std::runtime_error);
}

TEST(TscClock, RefusesATscTheKernelDoesNotUse) {
    for (const std::string_view clocksource : {"hpet", "kvm-clock", ""}) {
        TscEvidence evidence = trustworthy();
        evidence.clocksource = clocksource;
        EXPECT_THROW(std::ignore = TscClock::trust(evidence), std::runtime_error) << '"' << clocksource << '"';
    }
}

// Two attempts: the first is preempted between its counter reads, a million ticks apart; the second is not. The
// pair comes from the second, its reference stamp matched to the middle of the two counter reads around it.
TEST(TscClock, PairsTheReferenceWithTheTightestBracket) {
    const std::array<std::uint64_t, 4> counter{1'000, 1'001'000, 2'000'000, 2'000'040};
    const std::array<std::uint64_t, 2> reference{500, 900'000};
    std::size_t next_tick       = 0;
    std::size_t next_nanosecond = 0;

    const aloe::core::detail::TscPair pair =
        aloe::core::detail::closest_pair([&counter, &next_tick] { return counter.at(next_tick++); },
                                         [&reference, &next_nanosecond] { return reference.at(next_nanosecond++); },
                                         2);
    EXPECT_EQ(pair.ticks, 2'000'020U);
    EXPECT_EQ(pair.nanoseconds, 900'000U);
}

TEST(TscClock, ThisMachineQualifies) {
    if (!tsc_build) {
        GTEST_SKIP() << "built with USE_TSC_CLOCK=OFF";
    }
    EXPECT_NO_THROW(std::ignore = TscClock::scale());
}

// A wrong multiplier or shift shows as a rate far from the steady clock's; calibration error and NTP slewing of the
// steady clock stay well inside 1 %.
TEST(TscClock, KeepsTheSteadyClocksRate) {
    if (!tsc_build) {
        GTEST_SKIP() << "built with USE_TSC_CLOCK=OFF";
    }
    const Reading start = read_both();
    std::this_thread::sleep_for(100ms);
    const Reading end = read_both();

    const double tsc_seconds    = std::chrono::duration<double>(end.tsc - start.tsc).count();
    const double steady_seconds = std::chrono::duration<double>(end.steady - start.steady).count();
    EXPECT_NEAR(tsc_seconds, steady_seconds, steady_seconds / 100);
}

// One thread publishes its stamps, another on a different core reads one and takes its own after it. A stamp taken
// after another was seen is never earlier: the kernel has verified that every core's TSC agrees, and the read is
// ordered after the load that saw the other stamp.
TEST(TscClock, NeverRunsBackwardsAcrossCores) {
    if (!tsc_build) {
        GTEST_SKIP() << "built with USE_TSC_CLOCK=OFF";
    }
    const std::vector<unsigned> cpus = allowed_cpus();
    if (cpus.size() < 2) {
        GTEST_SKIP() << "needs two CPUs this process may run on";
    }
    std::ignore = TscClock::scale();  // calibrate before the threads race
    const RestoreAffinity restore;

    std::atomic<std::int64_t> published{0};
    std::atomic<bool> done{false};
    std::atomic<bool> writer_pinned{false};
    std::jthread writer{[&published, &done, &writer_pinned, cpu = cpus.back()] {
        writer_pinned.store(pin_to(cpu));
        while (!done.load(std::memory_order_relaxed)) {
            published.store(TscClock::now().time_since_epoch().count(), std::memory_order_release);
        }
    }};

    const bool reader_pinned = pin_to(cpus.front());
    std::size_t backwards    = 0;
    for (int round = 0; round < 200'000; ++round) {
        const std::int64_t seen = published.load(std::memory_order_acquire);
        const std::int64_t mine = TscClock::now().time_since_epoch().count();
        if (mine < seen) {
            ++backwards;
        }
    }
    done.store(true, std::memory_order_relaxed);
    writer.join();
    EXPECT_TRUE(reader_pinned);
    EXPECT_TRUE(writer_pinned.load());
    EXPECT_EQ(backwards, 0U);
}
