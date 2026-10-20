#pragma once

#include <algorithm>
#include <cerrno>
#include <system_error>
#include <thread>

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

    /// Lets the calling thread run on every online CPU. A thread spawned after the EAL started inherits the main
    /// lcore's mask, so a thread meant to be unpinned widens it first, or it shares the stack's core. A kernel that
    /// takes none of the CPUs (`EINVAL`) leaves the inherited mask in place; any other refusal throws
    /// std::system_error.
    inline void widen_to_all_cpus() {
        cpu_set_t set;
        CPU_ZERO(&set);
        const unsigned cpus = std::min<unsigned>(std::thread::hardware_concurrency(), CPU_SETSIZE);
        for (unsigned cpu = 0; cpu < cpus; ++cpu) {
            CPU_SET(cpu, &set);
        }
        if (const int error = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
            error != 0 && error != EINVAL) {
            throw std::system_error{error, std::generic_category(), "pthread_setaffinity_np"};
        }
    }

}  // namespace aloe::benchmarks
