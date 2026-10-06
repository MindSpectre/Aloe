# Core Module

The core module (`common/core/`) is Aloe's foundation: the small things every module links. The library
version, the clock and stamp types the whole stack shares, and header-only helpers: discarding values on
purpose, choosing a fallback, a string literal as a template parameter, and two guards that pin what a
member function is. It has no dependencies and keeps none. Everything is reached through the umbrella
`#include <aloe/core>` (`export/aloe/core`), and targets link `Aloe::Common::Core`. The execution facilities
and logging are modules of their own, [`execution`](execution.md) and [`log`](log.md), linked on demand.

## Key types

- **`aloe::core::Clock`, `aloe::core::TimePoint`, `aloe::core::Duration`** -- `std::chrono::steady_clock`,
  its time point, and `std::chrono::nanoseconds`. A loop reads `Clock` once per iteration and passes the
  `TimePoint` down; bricks never read a clock themselves. Configs spell their intervals as `Duration`.
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
```

## Design notes

Core has no dependencies so that every module can link it, the hot-path ones included. stdexec and quill
live in [`execution`](execution.md) and [`log`](log.md), which only the runtime, the examples and the tests
link, so a brick cannot include either by accident.

The clock aliases exist so the stack has one clock: a stamp from the runtime's loop, a brick's `now`
parameter and a timer's deadline are the same type without each class declaring its own alias.
`Duration` is `std::chrono::nanoseconds` by name rather than `Clock::duration`, so a config's units do not
depend on the standard library's choice.

The guards cost nothing: empty `constexpr` functions whose only content is a `static_assert` on the deduced
type. They exist for handle types such as the ethdev packet, whose state lives behind a pointer. There,
`const` on a member function is a promise the compiler cannot check, and the guard turns it into one it can.

The version header is a CMake template (`version/version.hpp.in`) instead of hand-written constants. The
version therefore has one source, the `project()` call, and a release cannot ship with a stale number.
