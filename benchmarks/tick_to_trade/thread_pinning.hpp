#pragma once

#include <cerrno>
#include <system_error>

#include <pthread.h>
#include <sched.h>

namespace aloe::benchmarks {

    /// Pins the calling thread to `cpu`, as the runtime pins a shard. Throws std::system_error when the CPU does not
    /// exist or is not allowed.
    inline void pin_current_thread(const unsigned cpu) {
        if (cpu >= CPU_SETSIZE) {
            throw std::system_error{EINVAL, std::generic_category(), "cpu beyond CPU_SETSIZE"};
        }
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (const int error = pthread_setaffinity_np(pthread_self(), sizeof(set), &set); error != 0) {
            throw std::system_error{error, std::generic_category(), "pthread_setaffinity_np"};
        }
    }

    /// Lets the calling thread run on every CPU the process may use. A thread spawned after the EAL started inherits
    /// the main lcore's mask, so a thread meant to be unpinned widens it first, or it shares the stack's core. Every
    /// bit is set and the kernel keeps the CPUs that exist and are allowed. Throws std::system_error when it refuses.
    inline void widen_to_all_cpus() {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (unsigned cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            CPU_SET(cpu, &set);
        }
        if (const int error = pthread_setaffinity_np(pthread_self(), sizeof(set), &set); error != 0) {
            throw std::system_error{error, std::generic_category(), "pthread_setaffinity_np"};
        }
    }

}  // namespace aloe::benchmarks
