# Execution Module

The execution module (`common/execution/`) is the one header that names the execution facilities every
asynchronous part of the stack is written in. It depends on stdexec alone. Everything is reached through the
umbrella `#include <aloe/execution>` (`export/aloe/execution`), and targets link `Aloe::Common::Execution`.
Only the runtime, and programs that wait, link it: a brick never does.

## Key types

- **`aloe::execution::ex`** -- namespace alias for the senders-and-receivers facilities:
  `aloe::execution::ex::just`, `aloe::execution::ex::then`, `aloe::execution::ex::sync_wait`, schedulers, stop
  tokens. It names stdexec today.
- **`aloe::execution::task<T, Env>`** -- the C++26 coroutine task (P3552). Inside one, `co_await` accepts any
  sender. `Env` fixes the scheduler type, stop source, errors and allocator; the default erases the
  scheduler, and the runtime binds its concrete one.
- **`aloe::execution::TaskEnvironment<Scheduler, StopSource>`** -- an environment for `task` that names a
  concrete scheduler. The one place that spells the member stdexec and P3552 name differently.
- **`aloe::execution::completion_behavior`, `aloe::execution::get_completion_behavior_t<Tag>`** -- how a sender
  says where it completes. A scheduler whose `schedule()` sender answers `asynchronous_affine` lets tasks await
  without rescheduling.

## Usage

```cpp
#include <aloe/execution>

aloe::execution::task<int> add_one(int value) {
    co_return value + 1;
}

aloe::execution::task<int> twice_plus_one(int value) {
    namespace ex = aloe::execution::ex;
    const int incremented = co_await add_one(value);
    co_return co_await (ex::just(incremented) | ex::then([](int v) { return v * 2; }));
}

int main() {
    const auto result = aloe::execution::ex::sync_wait(twice_plus_one(1));  // std::optional<std::tuple<int>>
    return std::get<0>(*result) == 4 ? 0 : 1;
}
```

## Design notes

Only `common/execution/execution/execution.hpp` spells `stdexec::` or `exec::`; the rest of the code base
writes `aloe::execution::ex::` and `aloe::execution::task`. When libstdc++ ships `std::execution`, that header
changes and nothing else does. The alias is a namespace alias rather than a set of wrappers, so there is no
forwarding layer between Aloe code and the sender algorithms, and every stdexec facility is available the day
it is needed.

The namespace is `aloe::execution`, not `aloe::exec`: inside `aloe`, a nested `exec` would hide stdexec's own
`::exec` from every unqualified spelling.

`task` is the standard-track P3552 task, not stdexec's older `exec::task`. The older one stores a type-erased
scheduler and reschedules through it after every await; the P3552 task takes its scheduler type from its
environment, so a runtime with a concrete scheduler pays nothing for the abstraction. The spot that
differs between stdexec and the paper, the environment's member name, is confined to `TaskEnvironment`.

The module is separate from [`core`](core.md) so that linking core, which every module does, never brings
stdexec with it.
