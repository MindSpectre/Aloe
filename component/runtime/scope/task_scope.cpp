#include <cassert>
#include <cstdint>
#include <exception>
#include <string>
#include <thread>
#include <utility>

#include <task_scope.hpp>

namespace aloe::runtime {

    TaskScope::TaskScope(const core::Logger logger, const std::uint16_t index, ShardCounters& counters) noexcept
        : logger_{logger},
          index_{index},
          counters_{&counters} {
    }

    TaskScope::~TaskScope() {
        assert(live_ == 0 && "a TaskScope is destroyed with work still running; drain the shard first");
    }

    void TaskScope::request_stop() noexcept {
        stop_source_.request_stop();
    }

    void TaskScope::finished(const detail::Outcome outcome) noexcept {
        if (outcome == detail::Outcome::Value) {
            ++counters_->tasks_completed;
        } else {
            ++counters_->tasks_stopped;
        }
        one_less();
    }

    void TaskScope::failed(std::exception_ptr error) noexcept {
        ++counters_->tasks_failed;
        std::string what = "an error that is not an exception";
        if (error) {
            try {
                std::rethrow_exception(std::move(error));
            } catch (const std::exception& exception) {
                what = exception.what();
            } catch (...) {
                what = "an exception that is not a std::exception";
            }
        }
        logger_.error<"shard {}: a task failed: {}">(index_, what);
        one_less();
    }

    void TaskScope::one_less() noexcept {
        assert(
            owner_ == std::this_thread::get_id() &&
            "a task completed off its shard: a shard task awaits only its own shard's senders (threading contract 4)");
        --live_;
        if (live_ == 0 && joiner_ != nullptr) {
            detail::JoinBase* join = std::exchange(joiner_, nullptr);
            join->complete(*join);
        }
    }

    void TaskScope::wait(detail::JoinBase& join) noexcept {
        assert(joiner_ == nullptr && "one join at a time");
        joiner_ = &join;
    }

}  // namespace aloe::runtime
