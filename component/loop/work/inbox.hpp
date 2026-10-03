#pragma once

#include <atomic>

#include <work.hpp>

namespace aloe::loop {

    /**
     * @brief Multi-producer, single-consumer intrusive queue of Work: a shard's inbox.
     *
     * Dmitry Vyukov's intrusive MPSC queue. `push` runs on any thread, is wait-free and costs one
     * atomic exchange and one release store. `pop` and `empty` run on the owning thread only and
     * cost one acquire load. Nothing allocates: the node is the pusher's operation state, which
     * stays alive until the shard runs it.
     *
     * `pop` returns nothing while a producer is between its exchange and its link store; the node
     * is not lost and the next `pop` sees it. `empty` is false in that state, which is what the
     * drain check needs.
     */
    class Inbox {
    public:
        Inbox() noexcept               = default;
        Inbox(const Inbox&)            = delete;
        Inbox& operator=(const Inbox&) = delete;
        Inbox(Inbox&&)                 = delete;
        Inbox& operator=(Inbox&&)      = delete;
        ~Inbox()                       = default;

        void push(Work& work) noexcept {
            work.next.store(nullptr, std::memory_order_relaxed);
            Work* previous = tail_.exchange(&work, std::memory_order_acq_rel);
            previous->next.store(&work, std::memory_order_release);
        }

        [[nodiscard]] Work* pop() noexcept {
            Work* front = head_;
            Work* next  = front->next.load(std::memory_order_acquire);
            if (front == &stub_) {
                if (next == nullptr) {
                    return nullptr;
                }
                head_ = next;
                front = next;
                next  = next->next.load(std::memory_order_acquire);
            }
            if (next != nullptr) {
                head_ = next;
                return front;
            }
            if (tail_.load(std::memory_order_acquire) != front) {
                return nullptr;  // a producer has exchanged the tail and not yet linked its node
            }
            push(stub_);
            next = front->next.load(std::memory_order_acquire);
            if (next != nullptr) {
                head_ = next;
                return front;
            }
            return nullptr;
        }

        /// Consumer thread only. False while anything is queued or on its way.
        [[nodiscard]] bool empty() const noexcept {
            return head_ == &stub_ && tail_.load(std::memory_order_acquire) == &stub_;
        }

    private:
        Work stub_;
        std::atomic<Work*> tail_{&stub_};
        Work* head_ = &stub_;
    };

}  // namespace aloe::loop
