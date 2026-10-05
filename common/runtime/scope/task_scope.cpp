#include <aloe/core>
#include <aloe/log>
#include <cassert>
#include <cstdint>
#include <exception>
#include <string>
#include <thread>
#include <utility>

#include <task_scope.hpp>

namespace aloe::runtime {

    TaskScope::TaskScope(const log::Logger logger, const std::uint16_t index, loop::ShardCounters& counters) noexcept
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

    void TaskScope::assert_owner() const noexcept {
        core::force_non_static(this);  // the assert below is all there is, and NDEBUG removes it
        assert(
            owner_ == std::this_thread::get_id() &&
            "a task completed off its shard: a shard task awaits only its own shard's senders (threading contract 4)");
    }

    void TaskScope::finished(const detail::Outcome outcome) noexcept {
        assert_owner();
        if (outcome == detail::Outcome::Value) {
            ++counters_->tasks_completed;
        } else {
            ++counters_->tasks_stopped;
        }
        one_less();
    }

    void TaskScope::failed(std::exception_ptr error) noexcept {
        assert_owner();
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
