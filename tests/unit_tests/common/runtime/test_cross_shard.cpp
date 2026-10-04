#include <aloe/fabric>
#include <aloe/runtime>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <thread>
#include <utility>

#include <echo_stack.hpp>
#include <gtest/gtest.h>
#include <logging_environment.hpp>

namespace {

    using namespace std::chrono_literals;

    constexpr int rounds = 5000;
    constexpr aloe::device::MacAddress server{0x02, 0, 0, 0, 0, 0x01};
    constexpr auto patience = 20s;

    // One logging environment per test binary; this file owns it for the Runtime.Threads target.
    const auto* const logging = ::testing::AddGlobalTestEnvironment(new aloe::testing::LoggingEnvironment{});

    [[nodiscard]] bool eventually(const std::function<bool()>& condition) {
        const auto deadline = std::chrono::steady_clock::now() + patience;
        while (std::chrono::steady_clock::now() < deadline) {
            if (condition()) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return condition();
    }

    /**
     * Hops between two shards through their inboxes: `other.schedule()` from wherever it is, then
     * `home.schedule()` from the other shard, `rounds` times. An operation state, not a task: the
     * receiver starts the next hop on the shard it landed on, and two slots alternate so a hop never
     * destroys the operation that is completing it. Counts every landing on the wrong shard.
     */
    class Hopper {
    public:
        Hopper(const aloe::runtime::Scheduler home,
               const aloe::runtime::Scheduler other,
               std::atomic<int>* wrong,
               std::atomic<int>* done) noexcept
            : home_{home},
              other_{other},
              wrong_{wrong},
              done_{done} {
        }

        Hopper(const Hopper&)            = delete;
        Hopper& operator=(const Hopper&) = delete;

        /// Any thread. Starts the first hop, towards `other`.
        void start() noexcept {
            hop();
        }

    private:
        struct Receiver {
            using receiver_concept = aloe::core::ex::receiver_t;
            Hopper* self;

            void set_value() const noexcept {
                self->landed();
            }

            void set_stopped() const noexcept {
                self->done_->store(-1);
            }
        };

        using HopSender = decltype(std::declval<const aloe::runtime::Scheduler&>().schedule());

        /// Builds the operation in place: operation states are immovable.
        struct Slot {
            Slot(const aloe::runtime::Scheduler target, Receiver receiver) noexcept
                : operation{aloe::core::ex::connect(target.schedule(), receiver)} {
            }

            aloe::core::ex::connect_result_t<HopSender, Receiver> operation;
        };

        /// Even hops go to `other`, odd hops come home.
        [[nodiscard]] aloe::runtime::Scheduler target() const noexcept {
            return hops_ % 2 == 0 ? other_ : home_;
        }

        void hop() noexcept {
            const auto slot = static_cast<std::size_t>(hops_ % 2);
            slots_[slot].emplace(target(), Receiver{this});
            aloe::core::ex::start(slots_[slot]->operation);
        }

        /// On the shard the hop targeted. The push into the next inbox orders every write here before the next landing.
        void landed() noexcept {
            if (aloe::runtime::ShardContext::current() != &target().context()) {
                ++*wrong_;
            }
            ++hops_;
            if (hops_ == 2 * rounds) {
                done_->store(1);
                return;
            }
            hop();
        }

        aloe::runtime::Scheduler home_;
        aloe::runtime::Scheduler other_;
        std::atomic<int>* wrong_;
        std::atomic<int>* done_;
        int hops_ = 0;
        std::array<std::optional<Slot>, 2> slots_;
    };

}  // namespace

// Two hoppers cross between two shards through their inboxes thousands of times, one started from each
// side. Under tsan this is the proof that the inbox and the scheduler's cross-shard path are race-free.
// The hoppers are declared before the runtime so that stop and join run before they are destroyed, on
// the failure path too; that is why the checks are EXPECTs and not ASSERTs.
TEST(CrossShard, HopsBetweenTwoShardsThroughTheirInboxes) {
    aloe::fabric::Fabric fabric;
    auto& port = fabric.add_port({.mac = server, .queues = 2, .pool_size = 16});
    std::atomic<int> wrong{0};
    std::atomic<int> done_a{0};
    std::atomic<int> done_b{0};
    std::optional<Hopper> a;
    std::optional<Hopper> b;

    aloe::runtime::Runtime<aloe::fabric::Port, aloe::testing::EchoStack<aloe::fabric::Port>> runtime{
        {.shard = {.idle = aloe::runtime::IdlePolicy::Yield, .yield_after = 10}, .threads = {}, .thread_hook = {}},
        port
    };
    runtime.start();
    a.emplace(runtime.scheduler(0), runtime.scheduler(1), &wrong, &done_a);
    b.emplace(runtime.scheduler(1), runtime.scheduler(0), &wrong, &done_b);
    a->start();
    b->start();
    EXPECT_TRUE(eventually([&] { return done_a.load() == 1 && done_b.load() == 1; }));
    runtime.stop();
    runtime.join();

    EXPECT_EQ(wrong.load(), 0);
    for (const std::uint16_t shard : {std::uint16_t{0}, std::uint16_t{1}}) {
        // Through each inbox: the stop, the home hopper's returns and the other hopper's visits.
        EXPECT_EQ(runtime.counters(shard).inbox_received, 1 + 2 * rounds) << "shard " << shard;
        EXPECT_EQ(runtime.counters(shard).work_run, 1 + 2 * rounds) << "shard " << shard;
    }
}
