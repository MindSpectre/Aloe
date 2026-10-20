#include "hop.hpp"

#include <atomic>
#include <cstdint>
#include <exception>
#include <thread>

#include <thread_pinning.hpp>
#include <tsc_clock.hpp>

namespace aloe::benchmarks {

    namespace {

        enum class Placement : std::uint8_t {
            Pending,
            Placed,
            Refused,
        };

    }  // namespace

    Distribution measure_hop(const std::optional<unsigned> other_core, const std::size_t rounds) {
        Distribution hops{rounds};
        alignas(64) std::atomic<std::uint64_t> flag{0};
        std::atomic<Placement> placement{Placement::Pending};
        std::exception_ptr refusal;
        std::jthread other{[&flag, &placement, &refusal, other_core, rounds] {
            // Unpinned, the thread would inherit the caller's mask and share its core: the hop would time the
            // scheduler.
            try {
                if (other_core) {
                    pin_current_thread(*other_core);
                } else {
                    widen_to_all_cpus();
                }
            } catch (...) {
                refusal = std::current_exception();
                placement.store(Placement::Refused, std::memory_order_release);
                return;
            }
            placement.store(Placement::Placed, std::memory_order_release);
            for (std::uint64_t round = 0; round < rounds; ++round) {
                while (flag.load(std::memory_order_acquire) != 2 * round + 1) {}
                flag.store(2 * round + 2, std::memory_order_release);
            }
        }};
        Placement placed = Placement::Pending;
        while ((placed = placement.load(std::memory_order_acquire)) == Placement::Pending) {}
        if (placed == Placement::Refused) {
            other.join();
            std::rethrow_exception(refusal);
        }
        for (std::uint64_t round = 0; round < rounds; ++round) {
            const std::uint64_t t0 = core::TscClock::get_ticks();
            flag.store(2 * round + 1, std::memory_order_release);
            while (flag.load(std::memory_order_acquire) != 2 * round + 2) {}
            const std::uint64_t t1 = core::TscClock::get_ticks();
            hops.record((t1 - t0) / 2);
        }
        other.join();
        return hops;
    }

}  // namespace aloe::benchmarks
