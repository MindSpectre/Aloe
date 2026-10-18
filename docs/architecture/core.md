# Core Module

The core module (`common/core/`) is Aloe's foundation: the small things every module links. The library
version, the clock and stamp types the whole stack shares with the TSC clock behind them, and header-only
helpers: discarding values on
purpose, choosing a fallback, a string literal as a template parameter, and two guards that pin what a
member function is. It has no dependencies and keeps none. Everything is reached through the umbrella
`#include <aloe/core>` (`export/aloe/core`), and targets link `Aloe::Common::Core`. The execution facilities
and logging are modules of their own, [`execution`](execution.md) and [`log`](log.md), linked on demand.

## Key types

- **`aloe::core::Clock`, `aloe::core::TimePoint`, `aloe::core::Duration`** -- the stack's clock, its time
  point, and `std::chrono::nanoseconds`. `Clock` is `TscClock`, or `std::chrono::steady_clock` in a build
  configured with `-DUSE_TSC_CLOCK=OFF`, as the `ci` preset is. A loop reads `Clock` once per iteration and
  passes the `TimePoint` down; bricks never read a clock themselves. Configs spell their intervals as
  `Duration`.
- **`aloe::core::calibrate_clock()`** -- calibrates `Clock` during setup, so the TSC clock's 100 ms of
  calibration and any refusal, as an exception, happen there and not at the first `Clock::now()`. Does
  nothing for the steady clock. `runtime::Runtime` calls it in its constructor; a hand-written loop calls it
  before its first stamp.
- **`aloe::core::TscClock`** -- a steady `std::chrono` clock over the CPU's time-stamp counter: `now()` is a
  fenced `rdtsc`, a multiply and a shift, with no system call. `scale()` calibrates it once per process from
  `trust(probe())` and throws `std::runtime_error` on a machine whose TSC cannot be trusted. `probe()` reads
  the CPU's invariant-TSC flag and the kernel's current clocksource, and counts ticks across 100 ms of
  `CLOCK_MONOTONIC_RAW`; `trust(evidence)` judges what it found (a `TscEvidence`).
- **`aloe::core::TscScale`** -- ticks to nanoseconds as `(ticks * multiplier) >> shift` with a 32-bit
  multiplier, made by `measured(ticks, nanoseconds)` from one measurement. Exact (rounded down) for any
  count whose result fits 64 bits, about 584 years.
- **`aloe::core::version_major`, `version_minor`, `version_patch`, `version_string`** -- the library version as
  `constexpr` values, generated from the version in the root `CMakeLists.txt`, so the two cannot drift.
- **`aloe::core::unused_value(values...)`** -- discards any number of values inside a body, where a
  template decides not to use something it was handed. `[[maybe_unused]]` marks a declaration and
  `std::ignore = f()` discards one result at a call; this is the visible form for everything else. Every
  argument is evaluated exactly once.
- **`aloe::core::value_or(value, fallback)`** -- GCC's `value ?: fallback` as a function: `value` when it
  converts to true, else `fallback`, with `value` evaluated once. Two lvalues of one type yield that lvalue,
  so nothing is copied; any other mix yields a value of the common type.
- **`aloe::core::FixedString<N>`** -- a string literal as a template parameter: `template <FixedString Text>`.
  Structural, so equal texts are one specialisation. The logger takes its format strings this way.
- **`aloe::core::force_non_const(this)`** -- does not compile inside a const member function. A const
  member may still write through a pointer member, so the compiler never objects when such a function is
  declared const by mistake; this does.
- **`aloe::core::force_non_static(this)`** -- does not compile inside a static member function, where there
  is no `this`.

## Usage

```cpp
#include <aloe/core>

class Handle {
public:
    void set(const int value) noexcept {
        aloe::core::force_non_const(this);  // writes through target_, which a const member could still do
        *target_ = value;
    }

private:
    int* target_ = nullptr;
};

template <typename... Args>
void ignore_all(Args&&... args) {
    aloe::core::unused_value(args...);
}

const char* name_or_default(const char* name) {
    return aloe::core::value_or(name, "anonymous");
}

bool due(const aloe::core::TimePoint now, const aloe::core::TimePoint deadline) {
    return now >= deadline;
}

int main() {
    aloe::core::calibrate_clock();  // 100 ms with the TSC clock, or an exception on a machine that cannot trust it
    const aloe::core::TimePoint start = aloe::core::Clock::now();
    // ... the loop: one stamp per iteration, passed down
}
```

## Design notes

Core has no dependencies so that every module can link it, the hot-path ones included. stdexec and quill
live in [`execution`](execution.md) and [`log`](log.md), which only the runtime, the examples and the tests
link, so a brick cannot include either by accident.

The clock aliases exist so the stack has one clock: a stamp from the runtime's loop, a brick's `now`
parameter and a timer's deadline are the same type without each class declaring its own alias.
`Duration` is `std::chrono::nanoseconds` by name rather than `Clock::duration`, so a config's units do not
depend on the standard library's choice.

**Why the TSC.** Every iteration of every loop reads the clock. `steady_clock` goes through the vDSO, which
reads the TSC itself and then converts it under a sequence lock; on a machine whose kernel has left the TSC
it becomes a system call. `TscClock` keeps only the fenced read and the conversion. The fence (`lfence`
before `rdtsc`) is Linux's: without it the counter can be read before an earlier load completes, and a stamp
taken after seeing another thread's stamp could be earlier than it.

**Trust the kernel's verdict.** Calibration accepts the TSC only when the CPU reports it invariant (one rate
in every power state) and the kernel's current clocksource is `tsc`. The kernel checks at boot and while it
runs that every core's counter agrees and stays stable, and switches away from the TSC when it does not; if
Linux will not use it, neither does Aloe. The frequency is measured, not read: the CPU on the development
machine (a Ryzen) reports none, and DPDK measures it the same way, against `CLOCK_MONOTONIC_RAW` across
100 ms, each end of the interval taken as the tightest of five reads of the reference between two counter
reads, as Linux's `tsc_read_refs` brackets its own. The conversion is the kernel's form: a 32-bit multiplier
with the largest shift that keeps it so, and a product taken in two 32-bit halves (`mul_u64_u32_shr`), exact
for any count whose result fits 64 bits.

**Refuse, never fall back.** A build that chose the TSC and lands on a machine that fails either check stops
with an exception that names the option, rather than quietly reading `steady_clock`: a silent fallback
would hide the cost the clock exists to remove. Virtual machines, CI runners among them, usually fail the
clocksource check, so the `ci` preset builds with `USE_TSC_CLOCK=OFF`. The option sets `ALOE_USE_TSC_CLOCK`
for every target in `cmake/features.cmake`, so every translation unit reads the same clock; `clock.hpp`
tests it and defines no macro of its own. `TscClock`'s stamps count from the counter's zero, not
from `steady_clock`'s epoch, and the two time point types do not convert: a stamp from the wrong clock does
not compile where the stack expects one.

The guards cost nothing: empty `constexpr` functions whose only content is a `static_assert` on the deduced
type. They exist for handle types such as the ethdev packet, whose state lives behind a pointer. There,
`const` on a member function is a promise the compiler cannot check, and the guard turns it into one it can.

The version header is a CMake template (`version/version.hpp.in`) instead of hand-written constants. The
version therefore has one source, the `project()` call, and a release cannot ship with a stale number.
