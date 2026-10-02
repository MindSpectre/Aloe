# Core Module

The core module (`common/core/`) is Aloe's foundation: the library version, and the one header that names
the execution facilities every asynchronous part of the stack is written in. It depends on stdexec and on
nothing else in Aloe. Everything is reached through the umbrella `#include <aloe/core>`
(`export/aloe/core`), and targets link `Aloe::Common::Core`.

## Key types

- **`aloe::core::ex`** -- namespace alias for the senders-and-receivers facilities: `aloe::core::ex::just`,
  `aloe::core::ex::then`, `aloe::core::ex::sync_wait`, schedulers, stop tokens. It names stdexec today.
- **`aloe::core::task<T, Env>`** -- the C++26 coroutine task (P3552). Inside one, `co_await` accepts any
  sender. `Env` fixes the scheduler type, stop source, errors and allocator; the default erases the
  scheduler, and the runtime binds its concrete one.
- **`aloe::core::TaskEnvironment<Scheduler, StopSource>`** -- an environment for `task` that names a concrete
  scheduler. The one place that spells the member stdexec and P3552 name differently.
- **`aloe::core::completion_behavior`, `aloe::core::get_completion_behavior_t<Tag>`** -- how a sender says where
  it completes. A scheduler whose `schedule()` sender answers `asynchronous_affine` lets tasks await
  without rescheduling.
- **`aloe::core::version_major`, `version_minor`, `version_patch`, `version_string`** -- the library version as
  `constexpr` values, generated from the version in the root `CMakeLists.txt`, so the two cannot drift.

## Usage

```cpp
#include <aloe/core>

aloe::core::task<int> add_one(int value) {
    co_return value + 1;
}

aloe::core::task<int> twice_plus_one(int value) {
    const int incremented = co_await add_one(value);
    co_return co_await (aloe::core::ex::just(incremented) | aloe::core::ex::then([](int v) { return v * 2; }));
}

int main() {
    const auto result = aloe::core::ex::sync_wait(twice_plus_one(1));  // std::optional<std::tuple<int>>
    return std::get<0>(*result) == 4 ? 0 : 1;
}
```

## Design notes

Only `common/core/execution/execution.hpp` spells `stdexec::` or `exec::`; the rest of the code base writes
`aloe::core::ex::` and `aloe::core::task`. When libstdc++ ships `std::execution`, that header changes and nothing else
does. The alias is a namespace alias rather than a set of wrappers, so there is no forwarding layer between
Aloe code and the sender algorithms, and every stdexec facility is available the day it is needed.

`task` is the standard-track P3552 task, not stdexec's older `exec::task`. The older one stores a type-erased
scheduler and reschedules through it after every await; the P3552 task takes its scheduler type from its
environment, so a runtime with a concrete scheduler pays nothing for the abstraction. The spot that
differs between stdexec and the paper, the environment's member name, is confined to `TaskEnvironment`.

The version header is a CMake template (`version/version.hpp.in`) instead of hand-written constants. The
version therefore has one source, the `project()` call, and a release cannot ship with a stale number.
