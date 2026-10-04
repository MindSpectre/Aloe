#pragma once

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace aloe::loop {

    /**
     * @brief An intrusive timer node: a deadline and what to call when it is reached.
     *
     * The owner keeps the node alive while it is armed, or cancels it first; the destructor
     * asserts that in debug builds. `fire` is called on the shard thread, with the timer already
     * unarmed, so it may re-arm the timer or cancel others.
     */
    struct Timer {
        using Function  = void (*)(Timer&) noexcept;
        using TimePoint = std::chrono::steady_clock::time_point;

        Timer() noexcept = default;

        constexpr explicit Timer(const Function function) noexcept
            : fire{function} {
        }

        Timer(const Timer&)            = delete;
        Timer& operator=(const Timer&) = delete;
        Timer(Timer&&)                 = delete;
        Timer& operator=(Timer&&)      = delete;

        ~Timer() {
            assert(!armed() && "a Timer is destroyed while armed; cancel it first");
        }

        [[nodiscard]] bool armed() const noexcept {
            return next != nullptr;
        }

        Timer* next = nullptr;
        Timer* prev = nullptr;
        TimePoint deadline{};
        Function fire = nullptr;
    };

    /**
     * @brief A hierarchical timing wheel: arm and cancel in constant time, swept by `advance`.
     *
     * Four levels of 256 slots at a resolution fixed on construction, one millisecond by default
     * in a shard. Deadlines are rounded up to the resolution, so a timer never fires early, and it
     * fires on the first `advance` whose stamp reaches its tick, so it is late by at most one
     * resolution plus the gap between advances. A deadline at or before the last processed tick
     * fires on the next `advance`. Timers for one tick fire in the order they entered their slot.
     * Deadlines beyond 2^32 ticks, about fifty days at one millisecond, are kept at the horizon and
     * still fire at their deadline. One thread uses a wheel; nothing in it is synchronised.
     */
    class TimerWheel {
    public:
        using Clock     = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;
        using Duration  = Clock::duration;

        static constexpr std::size_t levels          = 4;
        static constexpr std::size_t slots_per_level = 256;

        /// Tick zero begins at `start`. Throws std::invalid_argument when `resolution` is not positive.
        TimerWheel(Duration resolution, TimePoint start);
        TimerWheel(const TimerWheel&)            = delete;
        TimerWheel& operator=(const TimerWheel&) = delete;
        TimerWheel(TimerWheel&&)                 = delete;
        TimerWheel& operator=(TimerWheel&&)      = delete;
        /// Every armed timer must have been cancelled or fired.
        ~TimerWheel();

        /// Arms, or re-arms with a new deadline.
        void arm(Timer& timer, TimePoint deadline) noexcept;
        /// A no-op on a timer that is not armed.
        void cancel(Timer& timer) noexcept;
        /// Fires everything due at `now` and returns how many fired. An earlier stamp than the last one fires only the
        /// due list.
        [[nodiscard]] std::size_t advance(TimePoint now) noexcept;

        [[nodiscard]] std::size_t pending() const noexcept {
            return armed_;
        }

        [[nodiscard]] Duration resolution() const noexcept {
            return resolution_;
        }

    private:
        [[nodiscard]] std::uint64_t tick_of(TimePoint time) const noexcept;
        [[nodiscard]] std::uint64_t deadline_tick(TimePoint deadline) const noexcept;
        void place(Timer& timer, std::uint64_t tick, bool cascading) noexcept;
        void cascade(std::size_t level, std::size_t slot) noexcept;
        std::size_t fire_list(Timer& head) noexcept;

        Duration resolution_;
        TimePoint start_;
        TimePoint now_;
        std::uint64_t current_tick_ = 0;  ///< The last tick processed.
        std::size_t armed_          = 0;
        Timer due_;  ///< Deadlines already reached, fired first on the next advance.
        std::array<std::array<Timer, slots_per_level>, levels> slots_;
    };

}  // namespace aloe::loop
