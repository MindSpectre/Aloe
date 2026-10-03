#include <cstddef>

#include <shard_context.hpp>

namespace aloe::runtime {

    namespace {

        thread_local ShardContext* current_context = nullptr;

    }  // namespace

    ShardContext::ShardContext(const ShardContextConfig& config, const TimePoint start)
        : index_{config.index},
          logger_{core::logger("aloe.runtime")},
          now_{start},
          timers_{config.timer_resolution, start},
          scope_{logger_, config.index, counters_} {
    }

    ShardContext* ShardContext::current() noexcept {
        return current_context;
    }

    ShardContext::Current::Current(ShardContext& context) noexcept
        : previous_{current_context} {
        current_context = &context;
    }

    ShardContext::Current::~Current() {
        current_context = previous_;
    }

    bool ShardContext::run_once(const TimePoint now) noexcept {
        const Current current{*this};
        now_      = now;
        bool busy = false;
        for (Work* work = inbox_.pop(); work != nullptr; work = inbox_.pop()) {
            ready_.push(*work);
            ++counters_.inbox_received;
            busy = true;
        }
        const std::size_t fired  = timers_.advance(now);
        counters_.timers_fired  += fired;
        const std::size_t ran    = RunQueue::run_chain(ready_.take());
        counters_.work_run      += ran;
        return busy || fired > 0 || ran > 0;
    }

    void ShardContext::request_stop() noexcept {
        stopping_ = true;
        scope_.request_stop();
    }

}  // namespace aloe::runtime
