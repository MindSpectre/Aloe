#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include <timer_wheel.hpp>

namespace aloe::runtime {

    namespace {

        constexpr unsigned bits_per_level     = 8;
        constexpr std::uint64_t slot_mask     = TimerWheel::slots_per_level - 1;
        constexpr std::uint64_t horizon_ticks = std::uint64_t{1} << (bits_per_level * TimerWheel::levels);

        /// A sentinel is a list head linked to itself.
        void make_empty(Timer& head) noexcept {
            head.next = &head;
            head.prev = &head;
        }

        /// A sentinel that no longer heads a list must not look armed to the Timer destructor.
        void retire(Timer& head) noexcept {
            head.next = nullptr;
            head.prev = nullptr;
        }

        /// Appends `timer` at the end of the list `head` heads.
        void link(Timer& head, Timer& timer) noexcept {
            timer.prev      = head.prev;
            timer.next      = &head;
            head.prev->next = &timer;
            head.prev       = &timer;
        }

        void unlink(Timer& timer) noexcept {
            timer.prev->next = timer.next;
            timer.next->prev = timer.prev;
            timer.next       = nullptr;
            timer.prev       = nullptr;
        }

        [[nodiscard]] bool is_empty(const Timer& head) noexcept {
            return head.next == &head;
        }

    }  // namespace

    TimerWheel::TimerWheel(const Duration resolution, const TimePoint start)
        : resolution_{resolution},
          start_{start},
          now_{start} {
        if (resolution <= Duration::zero()) {
            throw std::invalid_argument{"timer wheel resolution must be positive"};
        }
        make_empty(due_);
        for (auto& level : slots_) {
            for (Timer& slot : level) {
                make_empty(slot);
            }
        }
    }

    TimerWheel::~TimerWheel() {
        retire(due_);
        for (auto& level : slots_) {
            for (Timer& slot : level) {
                retire(slot);
            }
        }
    }

    std::uint64_t TimerWheel::tick_of(const TimePoint time) const noexcept {
        if (time <= start_) {
            return 0;
        }
        return static_cast<std::uint64_t>((time - start_) / resolution_);
    }

    std::uint64_t TimerWheel::deadline_tick(const TimePoint deadline) const noexcept {
        if (deadline <= start_) {
            return 0;
        }
        const Duration elapsed = deadline - start_;
        const auto whole       = static_cast<std::uint64_t>(elapsed / resolution_);
        return (elapsed % resolution_ == Duration::zero()) ? whole : whole + 1;
    }

    void TimerWheel::arm(Timer& timer, const TimePoint deadline) noexcept {
        if (timer.armed()) {
            unlink(timer);
        } else {
            ++armed_;
        }
        timer.deadline = deadline;
        place(timer, deadline_tick(deadline), false);
    }

    void TimerWheel::cancel(Timer& timer) noexcept {
        if (!timer.armed()) {
            return;
        }
        unlink(timer);
        --armed_;
    }

    void TimerWheel::place(Timer& timer, std::uint64_t tick, const bool cascading) noexcept {
        if (tick <= current_tick_) {
            // Already due. From `arm` it waits for the next advance; from a cascade it fires in the
            // tick being processed, whose level-0 slot is fired right after the cascade.
            link(cascading ? slots_[0][current_tick_ & slot_mask] : due_, timer);
            return;
        }
        std::uint64_t delta = tick - current_tick_;
        if (delta >= horizon_ticks) {
            delta = horizon_ticks - 1;
            tick  = current_tick_ + delta;
        }
        std::size_t level = 0;
        while (level + 1 < levels && delta >= (std::uint64_t{1} << (bits_per_level * (level + 1)))) {
            ++level;
        }
        const auto slot = static_cast<std::size_t>((tick >> (bits_per_level * level)) & slot_mask);
        link(slots_[level][slot], timer);
    }

    void TimerWheel::cascade(const std::size_t level, const std::size_t slot) noexcept {
        Timer& head = slots_[level][slot];
        Timer pending;
        if (is_empty(head)) {
            retire(pending);
            return;
        }
        // Splice the slot's list onto a local head, then re-place each timer in order.
        pending.next       = head.next;
        pending.prev       = head.prev;
        pending.next->prev = &pending;
        pending.prev->next = &pending;
        make_empty(head);
        while (!is_empty(pending)) {
            Timer& timer = *pending.next;
            unlink(timer);
            place(timer, deadline_tick(timer.deadline), true);
        }
        retire(pending);
    }

    std::size_t TimerWheel::fire_list(Timer& head) noexcept {
        Timer pending;
        if (is_empty(head)) {
            retire(pending);
            return 0;
        }
        // Splice onto a local head and pop from the front: a timer cancelled from another's fire is
        // unlinked from `pending` before it is reached, and a timer re-armed from its own fire goes
        // to a slot or the due list, never back into `pending`.
        pending.next       = head.next;
        pending.prev       = head.prev;
        pending.next->prev = &pending;
        pending.prev->next = &pending;
        make_empty(head);
        std::size_t count = 0;
        while (!is_empty(pending)) {
            Timer& timer = *pending.next;
            unlink(timer);
            --armed_;
            ++count;
            timer.fire(timer);
        }
        retire(pending);
        return count;
    }

    std::size_t TimerWheel::advance(const TimePoint now) noexcept {
        if (now > now_) {
            now_ = now;
        }
        std::size_t fired          = fire_list(due_);
        const std::uint64_t target = tick_of(now_);
        while (current_tick_ < target) {
            ++current_tick_;
            const std::uint64_t tick = current_tick_;
            if ((tick & slot_mask) == 0) {
                cascade(1, static_cast<std::size_t>((tick >> bits_per_level) & slot_mask));
                if (((tick >> bits_per_level) & slot_mask) == 0) {
                    cascade(2, static_cast<std::size_t>((tick >> (2 * bits_per_level)) & slot_mask));
                    if (((tick >> (2 * bits_per_level)) & slot_mask) == 0) {
                        cascade(3, static_cast<std::size_t>((tick >> (3 * bits_per_level)) & slot_mask));
                    }
                }
            }
            fired += fire_list(slots_[0][tick & slot_mask]);
        }
        return fired;
    }

}  // namespace aloe::runtime
