#pragma once

#include <atomic>
#include <cstddef>

#include <work.hpp>

namespace aloe::runtime {

    /**
     * @brief Same-shard FIFO of Work. One thread, no synchronisation.
     *
     * `take` detaches the whole chain and leaves the queue empty, so work pushed while a chain runs
     * waits for the next `take`. That bounds one step of the shard loop.
     */
    class RunQueue {
    public:
        void push(Work& work) noexcept {
            work.next.store(nullptr, std::memory_order_relaxed);
            if (tail_ == nullptr) {
                head_ = &work;
            } else {
                tail_->next.store(&work, std::memory_order_relaxed);
            }
            tail_ = &work;
        }

        /// Everything queued, as a chain linked through `next`; the queue is empty afterwards.
        [[nodiscard]] Work* take() noexcept {
            Work* chain = head_;
            head_       = nullptr;
            tail_       = nullptr;
            return chain;
        }

        [[nodiscard]] bool empty() const noexcept {
            return head_ == nullptr;
        }

        /// Runs a detached chain in order. `next` is read before `run`, which may destroy or re-push the node.
        static std::size_t run_chain(Work* chain) noexcept {
            std::size_t count = 0;
            while (chain != nullptr) {
                Work* next = chain->next.load(std::memory_order_relaxed);
                chain->run(*chain);
                chain = next;
                ++count;
            }
            return count;
        }

    private:
        Work* head_ = nullptr;
        Work* tail_ = nullptr;
    };

}  // namespace aloe::runtime
