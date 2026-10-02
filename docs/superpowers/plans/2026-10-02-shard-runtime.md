# Shard Runtime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The shards that run everything above the device layer: one shard per core over one device queue, a run loop the stack owns, a concrete per-shard scheduler with timers, a counting scope, a shard-bound coroutine task, stop and drain, counters and logging, proven by an Ethernet echo on several shards on both backends.

**Architecture:** A device-free `ShardContext` owns the scheduler's state: an intrusive run queue, a multi-producer inbox, a hierarchical timer wheel, a single-threaded counting scope and the counters, with one verb, `run_once(now)`. A thin `Shard<Device, Stack>` adds one device queue and the tick: receive, hand the burst to the stack, `run_once`, flush. `Runtime<Device, Stack>` launches one shard per queue on its own thread. The scheduler is one pointer to a context, so tasks and senders are one concrete type for every backend, and stdexec's P3552 task never reschedules around them.

**Tech Stack:** C++26 on libstdc++ with clang 22 and GCC 16; stdexec (P3552 `task`, P3149 `counting_scope` as the model); quill 13 for logging behind a core alias; DPDK through `Aloe::Dpdk` only inside `ethdev`; GoogleTest; CMake presets with vcpkg.

**Spec:** `docs/superpowers/specs/2026-10-02-shard-runtime-design.md`. Read it first; this plan argues from it. The device layer it builds on is described in `docs/architecture/device.md`, and the stack's design in `docs/architecture/overview.md`.

## Global Constraints

Copied from the spec and from `AGENTS.md`; every task's requirements include these.

- C++26, clang 22 for every preset but `gcc-debug`, which is GCC 16; both on libstdc++. Warnings are errors, clang-tidy warnings are errors inside the clang builds, and every `.cpp`/`.hpp` passes `./scripts/check-format.sh`.
- Dependencies come from `vcpkg.json` only. This plan adds exactly one: `quill`.
- Only `common/core/execution/execution.hpp` names `stdexec::` or `exec::`. Only `common/core/log/log.hpp` names `quill::`. Everything else writes `aloe::core::ex::`, `aloe::core::task`, `aloe::core::log`, and the other `aloe::core` names those two headers export.
- Hot-path headers never include `<aloe/core>`. The runtime module is the public surface and may; `device`, `fabric`, `ethdev` and `utils` may not.
- Every module has a namespace named after it. New here: `aloe::runtime`. Another module's names are qualified (`device::IsDevice`, `core::Logger`), never pulled in with `using namespace`. Test helpers live in `aloe::testing`.
- Header basenames are unique across the repository. New basenames in this plan: `work.hpp`, `run_queue.hpp`, `inbox.hpp`, `timer_wheel.hpp`, `task_scope.hpp`, `shard_context.hpp`, `scheduler.hpp`, `shard_task.hpp`, `shard_queue.hpp`, `shard.hpp`, `runtime.hpp`, `counters.hpp`, `log.hpp`, `fixed_string.hpp`, `echo_stack.hpp`, `logging_environment.hpp`.
- Public headers define no macros. Implementation details go in a nested `detail` namespace.
- `codestyle.md` applies: PascalCase types, snake_case functions, trailing underscore members, `[[nodiscard]]` on getters and factories, `explicit` single-argument constructors, `{}` initialisation, by-value parameters `const` in definitions but not in bodiless declarations, spans the function writes through unqualified.
- Structs initialised with designated initialisers give every member a default member initialiser: clang's `-Wmissing-designated-field-initializers` is an error here.
- Protocol-defined numbers with named values are `enum class` with the wire type as the underlying type, never raw integers.
- `std::expected` on hot paths, exceptions on setup and cold paths. The runtime's hot path, `step`, `run_once`, `Work::run`, `Timer::fire`, `transmit`, `allocate`, never throws.
- Tests need no root, no hugepages and no network card. Tests that need them get the `manual` label.
- No tests for trivial helpers. A test pins behaviour that could regress or that a reviewer could dispute.
- Conventional commits with the module as scope: `feat(runtime): ...`, `feat(core): ...`, `feat(ethdev): ...`, `test(runtime): ...`, `docs: ...`, `build: ...`.
- No C++ modules; `CMAKE_CXX_SCAN_FOR_MODULES` stays `OFF`.
- Every command in this plan runs from the repository root unless the step says otherwise.
- Work on the branch the author created for issue #4 (`git branch --show-current` shows it); never commit to `main`.

## Verified Beforehand

Nothing in this plan was built end to end. The author asked for the plan without a prototype. Three claims from the spec's "Claims the prototype must prove" were spot-checked on 2026-10-02 with two single-file probes compiled against the headers in `build/release/vcpkg_installed/x64-linux-clang/include`, with the project's warning set (`-std=c++26 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`), on clang 22 and GCC 16. The probes were throwaway and are not in the repository.

| Claim | Result |
|---|---|
| `stdexec::task<T, Env>` accepts an environment whose `start_scheduler_type` is a hand-written single-threaded scheduler, picks that scheduler up from the receiver's environment through `get_start_scheduler`, and never wraps awaited senders in `affine` when the scheduler's own `schedule()` sender advertises `exec::completion_behavior::asynchronous_affine` for `set_value`. | Proven. A parent task awaiting one `schedule()`, one timer, a child task that awaits a timer, and `ex::just(1)` produced exactly 1 run-queue push and 2 timer arms. The child inherited the scheduler. |
| A stop requested on the scope's stop source while the task is parked on a timer completes the task stopped. | Proven, with a timer sender that checks the token when it fires. The real timer sender adds a stop callback that cancels the timer early; the pattern is standard and Task 8 tests it. |
| `ex::task<int>` with the default environment still runs under `ex::sync_wait`. | Proven. The existing tests in `test_execution.cpp` keep passing once `core::task` aliases `stdexec::task`. |
| A macro-free quill wrapper with the format string as a `FixedString` template parameter, one `static constexpr quill::MacroMetadata` per instantiation, calling `quill::Logger::log_statement<false>`. | Proven. Both compilers accept it; both lines came out on the console with the logger name and level. |
| quill 13.0.0, installed by the reconfigure at review time on 2026-10-02, has what Task 2 calls: `Logger::log_statement<bool, Args...>(MacroMetadata const*, Args&&...)`, `LoggerBase::should_log_statement<LogLevel>()`, `LoggerBase::set_log_level`, the six-argument `constexpr MacroMetadata` constructor with `Event::Log`, `BackendOptions::cpu_affinity` as `std::vector<uint16_t>`, `FileSinkConfig::set_open_mode(char)`, `Backend::start`, `stop` and `is_running`, `Frontend::create_or_get_sink<TSink>` and `create_or_get_logger(name, sink)`. | Checked against the headers, not compiled. `Logger::flush_log` spins until the backend acknowledges, so `core::Logger::flush` with no `Logging` alive never returns. |

What the probes established about the exact mechanism, which the spec left to the prototype:

- The task's `await_transform` looks at `env_of_t<schedule_result_t<start_scheduler_type>>`, the attributes of the scheduler's own `schedule()` sender. If `exec::get_completion_behavior_t<set_value_t>` on those attributes answers `exec::completion_behavior::asynchronous_affine`, every awaited sender is passed to `as_awaitable` directly. So the attribute goes on the `schedule()` sender, and no customisation of `affine` is needed.
- The receiver environment that starts a task must answer `ex::get_start_scheduler` (stdexec's P3552 spelling) with a value the environment's `start_scheduler_type` is constructible from. Answering `ex::get_scheduler` too is harmless and keeps `ex::read_env(ex::get_scheduler)` working inside the task.
- The deprecated `stdexec::get_completion_behavior` spelling emits a deprecation warning, which `-Werror` turns into an error. Use `exec::get_completion_behavior_t` and `exec::completion_behavior` from `<exec/completion_behavior.hpp>`, aliased in core.

Not checked, left to the tasks that own them: `rte_thread_register` on a `std::jthread` after an EAL started with `-l 0` (Task 13), the echo over `net_ring` with four queues (Task 13), and the whole suite under `asan` and `tsan` (every task, and Task 16).

## Read This First

For an executor who has the repository, the spec and this plan, and nothing else.

**Baseline.** From a clean checkout the first configure clones vcpkg and builds DPDK, about five minutes:

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Configure, build and test presets share names: `debug`, `gcc-debug`, `asan`, `tsan`, `release`. The lint check is a clean clang build: clang-tidy runs inside it with warnings as errors. `./scripts/check-format.sh` checks formatting; the pre-commit hook runs it on staged files, so a badly formatted commit does not go through. Run `clang-format -i` on files you touch before committing.

**A change is done** when `debug`, `gcc-debug`, `asan` and `tsan` all build and pass, and the format check passes. Build the other presets at the end of each task at least; the thread tests of Tasks 4 and 12 exist for `tsan` and must be run under it in their own task.

**Traps already found** (from `AGENTS.md`, plus what this plan's probes added):

- clang-tidy's `readability-static-accessed-through-instance` is disabled because it fires on every `co_await`; do not re-enable it.
- clang-tidy rejects `std::move` on a sender whose type is trivially copyable. Pass the sender expression directly.
- `EXPECT_THROW(std::ignore = T{.a = 1, .b = 2}, E)` needs the expression in an extra pair of parentheses.
- One process starts DPDK's EAL once. A test binary that uses DPDK registers one `aloe::testing::EalEnvironment` and probes fresh virtual devices with `aloe::testing::probe_vdev`. A closed port cannot be reopened.
- The ring driver (`net_ring`) has no RSS and sets no MTU; what a queue transmits comes back on the same queue. The null driver (`net_null`) generates a frame per receive slot by itself, so it is useless for an echo.
- GCC needs masks on the mbuf `l2_len`/`l3_len` bit-field writes; not touched here.
- Debug executables that link DPDK are about 200 MB.
- `stdexec::get_completion_behavior` is deprecated and fails under `-Werror`; the `exec::` spelling in `<exec/completion_behavior.hpp>` is the one to alias.
- `std::atomic` members make a type immovable. `Work` has one, so every operation state that derives from it is immovable, which is what stdexec requires of operation states anyway. Containers of such types use `std::unique_ptr` or `std::deque` with `emplace_back` and no reallocation moves.
- clang-tidy's `cppcoreguidelines-pro-type-member-init` wants every member initialised; give members default member initialisers. `performance-unnecessary-value-param` fires on by-value parameters of non-trivial types that are only read; take those by `const&`.
- clang-tidy's `readability-make-member-function-const` fires on a member that only writes through a pointer member, such as `ShardQueue::allocate` and `receive`, `Runtime::shard` and `spawn`, or a test wrapper that forwards to a port. Call `utils::force_non_const(this)` first, as `ethdev::Packet` does; never make such a function `const` to please the check.
- clang-tidy's `readability-convert-member-functions-to-static` fires on a `query` member that never touches `this`, such as the completion-behaviour and forward-progress answers. Make those `static`; stdexec calls `env.query(tag)`, which a static member satisfies.
- With `ShardEnvironment` the task awaits every sender directly, with no `affine` wrap, including one that completes on another thread. A shard task awaits only its own shard's senders and child tasks (spec, threading contract rule 4); the scope's owner assertion catches a task that ends elsewhere.
- PR CI (`.github/workflows/pr-testing.yaml`) builds the `ci` preset only: Release, clang, examples off, asserts out. `gcc-debug`, `asan`, `tsan` and the example build are local gates; run them before the PR, CI will not.
- `core::Logger::flush` spins forever when no `Logging` is alive: quill's `flush_log` waits for the backend to acknowledge. Flush only in code that owns or runs under a `Logging`.

**Where things are.** Module layout and target names are in `AGENTS.md`. Test registration helpers are `add_unit_test`, `add_integration_test` and `add_manual_test` from `cmake/tests.cmake`, with the target name variables `${UNIT_TESTING_TARGET}`, `${INTEGRATION_TESTING_TARGET}`, `${MANUAL_TESTING_TARGET}`, `${SHARED_TESTING_TARGET}` and the gtest libraries in `${TEST_LIBS}`, all set in `tests/CMakeLists.txt`. Frame builders are in `tests/shared/device/frames.hpp`: `ethernet_frame(destination, source, ethertype, payload)`, `ipv4_frame(Ipv4Spec, payload)`, `flow_of(Ipv4Spec)`, `fill(packet, frame)`, `bytes_of(packet)`, `pattern(length)`, and `ethertype_experimental`. Steering is predicted with `aloe::device::queue_for(steering, flow_tuple)` in `component/device/steering/rss.hpp`.

## How to Execute and Review

Use `superpowers:subagent-driven-development`. For each task, in order:

1. An implementer subagent gets the task text, the spec and `AGENTS.md`, and implements it test-first exactly as the steps say, committing at the end of the task.
2. A spec-compliance reviewer checks the diff against the task's "Interfaces" block and the spec sections the task names. It rejects renamed or missing names, behaviour the spec forbids, and scope beyond the task.
3. A code-quality reviewer checks the list below. Both reviewers get the diff and the spec, not the chat.
4. Fix findings, re-run the task's tests, and move on only when both reviewers approve.

After the last task, one whole-branch review against the spec's "Done when" list and the Review Focus below, then the verification in Task 16.

What the reviewers check for this repository, beyond correctness:

- Namespaces: everything new in `aloe::runtime`, `aloe::core`, `aloe::utils`, `aloe::ethdev` or `aloe::testing`; other modules' names qualified; nothing declared directly in `aloe`.
- `stdexec::`, `exec::` only in `execution.hpp`; `quill::` only in `log.hpp`. `grep -rn 'stdexec::\|exec::\|quill::' common component tests examples --include=*.hpp --include=*.cpp` lists only those two files.
- `<aloe/core>` not included by any header under `component/device`, `component/fabric`, `component/ethdev` or `common/utils`.
- By-value parameters `const` in definitions, not in bodiless declarations; written-through spans unqualified.
- `[[nodiscard]]` on getters, factories and anything whose result is a bug to ignore; `explicit` on single-argument constructors; default member initialisers on every struct member.
- No macros in public headers. No `using namespace`.
- Hot-path functions `noexcept` and allocation-free except for packets, as the spec's "Error handling" lists.
- Tests pin behaviour; no test exists only to show a forwarding helper runs.
- Documentation updated in the same change where the task says so.
- Commit subjects are conventional with the module scope.

## Review Focus

Conditions the spec implies but does not spell out, most likely to bite first. Each is pinned by a test in the task that owns the code.

1. **Work pushed from inside running work.** A task that reschedules itself, or a timer that re-arms itself in its own callback, must run on the next step, never spin the current step forever. Pinned in Task 3 (run queue) and Task 5 (wheel: a timer re-armed from its own `fire` goes to the due list for the next `advance`).
2. **A stop requested before the operation runs.** `schedule()` from the main thread, then `Runtime::stop()` before the shard gets to it: the operation completes stopped, the receiver is still invoked, and nothing leaks. Pinned in Task 8.
3. **Cancellation racing with firing on the same thread.** A timer's stop callback runs while the wheel is firing another timer in the same slot; the cancelled node is unlinked from a list being walked. Pinned in Task 5 (cancel from inside another timer's `fire`) and Task 8.
4. **The device refuses transmission.** The transmit ring fills, the device accepts nothing, and `transmit` must return false without losing the caller's packet or the ring's. Pinned in Task 10 with a fabric port whose peer's receive queue is full.
5. **Stop while tasks are parked.** `Runtime::stop()` with tasks waiting on timers: every task completes stopped, every shard drains, `join` returns. A task never waits in another shard's inbox (threading contract rule 4); the scope's owner assertion in Task 6 catches one that does. Pinned in Task 11.

---

## File Structure

New files, by responsibility. Paths are exact.

```
vcpkg.json                                           + quill
common/utils/fixed_string/fixed_string.hpp           aloe::utils::FixedString<N>
common/utils/export/aloe/utils                       + fixed_string.hpp
common/utils/CMakeLists.txt                          + Utils.FixedString
common/core/execution/execution.hpp                  task -> stdexec::task, TaskEnvironment, completion_behavior aliases
common/core/log/log.hpp                              quill alias: LogLevel, Logging, Logger, logger(), log<>() and helpers
common/core/export/aloe/core                         + log.hpp
common/core/CMakeLists.txt                           + Core.Log linking quill::quill and Aloe::Common::Utils
component/CMakeLists.txt                             + add_subdirectory(runtime)
component/runtime/CMakeLists.txt                     Aloe.Component.Runtime and its parts
component/runtime/export/aloe/runtime                umbrella
component/runtime/work/work.hpp                      Work
component/runtime/work/run_queue.hpp                 RunQueue
component/runtime/work/inbox.hpp                     Inbox
component/runtime/timer/timer_wheel.hpp              Timer, TimerWheel
component/runtime/timer/timer_wheel.cpp
component/runtime/counters/counters.hpp              ShardCounters
component/runtime/scope/task_scope.hpp               TaskScope
component/runtime/scope/task_scope.cpp
component/runtime/context/shard_context.hpp          ShardContextConfig, ShardContext
component/runtime/context/shard_context.cpp
component/runtime/scheduler/scheduler.hpp            Scheduler and its senders
component/runtime/task/shard_task.hpp                ShardEnvironment, task<T>
component/runtime/shard/shard_queue.hpp              ShardQueue<Device>
component/runtime/shard/shard.hpp                    IdlePolicy, ShardConfig, IsStack, Shard<Device, Stack>
component/runtime/runtime/runtime.hpp                ShardThread, RuntimeConfig, RuntimeError, Runtime<Device, Stack>
component/ethdev/eal/eal.hpp, eal.cpp                + register_thread()
tests/shared/log/logging_environment.hpp             aloe::testing::LoggingEnvironment
tests/shared/runtime/echo_stack.hpp                  aloe::testing::EchoStack<Device>, stamp_of()
tests/shared/CMakeLists.txt                          + Shared.Log, Shared.Runtime
tests/unit_tests/common/core/test_execution.cpp      + environment tests
tests/unit_tests/common/core/test_log.cpp            logging through a file sink
tests/unit_tests/common/CMakeLists.txt               + Core.Log target
tests/unit_tests/component/runtime/CMakeLists.txt    Runtime and Runtime.Threads targets
tests/unit_tests/component/runtime/test_run_queue.cpp
tests/unit_tests/component/runtime/test_inbox.cpp
tests/unit_tests/component/runtime/test_inbox_threads.cpp
tests/unit_tests/component/runtime/test_timer_wheel.cpp
tests/unit_tests/component/runtime/test_task_scope.cpp
tests/unit_tests/component/runtime/test_shard_context.cpp
tests/unit_tests/component/runtime/test_scheduler.cpp
tests/unit_tests/component/runtime/test_shard_task.cpp
tests/unit_tests/component/runtime/test_shard.cpp
tests/unit_tests/component/runtime/test_runtime.cpp
tests/unit_tests/component/runtime/test_cross_shard.cpp
tests/unit_tests/component/runtime/test_steering.cpp
tests/unit_tests/component/CMakeLists.txt            + add_subdirectory(runtime)
tests/integration_tests/runtime/CMakeLists.txt       Runtime.Ring target
tests/integration_tests/runtime/test_runtime_ring.cpp
tests/integration_tests/CMakeLists.txt               + add_subdirectory(runtime)
tests/manual_tests/runtime/CMakeLists.txt            Runtime.Tap target
tests/manual_tests/runtime/test_runtime_tap.cpp
tests/manual_tests/CMakeLists.txt                    + add_subdirectory(runtime)
examples/ethernet_echo/CMakeLists.txt
examples/ethernet_echo/ethernet_echo.cpp
examples/CMakeLists.txt                              + add_subdirectory(ethernet_echo)
docs/architecture/runtime.md                         new page
docs/architecture/overview.md, core.md, utils.md     edits
docs/roadmap.md, README.md, AGENTS.md                edits
```

Task order and what each hands on:

| Task | Delivers | Needed by |
|---|---|---|
| 1 | `core::task` on `stdexec::task`, `TaskEnvironment`, completion-behaviour aliases | 8, 9 |
| 2 | `utils::FixedString`, `core::Logging`, `core::Logger`, `core::logger`, `core::log` and helpers, `LoggingEnvironment` | 6, 7, 11 |
| 3 | runtime module skeleton, `Work`, `RunQueue` | all later |
| 4 | `Inbox` and the `Runtime.Threads` test target | 7, 8, 12 |
| 5 | `Timer`, `TimerWheel` | 7, 8 |
| 6 | `ShardCounters`, `TaskScope` | 7, 8 |
| 7 | `ShardContext` | 8 |
| 8 | `Scheduler`, `schedule`, `schedule_after`, `schedule_at`, `spawn` | 9, 10 |
| 9 | `ShardEnvironment`, `runtime::task<T>` | 10, 11, 12 |
| 10 | `ShardQueue`, `Shard`, `IsStack`, `EchoStack` | 11 |
| 11 | `Runtime` | 12, 13, 14 |
| 12 | cross-shard and steering tests under TSan | 16 |
| 13 | `ethdev::register_thread`, the ring integration test | 14 |
| 14 | the tap manual test, `examples/ethernet_echo` | 15 |
| 15 | documentation | 16 |
| 16 | whole-branch verification | done |

---

### Task 1: Core task on P3552, task environment, completion-behaviour aliases

Spec: "Core additions", "The shard task", "Verified facts".

**Files:**
- Modify: `common/core/execution/execution.hpp`
- Modify: `tests/unit_tests/common/core/test_execution.cpp`
- Modify: `docs/architecture/core.md` (the Key types list and the Design notes)

**Interfaces:**
- Consumes: nothing new.
- Produces:
  - `aloe::core::task<T, Env = aloe::core::ex::env<>>`, now `::stdexec::task<T, Env>`.
  - `aloe::core::TaskEnvironment<Scheduler, StopSource = aloe::core::ex::inplace_stop_source>`: a struct with `using start_scheduler_type = Scheduler; using stop_source_type = StopSource;`.
  - `aloe::core::completion_behavior`, a type with static members `asynchronous_affine`, `asynchronous`, `inline_completion`, `unknown`.
  - `aloe::core::get_completion_behavior_t<Tag>`, the query type a sender's attributes answer.

- [ ] **Step 1: Add the failing test**

Append to `tests/unit_tests/common/core/test_execution.cpp`, inside the anonymous namespace a receiver, and after the existing tests one test:

```cpp
    /// Stands in for a scope's spawn receiver: its environment names the scheduler a task starts on.
    struct LoopReceiver {
        using receiver_concept = aloe::core::ex::receiver_t;

        struct Env {
            aloe::core::ex::run_loop* loop;

            [[nodiscard]] auto query(aloe::core::ex::get_scheduler_t) const noexcept {
                return loop->get_scheduler();
            }

            [[nodiscard]] auto query(aloe::core::ex::get_start_scheduler_t) const noexcept {
                return loop->get_scheduler();
            }
        };

        aloe::core::ex::run_loop* loop;
        std::optional<bool>* result;

        void set_value(const bool same) noexcept {
            *result = same;
            loop->finish();
        }

        void set_error(std::exception_ptr) noexcept {
            loop->finish();
        }

        void set_stopped() noexcept {
            loop->finish();
        }

        [[nodiscard]] Env get_env() const noexcept {
            return Env{loop};
        }
    };
```

```cpp
TEST(Execution, TaskEnvironmentBindsAConcreteScheduler) {
    aloe::core::ex::run_loop loop;
    using Scheduler = decltype(loop.get_scheduler());
    using Env       = aloe::core::TaskEnvironment<Scheduler>;
    static_assert(std::same_as<aloe::core::task<bool, Env>::start_scheduler_type, Scheduler>);

    const auto body = [](Scheduler expected) -> aloe::core::task<bool, Env> {
        const auto here = co_await aloe::core::ex::read_env(aloe::core::ex::get_scheduler);
        co_return here == expected;
    };

    std::optional<bool> result;
    auto operation = aloe::core::ex::connect(body(loop.get_scheduler()), LoopReceiver{&loop, &result});
    aloe::core::ex::start(operation);
    loop.run();

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(*result) << "the task's scheduler is the one its receiver's environment named";
}
```

Add `#include <concepts>`, `#include <exception>` and `#include <optional>` to the file's includes.

- [ ] **Step 2: Run the test to see it fail**

```bash
cmake --build --preset debug --target Aloe.Tests.Unit.Core.Execution 2>&1 | tail -20
```

Expected: compile error, `TaskEnvironment` is not a member of `aloe::core`.

- [ ] **Step 3: Rewrite `execution.hpp`**

Replace the file's contents with:

```cpp
#pragma once

#include <exec/completion_behavior.hpp>
#include <stdexec/execution.hpp>

namespace aloe::core {

    /**
     * @brief The single point where Aloe names its execution facilities.
     *
     * Aloe code spells senders, receivers and schedulers as `aloe::core::ex::...` and
     * coroutine tasks as `aloe::core::task<T>`, never as `stdexec::` or `exec::` directly.
     * The facilities come from stdexec today. Once the standard library ships
     * std::execution, only this header changes.
     */
    namespace ex = ::stdexec;

    /**
     * @brief Coroutine task type, the one P3552 adds to C++26. Inside one, `co_await` accepts any sender.
     *
     * `Env` fixes the task's scheduler type, stop source, error types and allocator. With the
     * default environment the scheduler is type-erased; a runtime binds its concrete scheduler
     * through a TaskEnvironment.
     */
    template <typename T, typename Env = ex::env<>>
    using task = ::stdexec::task<T, Env>;

    /**
     * @brief A task environment naming a concrete scheduler.
     *
     * P3552 calls the member `scheduler_type`; stdexec spells it `start_scheduler_type` today.
     * That spelling lives here and nowhere else, like the namespace alias above.
     */
    template <typename Scheduler, typename StopSource = ex::inplace_stop_source>
    struct TaskEnvironment {
        using start_scheduler_type = Scheduler;
        using stop_source_type     = StopSource;
    };

    /**
     * @brief How a sender completes relative to where it started.
     *
     * A sender whose attributes answer `get_completion_behavior_t<set_value_t>` with
     * `completion_behavior::asynchronous_affine` promises to complete on the scheduler it was
     * started from. A task started on such a scheduler awaits every sender directly, with no
     * reschedule.
     */
    using completion_behavior = ::exec::completion_behavior;

    template <typename Tag>
    using get_completion_behavior_t = ::exec::get_completion_behavior_t<Tag>;

}  // namespace aloe::core
```

- [ ] **Step 4: Build and run the core tests**

```bash
cmake --build --preset debug --target Aloe.Tests.Unit.Core.Execution && ctest --preset debug -R Core
```

Expected: every `Execution` test passes, the old four and the new one. If `ErrorSurfacesAsAnException` or `StoppedTaskYieldsNoValue` fail, the P3552 task's error or stop plumbing differs from `exec::task`; read the failure, fix the test's expectation only if the new behaviour is what P3552 specifies, and record the difference in the spec's "Verified facts".

- [ ] **Step 5: Update `docs/architecture/core.md`**

In "Key types", replace the `aloe::core::task<T>` bullet with:

```markdown
- **`aloe::core::task<T, Env>`** -- the C++26 coroutine task (P3552). Inside one, `co_await` accepts any
  sender. `Env` fixes the scheduler type, stop source, errors and allocator; the default erases the
  scheduler, and the runtime binds its concrete one.
- **`aloe::core::TaskEnvironment<Scheduler, StopSource>`** -- an environment for `task` that names a concrete
  scheduler. The one place that spells the member stdexec and P3552 name differently.
- **`aloe::core::completion_behavior`, `aloe::core::get_completion_behavior_t<Tag>`** -- how a sender says where
  it completes. A scheduler whose `schedule()` sender answers `asynchronous_affine` lets tasks await
  without rescheduling.
```

In "Design notes", after the first paragraph add:

```markdown
`task` is the standard-track P3552 task, not stdexec's older `exec::task`. The older one stores a type-erased
scheduler and reschedules through it after every await; the P3552 task takes its scheduler type from its
environment, so a runtime with a concrete scheduler pays nothing for the abstraction. The spot that
differs between stdexec and the paper, the environment's member name, is confined to `TaskEnvironment`.
```

- [ ] **Step 6: Format, build the GCC preset, commit**

```bash
clang-format -i common/core/execution/execution.hpp tests/unit_tests/common/core/test_execution.cpp
./scripts/check-format.sh
cmake --build --preset gcc-debug --target Aloe.Tests.Unit.Core.Execution && ctest --preset gcc-debug -R Core
git add common/core/execution/execution.hpp tests/unit_tests/common/core/test_execution.cpp docs/architecture/core.md
git commit -m "feat(core): alias task to the P3552 task and add TaskEnvironment"
```

(`cmake --preset gcc-debug` first if that build directory does not exist yet.)

---

### Task 2: FixedString, logging through quill, and the test logging environment

Spec: "Core additions", "Logging in the runtime", decision "Logging".

**Files:**
- Modify: `vcpkg.json`
- Modify: `CMakeLists.txt` (root; one `find_package`)
- Create: `common/utils/fixed_string/fixed_string.hpp`
- Modify: `common/utils/export/aloe/utils`, `common/utils/CMakeLists.txt`
- Modify: `common/CMakeLists.txt` (utils before core)
- Create: `common/core/log/log.hpp`
- Modify: `common/core/export/aloe/core`, `common/core/CMakeLists.txt`
- Create: `tests/shared/log/logging_environment.hpp`
- Modify: `tests/shared/CMakeLists.txt`
- Create: `tests/unit_tests/common/core/test_log.cpp`
- Modify: `tests/unit_tests/common/CMakeLists.txt`
- Modify: `docs/architecture/core.md`, `docs/architecture/utils.md`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `aloe::utils::FixedString<N>`: structural type with `char value[N]`, constructible from a string literal, `data()`, `size()`, `view()`.
  - `aloe::core::LogLevel { Trace, Debug, Info, Warning, Error, Critical, None }`.
  - `aloe::core::LoggingConfig { LogLevel level = Info; std::optional<std::uint16_t> backend_cpu; std::optional<std::filesystem::path> file; }`.
  - `aloe::core::Logging`: `explicit Logging(const LoggingConfig& = {})`, immovable, destructor stops the backend. One per process.
  - `aloe::core::Logger`: `log<Level, Format>(args...)`, `trace<Format>(args...)`, `debug<Format>`, `info<Format>`, `warning<Format>`, `error<Format>`, `critical<Format>`, `set_level(LogLevel)`, `flush()`. Copyable handle.
  - `aloe::core::logger(const std::string& name) -> Logger`.
  - `aloe::testing::LoggingEnvironment`: a gtest environment that owns a `Logging` at `Warning`.
  - CMake: `quill::quill` found at the root; `Aloe::Common::Core` links it and `Aloe::Common::Utils`.

- [ ] **Step 1: Add quill to the manifest and find it**

`vcpkg.json` already lists `"quill"` after `"gtest"`, added at review time on 2026-10-02, and the `debug` preset was reconfigured once with it. In the root `CMakeLists.txt`, after the line `find_package(stdexec CONFIG REQUIRED)` add:

```cmake
find_package(quill CONFIG REQUIRED)
```

In `common/CMakeLists.txt` put `add_subdirectory(utils)` before `add_subdirectory(core)`.

- [ ] **Step 2: Reconfigure**

```bash
cmake --preset debug 2>&1 | tail -5
```

Expected: quill is already installed (the manifest change is committed as `build: add quill to the vcpkg manifest`) and configure succeeds. Commit the CMake wiring on its own:

```bash
git add CMakeLists.txt common/CMakeLists.txt
git commit -m "build(core): find quill and configure utils before core"
```

- [ ] **Step 3: Write the failing log test**

`tests/unit_tests/common/core/test_log.cpp`:

```cpp
#include <aloe/core>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>
#include <unistd.h>

namespace {

    std::string read_all(const std::filesystem::path& path) {
        std::ifstream in{path};
        return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    }

    std::filesystem::path unique_log_path() {
        return std::filesystem::temp_directory_path() / ("aloe-log-" + std::to_string(::getpid()) + ".txt");
    }

}  // namespace

// One Logging per process, so this binary holds exactly one test that constructs it.
TEST(Log, LinesReachTheSinkWithLevelLoggerNameAndArguments) {
    const auto path = unique_log_path();
    {
        aloe::core::Logging logging{{.level = aloe::core::LogLevel::Debug, .file = path}};
        auto log = aloe::core::logger("aloe.test");
        log.info<"shard {} starting on cpu {}">(3, 7U);
        log.trace<"filtered out at Debug">();
        log.flush();
    }
    const std::string text = read_all(path);
    std::filesystem::remove(path);

    EXPECT_NE(text.find("LOG_INFO"), std::string::npos) << text;
    EXPECT_NE(text.find("aloe.test"), std::string::npos) << text;
    EXPECT_NE(text.find("shard 3 starting on cpu 7"), std::string::npos) << text;
    EXPECT_EQ(text.find("filtered out"), std::string::npos) << text;
}
```

Register it in `tests/unit_tests/common/CMakeLists.txt`, after the `Core.Execution` block:

```cmake
##############################################################################
# Test Core: logging through the quill alias, into a file sink
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Core.Log
        core/test_log.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Core.Log
        PRIVATE
        Aloe::Common::Core
        ${TEST_LIBS}
)
##############################################################################
```

- [ ] **Step 4: Run it to see it fail**

```bash
cmake --preset debug > /dev/null && cmake --build --preset debug --target Aloe.Tests.Unit.Core.Log 2>&1 | tail -5
```

Expected: compile error, `Logging` is not a member of `aloe::core`.

- [ ] **Step 5: `FixedString` in utils**

`common/utils/fixed_string/fixed_string.hpp`:

```cpp
#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace aloe::utils {

    /**
     * @brief A string literal usable as a template parameter.
     *
     *     template <FixedString Text> void greet();
     *     greet<"hello">();
     *
     * A structural type: every member is public and the array is compared by value, so two
     * instantiations with the same text are the same specialisation. The conversion from a literal
     * is implicit on purpose, so a call site writes the text and nothing else.
     */
    template <std::size_t N>
    struct FixedString {
        char value[N]{};

        // NOLINTNEXTLINE(google-explicit-constructor): the whole point is `f<"text">()`.
        constexpr explicit(false) FixedString(const char (&text)[N]) noexcept {
            std::copy_n(text, N, value);
        }

        [[nodiscard]] constexpr const char* data() const noexcept {
            return value;
        }

        /// Characters before the terminating zero.
        [[nodiscard]] constexpr std::size_t size() const noexcept {
            return N - 1;
        }

        [[nodiscard]] constexpr std::string_view view() const noexcept {
            return {value, N - 1};
        }
    };

}  // namespace aloe::utils
```

Add `#include <fixed_string.hpp>` to `common/utils/export/aloe/utils` (alphabetically, before `guards.hpp`). In `common/utils/CMakeLists.txt` add before the "Utils exported library" block:

```cmake
##############################################################################
# FixedString: a string literal as a template parameter
##############################################################################
add_library(${UTILS}.FixedString INTERFACE
        fixed_string/fixed_string.hpp
)
target_include_directories(${UTILS}.FixedString INTERFACE
        fixed_string/
)
##############################################################################
```

and `${UTILS}.FixedString` to the `LIBRARIES` of `add_combined_library`.

- [ ] **Step 6: The quill alias, `log.hpp`**

`common/core/log/log.hpp`:

```cpp
#pragma once

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/Logger.h>
#include <quill/core/MacroMetadata.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/FileSink.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <fixed_string.hpp>

namespace aloe::core {

    /// Severity, lowest first. `None` on a logger silences it.
    enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warning, Error, Critical, None };

    namespace detail {

        [[nodiscard]] constexpr quill::LogLevel to_quill(const LogLevel level) noexcept {
            switch (level) {
                case LogLevel::Trace:
                    return quill::LogLevel::TraceL1;
                case LogLevel::Debug:
                    return quill::LogLevel::Debug;
                case LogLevel::Info:
                    return quill::LogLevel::Info;
                case LogLevel::Warning:
                    return quill::LogLevel::Warning;
                case LogLevel::Error:
                    return quill::LogLevel::Error;
                case LogLevel::Critical:
                    return quill::LogLevel::Critical;
                case LogLevel::None:
                    return quill::LogLevel::None;
            }
            return quill::LogLevel::None;
        }

        /// The sink `logger()` hands to new loggers: set by Logging, the console until then.
        [[nodiscard]] inline std::shared_ptr<quill::Sink>& default_sink() {
            static std::shared_ptr<quill::Sink> sink;
            return sink;
        }

        /// The level `logger()` gives new loggers.
        [[nodiscard]] inline LogLevel& default_level() noexcept {
            static LogLevel level = LogLevel::Info;
            return level;
        }

        [[nodiscard]] inline std::shared_ptr<quill::Sink> console_sink() {
            return quill::Frontend::create_or_get_sink<quill::ConsoleSink>("aloe-console");
        }

    }  // namespace detail

    /**
     * @brief A named logger: a copyable handle, cheap to pass by value.
     *
     * Every call takes its format string as a template parameter, so each call site owns one
     * static metadata record the way quill's macros would, and no macro is needed. Formatting
     * happens on the backend thread; the caller only copies the arguments into a per-thread queue.
     * Arguments are what quill accepts: integers, floating point, `std::string`, `std::string_view`,
     * C strings, and the standard containers quill has codecs for.
     */
    class Logger {
    public:
        explicit Logger(quill::Logger* logger) noexcept
            : logger_{logger} {
        }

        template <LogLevel Level, utils::FixedString Format, typename... Args>
        void log(Args&&... args) {
            static constexpr quill::MacroMetadata metadata{
                "", "", Format.data(), nullptr, detail::to_quill(Level), quill::MacroMetadata::Event::Log};
            if (logger_->template should_log_statement<detail::to_quill(Level)>()) {
                logger_->template log_statement<false>(&metadata, std::forward<Args>(args)...);
            }
        }

        template <utils::FixedString Format, typename... Args>
        void trace(Args&&... args) {
            log<LogLevel::Trace, Format>(std::forward<Args>(args)...);
        }

        template <utils::FixedString Format, typename... Args>
        void debug(Args&&... args) {
            log<LogLevel::Debug, Format>(std::forward<Args>(args)...);
        }

        template <utils::FixedString Format, typename... Args>
        void info(Args&&... args) {
            log<LogLevel::Info, Format>(std::forward<Args>(args)...);
        }

        template <utils::FixedString Format, typename... Args>
        void warning(Args&&... args) {
            log<LogLevel::Warning, Format>(std::forward<Args>(args)...);
        }

        template <utils::FixedString Format, typename... Args>
        void error(Args&&... args) {
            log<LogLevel::Error, Format>(std::forward<Args>(args)...);
        }

        template <utils::FixedString Format, typename... Args>
        void critical(Args&&... args) {
            log<LogLevel::Critical, Format>(std::forward<Args>(args)...);
        }

        void set_level(const LogLevel level) noexcept {
            logger_->set_log_level(detail::to_quill(level));
        }

        /// Blocks until the backend has written everything this logger queued. Cold path only, and only while a
        /// `Logging` is alive: quill spins until the backend acknowledges, so with no backend this never returns.
        void flush() {
            logger_->flush_log();
        }

    private:
        quill::Logger* logger_;
    };

    struct LoggingConfig {
        LogLevel level                           = LogLevel::Info;
        std::optional<std::uint16_t> backend_cpu = std::nullopt;  ///< Pin the backend thread here; never a shard's core.
        std::optional<std::filesystem::path> file = std::nullopt;  ///< Write here, truncating, instead of the console.
    };

    /**
     * @brief Owns the logging backend for the process. Construct one, once, before the first log line.
     *
     * Starts quill's backend thread with the sink and level the config asks for; the destructor
     * flushes and stops it. Loggers created before it exists write to the console at Info. Tests
     * construct one through aloe::testing::LoggingEnvironment.
     */
    class Logging {
    public:
        explicit Logging(const LoggingConfig& config = {}) {
            quill::BackendOptions options;
            if (config.backend_cpu) {
                options.cpu_affinity = {*config.backend_cpu};
            }
            if (!quill::Backend::is_running()) {
                quill::Backend::start(options);
            }
            if (config.file) {
                quill::FileSinkConfig file_config;
                file_config.set_open_mode('w');
                detail::default_sink() = quill::Frontend::create_or_get_sink<quill::FileSink>(
                    config.file->string(), file_config, quill::FileEventNotifier{});
            } else {
                detail::default_sink() = detail::console_sink();
            }
            detail::default_level() = config.level;
        }

        Logging(const Logging&)            = delete;
        Logging& operator=(const Logging&) = delete;
        Logging(Logging&&)                 = delete;
        Logging& operator=(Logging&&)      = delete;

        ~Logging() {
            quill::Backend::stop();
            detail::default_sink().reset();
            detail::default_level() = LogLevel::Info;
        }
    };

    /**
     * @brief The logger called `name`, created on first use with the process's default sink and level.
     *
     * quill keeps loggers by name: a second call with the same name returns the first logger, with
     * the sink it was created with. Module code names its logger after the module, `"aloe.runtime"`.
     */
    [[nodiscard]] inline Logger logger(const std::string& name) {
        std::shared_ptr<quill::Sink> sink = detail::default_sink();
        if (!sink) {
            sink = detail::console_sink();
        }
        Logger result{quill::Frontend::create_or_get_logger(name, std::move(sink))};
        result.set_level(detail::default_level());
        return result;
    }

}  // namespace aloe::core
```

Add `#include <log.hpp>` to `common/core/export/aloe/core` (after `execution.hpp`). In `common/core/CMakeLists.txt`, before the "Core exported library" block:

```cmake
##############################################################################
# Logging: the one place that names quill
##############################################################################
add_library(${CORE}.Log INTERFACE
        log/log.hpp
)
target_include_directories(${CORE}.Log INTERFACE
        log/
)
target_link_libraries(${CORE}.Log INTERFACE
        quill::quill
        Aloe::Common::Utils
        Threads::Threads
)
##############################################################################
```

and `${CORE}.Log` to the `LIBRARIES` of the combined library.

- [ ] **Step 7: The test environment**

`tests/shared/log/logging_environment.hpp`:

```cpp
#pragma once

#include <aloe/core>
#include <optional>
#include <utility>

#include <gtest/gtest.h>

namespace aloe::testing {

    /**
     * @brief Starts logging once for a test binary, at Warning so a passing suite stays quiet.
     *
     * Register it from a static initializer in one test file of the binary:
     *
     *     const auto* const logging = ::testing::AddGlobalTestEnvironment(new aloe::testing::LoggingEnvironment{});
     */
    class LoggingEnvironment : public ::testing::Environment {
    public:
        explicit LoggingEnvironment(core::LoggingConfig config = {.level = core::LogLevel::Warning})
            : config_{std::move(config)} {
        }

        void SetUp() override {
            logging_.emplace(config_);
        }

        void TearDown() override {
            logging_.reset();
        }

    private:
        core::LoggingConfig config_;
        std::optional<core::Logging> logging_;
    };

}  // namespace aloe::testing
```

In `tests/shared/CMakeLists.txt` add:

```cmake
##############################################################################
# Logging as a gtest environment
##############################################################################
add_library(${SHARED_TESTING_TARGET}.Log INTERFACE
        log/logging_environment.hpp
)
target_include_directories(${SHARED_TESTING_TARGET}.Log INTERFACE
        log/
)
target_link_libraries(${SHARED_TESTING_TARGET}.Log INTERFACE
        Aloe::Common::Core
        ${TEST_LIBS}
)
##############################################################################
```

- [ ] **Step 8: Build and run**

```bash
cmake --preset debug > /dev/null && cmake --build --preset debug --target Aloe.Tests.Unit.Core.Log Aloe.Tests.Unit.Utils && ctest --preset debug -R 'Core.Log|Utils'
```

Expected: pass. If `should_log_statement<...>()` does not compile as a template, quill's version differs from 13.0.0; use the runtime overload `should_log_statement(detail::to_quill(Level))` and note it in the spec's "Verified facts".

- [ ] **Step 9: Documentation**

`docs/architecture/core.md`: in the first paragraph change "It depends on stdexec and on nothing else in Aloe" to "It depends on stdexec and quill, and on [`utils`](utils.md) for `FixedString`." Add to "Key types":

```markdown
- **`aloe::core::Logging`, `aloe::core::LoggingConfig`** -- the process's logging backend: construct one, once,
  with the level, an optional file instead of the console, and an optional CPU to pin the backend thread to.
- **`aloe::core::Logger`, `aloe::core::logger(name)`** -- a named logger. `log.info<"shard {} starting">(index)`:
  the format string is a template parameter, so there are no macros and each call site owns its metadata.
  Formatting happens on the backend thread.
- **`aloe::core::LogLevel`** -- `Trace` to `Critical`, and `None`.
```

Add to "Design notes":

```markdown
Logging is quill behind the same kind of alias as stdexec: `common/core/log/log.hpp` is the only file that
names `quill::`. The wrapper is macro-free, which the "public headers define no macros" rule demands, and keeps
quill's shape: a lock-free per-thread queue on the calling side, one backend thread that formats and writes.
That backend thread must not land on a shard's core, which is what `LoggingConfig::backend_cpu` is for. Nothing
in the stack logs on a hot path; the convention is enforced by review, not by the type system.
```

`docs/architecture/utils.md`: add to "Key types":

```markdown
- **`aloe::utils::FixedString<N>`** -- a string literal as a template parameter: `template <FixedString Text>`.
  Structural, so equal texts are one specialisation. Core's logger takes its format strings this way.
```

and update the first paragraph's list to mention it ("..., a string literal as a template parameter, and two guards...").

- [ ] **Step 10: Format, all presets, commit**

```bash
clang-format -i common/utils/fixed_string/fixed_string.hpp common/core/log/log.hpp tests/shared/log/logging_environment.hpp tests/unit_tests/common/core/test_log.cpp
./scripts/check-format.sh
cmake --preset gcc-debug > /dev/null && cmake --build --preset gcc-debug && ctest --preset gcc-debug
cmake --preset asan > /dev/null && cmake --build --preset asan && ctest --preset asan
git add common/utils common/core tests/shared tests/unit_tests/common docs/architecture/core.md docs/architecture/utils.md
git commit -m "feat(core): logging through quill behind a macro-free alias, FixedString in utils"
```

The `asan` run matters here: the backend thread's shutdown in `~Logging` is where a leak or a use-after-stop would show.

---

### Task 3: Runtime module skeleton, `Work` and `RunQueue`

Spec: "Module layout", "Work and the run queue".

**Files:**
- Create: `component/runtime/CMakeLists.txt`, `component/runtime/export/aloe/runtime`
- Create: `component/runtime/work/work.hpp`, `component/runtime/work/run_queue.hpp`
- Modify: `component/CMakeLists.txt`
- Create: `tests/unit_tests/component/runtime/CMakeLists.txt`, `tests/unit_tests/component/runtime/test_run_queue.cpp`
- Modify: `tests/unit_tests/component/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `aloe::runtime::Work { using Function = void (*)(Work&) noexcept; std::atomic<Work*> next{nullptr}; Function run = nullptr; }`, default-constructible and constructible from a `Function`. Immovable, because of the atomic.
  - `aloe::runtime::RunQueue`: `void push(Work&) noexcept`, `Work* take() noexcept` (detaches the whole chain), `bool empty() const noexcept`, `static std::size_t run_chain(Work*) noexcept` (runs a detached chain, returns how many ran).
  - Target `Aloe::Component::Runtime`, umbrella `<aloe/runtime>`, unit test target `Aloe.Tests.Unit.Runtime`.

- [ ] **Step 1: Module skeleton**

`component/runtime/CMakeLists.txt`:

```cmake
set(RUNTIME ${COMPONENT}.Runtime)

##############################################################################
# Work: the intrusive node and the same-shard run queue
##############################################################################
add_library(${RUNTIME}.Work INTERFACE
        work/work.hpp
        work/run_queue.hpp
)
target_include_directories(${RUNTIME}.Work INTERFACE
        work/
)
##############################################################################

##############################################################################
# Runtime exported library
##############################################################################
add_combined_library(${RUNTIME}
        DIRECTORIES
        export
        SOURCES
        export/aloe/runtime
        LIBRARIES
        ${RUNTIME}.Work
)

add_library(Aloe::Component::Runtime ALIAS ${RUNTIME})
##############################################################################
```

`component/runtime/export/aloe/runtime`:

```cpp
#pragma once

/**
 * Public umbrella for the Aloe runtime module: shards, their scheduler, timers,
 * scope and task, and the runtime that launches them. Consumers link
 * Aloe::Component::Runtime and write `#include <aloe/runtime>`.
 */

#include <run_queue.hpp>
#include <work.hpp>
```

Add `add_subdirectory(runtime)` at the end of `component/CMakeLists.txt`. Later tasks extend the CMake file with one block per part and the umbrella with one include per header, in alphabetical order.

- [ ] **Step 2: The failing test**

`tests/unit_tests/component/runtime/test_run_queue.cpp`:

```cpp
#include <aloe/runtime>
#include <vector>

#include <gtest/gtest.h>

namespace {

    /// Records its id when run, and re-pushes itself once if asked to.
    struct Recorder : aloe::runtime::Work {
        Recorder(std::vector<int>& record, const int id)
            : Work{&Recorder::execute}
            , record_{&record}
            , id_{id} {
        }

        void requeue_once_into(aloe::runtime::RunQueue& queue) noexcept {
            requeue_ = &queue;
        }

        static void execute(aloe::runtime::Work& work) noexcept {
            auto& self = static_cast<Recorder&>(work);
            self.record_->push_back(self.id_);
            if (self.requeue_ != nullptr) {
                aloe::runtime::RunQueue* queue = self.requeue_;
                self.requeue_                  = nullptr;
                queue->push(self);
            }
        }

    private:
        std::vector<int>* record_;
        int id_;
        aloe::runtime::RunQueue* requeue_ = nullptr;
    };

}  // namespace

TEST(RunQueue, RunsInPushOrderAndTakeEmptiesIt) {
    aloe::runtime::RunQueue queue;
    std::vector<int> record;
    Recorder first{record, 1};
    Recorder second{record, 2};
    Recorder third{record, 3};
    EXPECT_TRUE(queue.empty());

    queue.push(first);
    queue.push(second);
    queue.push(third);
    EXPECT_FALSE(queue.empty());

    aloe::runtime::Work* chain = queue.take();
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(aloe::runtime::RunQueue::run_chain(chain), 3);
    EXPECT_EQ(record, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(queue.take(), nullptr);
}

// Review Focus 1: work that re-pushes itself runs on the next step, not in the same chain.
TEST(RunQueue, WorkPushedWhileAChainRunsWaitsForTheNextTake) {
    aloe::runtime::RunQueue queue;
    std::vector<int> record;
    Recorder self_pusher{record, 1};
    Recorder other{record, 2};
    self_pusher.requeue_once_into(queue);

    queue.push(self_pusher);
    queue.push(other);
    EXPECT_EQ(aloe::runtime::RunQueue::run_chain(queue.take()), 2);
    EXPECT_EQ(record, (std::vector<int>{1, 2})) << "the re-push did not run in the same chain";
    EXPECT_FALSE(queue.empty()) << "the re-pushed node waits in the queue";

    EXPECT_EQ(aloe::runtime::RunQueue::run_chain(queue.take()), 1);
    EXPECT_EQ(record, (std::vector<int>{1, 2, 1}));
    EXPECT_TRUE(queue.empty());
}
```

`tests/unit_tests/component/runtime/CMakeLists.txt`:

```cmake
##############################################################################
# Test Runtime: the context's primitives, the scheduler, the shard and the runtime on the fabric
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Runtime
        test_run_queue.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Runtime
        PRIVATE
        Aloe::Component::Runtime
        ${TEST_LIBS}
)
##############################################################################
```

Add `add_subdirectory(runtime)` to `tests/unit_tests/component/CMakeLists.txt`.

- [ ] **Step 3: See it fail**

```bash
cmake --preset debug > /dev/null && cmake --build --preset debug --target Aloe.Tests.Unit.Runtime 2>&1 | tail -5
```

Expected: `work.hpp` not found.

- [ ] **Step 4: `work.hpp`**

```cpp
#pragma once

#include <atomic>

namespace aloe::runtime {

    /**
     * @brief The intrusive node every unit of ready work is.
     *
     * An operation state derives from it and sets `run`. One node type serves both queues of a
     * shard: the inbox uses acquire and release on `next`, the run queue relaxed operations, which
     * are plain loads and stores on every supported target, so the same-shard path pays no fence.
     * `run` is called once per push, on the owning shard's thread, and may destroy or re-push the
     * node; whoever runs a chain therefore reads `next` before calling it.
     */
    struct Work {
        using Function = void (*)(Work&) noexcept;

        Work() noexcept = default;

        constexpr explicit Work(const Function function) noexcept
            : run{function} {
        }

        Work(const Work&)            = delete;
        Work& operator=(const Work&) = delete;
        Work(Work&&)                 = delete;
        Work& operator=(Work&&)      = delete;
        ~Work()                      = default;

        std::atomic<Work*> next{nullptr};
        Function run = nullptr;
    };

}  // namespace aloe::runtime
```

- [ ] **Step 5: `run_queue.hpp`**

```cpp
#pragma once

#include <atomic>
#include <cstddef>

#include <work.hpp>

namespace aloe::runtime {

    /**
     * @brief Same-shard FIFO of Work. One thread, no synchronisation.
     *
     * `take` detaches the whole chain and leaves the queue empty, so work pushed while a chain runs
     * waits for the next `take`. That bounds one step of the shard loop.
     */
    class RunQueue {
    public:
        void push(Work& work) noexcept {
            work.next.store(nullptr, std::memory_order_relaxed);
            if (tail_ == nullptr) {
                head_ = &work;
            } else {
                tail_->next.store(&work, std::memory_order_relaxed);
            }
            tail_ = &work;
        }

        /// Everything queued, as a chain linked through `next`; the queue is empty afterwards.
        [[nodiscard]] Work* take() noexcept {
            Work* chain = head_;
            head_       = nullptr;
            tail_       = nullptr;
            return chain;
        }

        [[nodiscard]] bool empty() const noexcept {
            return head_ == nullptr;
        }

        /// Runs a detached chain in order. `next` is read before `run`, which may destroy or re-push the node.
        static std::size_t run_chain(Work* chain) noexcept {
            std::size_t count = 0;
            while (chain != nullptr) {
                Work* next = chain->next.load(std::memory_order_relaxed);
                chain->run(*chain);
                chain = next;
                ++count;
            }
            return count;
        }

    private:
        Work* head_ = nullptr;
        Work* tail_ = nullptr;
    };

}  // namespace aloe::runtime
```

- [ ] **Step 6: Build, test, commit**

```bash
cmake --build --preset debug --target Aloe.Tests.Unit.Runtime && ctest --preset debug -R Runtime
clang-format -i component/runtime/work/*.hpp tests/unit_tests/component/runtime/test_run_queue.cpp
./scripts/check-format.sh
git add component/CMakeLists.txt component/runtime tests/unit_tests/component/CMakeLists.txt tests/unit_tests/component/runtime
git commit -m "feat(runtime): module skeleton with the Work node and the run queue"
```

---

### Task 4: `Inbox`, and the thread-stress test target

Spec: "Inbox", "Testing: Threads".

**Files:**
- Create: `component/runtime/work/inbox.hpp`
- Modify: `component/runtime/CMakeLists.txt` (add `work/inbox.hpp` to `${RUNTIME}.Work`), umbrella
- Create: `tests/unit_tests/component/runtime/test_inbox.cpp`, `tests/unit_tests/component/runtime/test_inbox_threads.cpp`
- Modify: `tests/unit_tests/component/runtime/CMakeLists.txt`

**Interfaces:**
- Consumes: `Work`.
- Produces: `aloe::runtime::Inbox`: `void push(Work&) noexcept` from any thread; `Work* pop() noexcept` and `bool empty() const noexcept` from the consumer thread only. `pop` may return `nullptr` while a producer is mid-push even though a node is on its way; the node is seen on a later `pop`. `empty()` is false in that state. Test target `Aloe.Tests.Unit.Runtime.Threads`.

- [ ] **Step 1: Failing tests**

`tests/unit_tests/component/runtime/test_inbox.cpp`:

```cpp
#include <aloe/runtime>
#include <vector>

#include <gtest/gtest.h>

namespace {

    struct Numbered : aloe::runtime::Work {
        explicit Numbered(const int id)
            : id_{id} {
        }

        [[nodiscard]] int id() const noexcept {
            return id_;
        }

    private:
        int id_;
    };

    std::vector<int> drain(aloe::runtime::Inbox& inbox) {
        std::vector<int> ids;
        for (aloe::runtime::Work* work = inbox.pop(); work != nullptr; work = inbox.pop()) {
            ids.push_back(static_cast<Numbered&>(*work).id());
        }
        return ids;
    }

}  // namespace

TEST(Inbox, PopsInPushOrderAndReportsEmpty) {
    aloe::runtime::Inbox inbox;
    EXPECT_TRUE(inbox.empty());
    EXPECT_EQ(inbox.pop(), nullptr);

    Numbered a{1};
    Numbered b{2};
    Numbered c{3};
    inbox.push(a);
    EXPECT_FALSE(inbox.empty());
    inbox.push(b);
    inbox.push(c);

    EXPECT_EQ(drain(inbox), (std::vector<int>{1, 2, 3}));
    EXPECT_TRUE(inbox.empty());
    EXPECT_EQ(inbox.pop(), nullptr);
}

TEST(Inbox, InterleavedPushAndPopKeepsOrder) {
    aloe::runtime::Inbox inbox;
    Numbered a{1};
    Numbered b{2};
    Numbered c{3};
    inbox.push(a);
    inbox.push(b);
    EXPECT_EQ(static_cast<Numbered*>(inbox.pop())->id(), 1);
    inbox.push(c);
    EXPECT_EQ(static_cast<Numbered*>(inbox.pop())->id(), 2);
    EXPECT_FALSE(inbox.empty());
    EXPECT_EQ(static_cast<Numbered*>(inbox.pop())->id(), 3);
    EXPECT_TRUE(inbox.empty());

    // A node may be pushed again once it has been popped.
    inbox.push(a);
    EXPECT_EQ(static_cast<Numbered*>(inbox.pop())->id(), 1);
    EXPECT_TRUE(inbox.empty());
}
```

`tests/unit_tests/component/runtime/test_inbox_threads.cpp`:

```cpp
#include <aloe/runtime>
#include <cstddef>
#include <deque>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

    constexpr std::size_t producers          = 6;
    constexpr std::size_t nodes_per_producer = 2000;

    struct Message : aloe::runtime::Work {
        Message(const std::size_t producer, const std::size_t sequence)
            : producer_{producer}
            , sequence_{sequence} {
        }

        std::size_t producer_;
        std::size_t sequence_;
    };

}  // namespace

// The test the tsan preset exists for: every producer pushes from its own thread while the
// consumer pops, and every node arrives exactly once, in its producer's order.
TEST(InboxThreads, NodesFromManyThreadsArriveExactlyOnceInProducerOrder) {
    aloe::runtime::Inbox inbox;
    std::vector<std::deque<Message>> storage(producers);  // deque: Message is immovable
    for (std::size_t producer = 0; producer < producers; ++producer) {
        for (std::size_t sequence = 0; sequence < nodes_per_producer; ++sequence) {
            storage[producer].emplace_back(producer, sequence);
        }
    }

    std::set<std::pair<std::size_t, std::size_t>> seen;
    std::vector<std::size_t> next_expected(producers, 0);
    std::size_t received = 0;

    std::jthread consumer{[&] {
        while (received < producers * nodes_per_producer) {
            aloe::runtime::Work* work = inbox.pop();
            if (work == nullptr) {
                std::this_thread::yield();
                continue;
            }
            auto& message = static_cast<Message&>(*work);
            EXPECT_TRUE(seen.emplace(message.producer_, message.sequence_).second) << "a node arrived twice";
            EXPECT_EQ(message.sequence_, next_expected[message.producer_]) << "out of order within a producer";
            next_expected[message.producer_] = message.sequence_ + 1;
            ++received;
        }
    }};

    {
        std::vector<std::jthread> threads;
        threads.reserve(producers);
        for (std::size_t producer = 0; producer < producers; ++producer) {
            threads.emplace_back([&inbox, &storage, producer] {
                for (Message& message : storage[producer]) {
                    inbox.push(message);
                }
            });
        }
    }
    consumer.join();

    EXPECT_EQ(received, producers * nodes_per_producer);
    EXPECT_TRUE(inbox.empty());
}
```

In `tests/unit_tests/component/runtime/CMakeLists.txt` add `test_inbox.cpp` to the `Runtime` target and a second block:

```cmake
##############################################################################
# Test Runtime: cross-thread paths, for the tsan preset
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Runtime.Threads
        test_inbox_threads.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Runtime.Threads
        PRIVATE
        Aloe::Component::Runtime
        Threads::Threads
        ${TEST_LIBS}
)
##############################################################################
```

- [ ] **Step 2: See them fail**

```bash
cmake --preset debug > /dev/null && cmake --build --preset debug --target Aloe.Tests.Unit.Runtime 2>&1 | tail -5
```

Expected: `Inbox` is not a member of `aloe::runtime`.

- [ ] **Step 3: `inbox.hpp`**

```cpp
#pragma once

#include <atomic>

#include <work.hpp>

namespace aloe::runtime {

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
        Inbox() noexcept = default;
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

}  // namespace aloe::runtime
```

Add `work/inbox.hpp` to `${RUNTIME}.Work` in the CMake file and `#include <inbox.hpp>` to the umbrella.

- [ ] **Step 4: Build and run under debug and tsan**

```bash
cmake --build --preset debug --target Aloe.Tests.Unit.Runtime Aloe.Tests.Unit.Runtime.Threads && ctest --preset debug -R Runtime
cmake --preset tsan > /dev/null && cmake --build --preset tsan --target Aloe.Tests.Unit.Runtime.Threads && ctest --preset tsan -R Runtime.Threads
```

Expected: both pass; the TSan run reports no races. A TSan report here means a memory order is wrong; the algorithm above is the published one, so compare against it before changing orders.

- [ ] **Step 5: Format and commit**

```bash
clang-format -i component/runtime/work/inbox.hpp tests/unit_tests/component/runtime/test_inbox.cpp tests/unit_tests/component/runtime/test_inbox_threads.cpp
./scripts/check-format.sh
git add component/runtime tests/unit_tests/component/runtime
git commit -m "feat(runtime): multi-producer single-consumer inbox"
```

---

### Task 5: `Timer` and `TimerWheel`

Spec: "Timer wheel". Review Focus 1 and 3.

**Files:**
- Create: `component/runtime/timer/timer_wheel.hpp`, `component/runtime/timer/timer_wheel.cpp`
- Modify: `component/runtime/CMakeLists.txt`, umbrella
- Create: `tests/unit_tests/component/runtime/test_timer_wheel.cpp`
- Modify: `tests/unit_tests/component/runtime/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `aloe::runtime::Timer { using Function = void (*)(Timer&) noexcept; using TimePoint = std::chrono::steady_clock::time_point; Timer* next; Timer* prev; TimePoint deadline; Function fire; bool armed() const; }`, default-constructible or from a `Function`, immovable. Its destructor asserts it is not armed.
  - `aloe::runtime::TimerWheel(Duration resolution, TimePoint start)`: `arm(Timer&, TimePoint)`, `cancel(Timer&)`, `std::size_t advance(TimePoint now)` (fires, returns the count), `pending()`, `resolution()`. Constants `levels = 4`, `slots_per_level = 256`.

**Algorithm**, for the implementer and the reviewer. Ticks count from `start` in units of `resolution`; `current_tick_` is the last tick processed, zero at construction. A timer's tick is its deadline rounded up, so it never fires early. `arm` with a tick at or before `current_tick_` links the timer into the due list, fired first on the next `advance`; otherwise it is placed by its distance `delta = tick - current_tick_`: level 0 for `delta < 256`, level 1 below `65536`, level 2 below `2^24`, level 3 beyond, in slot `(tick >> 8 * level) & 0xff`. `advance(now)` fires the due list, then processes every tick from `current_tick_ + 1` to the tick of `now`: when the tick's low byte is zero it cascades level 1's slot `(tick >> 8) & 0xff` by re-placing its timers, then level 2 if level 1's index was zero too, then level 3; then it fires level 0's slot `tick & 0xff`. A timer cascaded with a tick equal to the tick being processed goes into that level-0 slot and fires in the same tick. Each list is fired by popping from the front, so a timer cancelled from another timer's `fire` is unlinked before it is reached. A deadline more than `2^32` ticks out is placed at the horizon and re-placed at each cascade until its real tick is within reach, so it fires at its deadline. Timers for one tick fire in the order they entered their final slot.

- [ ] **Step 1: Failing tests**

`tests/unit_tests/component/runtime/test_timer_wheel.cpp`:

```cpp
#include <aloe/runtime>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using TimePoint = aloe::runtime::Timer::TimePoint;

    constexpr TimePoint start{};
    constexpr auto resolution = 1ms;

    [[nodiscard]] TimePoint at(const std::chrono::nanoseconds offset) {
        return start + offset;
    }

    struct Fired {
        int id;
        TimePoint now;  ///< The stamp of the `advance` that fired it.
    };

    /// Records itself when fired; can re-arm itself or cancel another timer from inside `fire`.
    struct Probe : aloe::runtime::Timer {
        Probe(std::vector<Fired>& record, const TimePoint* now, const int id)
            : Timer{&Probe::execute}
            , record_{&record}
            , now_{now}
            , id_{id} {
        }

        void rearm_once_from_fire(aloe::runtime::TimerWheel& wheel, const TimePoint deadline) noexcept {
            rearm_wheel_    = &wheel;
            rearm_deadline_ = deadline;
        }

        void cancel_from_fire(aloe::runtime::TimerWheel& wheel, Probe& victim) noexcept {
            cancel_wheel_  = &wheel;
            cancel_victim_ = &victim;
        }

        static void execute(aloe::runtime::Timer& timer) noexcept {
            auto& self = static_cast<Probe&>(timer);
            self.record_->push_back({self.id_, *self.now_});
            if (self.rearm_wheel_ != nullptr) {
                aloe::runtime::TimerWheel* wheel = self.rearm_wheel_;
                self.rearm_wheel_                = nullptr;
                wheel->arm(self, self.rearm_deadline_);
            }
            if (self.cancel_wheel_ != nullptr) {
                self.cancel_wheel_->cancel(*self.cancel_victim_);
                self.cancel_wheel_ = nullptr;
            }
        }

    private:
        std::vector<Fired>* record_;
        const TimePoint* now_;
        int id_;
        aloe::runtime::TimerWheel* rearm_wheel_ = nullptr;
        TimePoint rearm_deadline_{};
        aloe::runtime::TimerWheel* cancel_wheel_ = nullptr;
        Probe* cancel_victim_                    = nullptr;
    };

    class TimerWheelTest : public testing::Test {
    protected:
        std::vector<Fired> record_;
        TimePoint now_ = start;
        aloe::runtime::TimerWheel wheel_{resolution, start};

        std::size_t advance(const TimePoint now) {
            now_ = now;
            return wheel_.advance(now);
        }

        [[nodiscard]] std::vector<int> fired_ids() const {
            std::vector<int> ids;
            for (const Fired& fired : record_) {
                ids.push_back(fired.id);
            }
            return ids;
        }

    };

}  // namespace

TEST_F(TimerWheelTest, NeverFiresEarlyAndAtMostOneResolutionLate) {
    Probe exact{record_, &now_, 1};
    Probe between{record_, &now_, 2};
    wheel_.arm(exact, at(5ms));
    wheel_.arm(between, at(5ms + 1us));
    EXPECT_EQ(wheel_.pending(), 2);

    EXPECT_EQ(advance(at(4ms + 999us)), 0) << "before both deadlines";
    EXPECT_EQ(advance(at(5ms)), 1) << "the exact deadline fires on its tick";
    EXPECT_EQ(fired_ids(), (std::vector<int>{1}));
    EXPECT_EQ(advance(at(5ms + 999us)), 0) << "one microsecond past the deadline is still inside tick 5";
    EXPECT_EQ(advance(at(6ms)), 1) << "rounded up to tick 6, one resolution late at most";
    EXPECT_EQ(fired_ids(), (std::vector<int>{1, 2}));
    EXPECT_EQ(wheel_.pending(), 0);
    EXPECT_FALSE(exact.armed());
    EXPECT_FALSE(between.armed());
}

TEST_F(TimerWheelTest, FiresInDeadlineOrderAcrossSlotsAndInArmOrderWithinOne) {
    Probe late{record_, &now_, 30};
    Probe early{record_, &now_, 10};
    Probe middle{record_, &now_, 20};
    Probe same_a{record_, &now_, 1};
    Probe same_b{record_, &now_, 2};
    Probe same_c{record_, &now_, 3};
    wheel_.arm(late, at(30ms));
    wheel_.arm(early, at(10ms));
    wheel_.arm(middle, at(20ms));
    wheel_.arm(same_a, at(20ms));
    wheel_.arm(same_b, at(20ms));
    wheel_.arm(same_c, at(20ms));

    EXPECT_EQ(advance(at(100ms)), 6);
    EXPECT_EQ(fired_ids(), (std::vector<int>{10, 20, 1, 2, 3, 30}));
}

TEST_F(TimerWheelTest, CancelPreventsFiringAndIsIdempotent) {
    Probe kept{record_, &now_, 1};
    Probe cancelled{record_, &now_, 2};
    wheel_.arm(kept, at(10ms));
    wheel_.arm(cancelled, at(10ms));
    wheel_.cancel(cancelled);
    wheel_.cancel(cancelled);
    EXPECT_FALSE(cancelled.armed());
    EXPECT_EQ(wheel_.pending(), 1);

    EXPECT_EQ(advance(at(10ms)), 1);
    EXPECT_EQ(fired_ids(), (std::vector<int>{1}));
}

TEST_F(TimerWheelTest, ReArmingMovesTheDeadline) {
    Probe timer{record_, &now_, 1};
    wheel_.arm(timer, at(10ms));
    wheel_.arm(timer, at(50ms));
    EXPECT_EQ(wheel_.pending(), 1) << "re-arming does not count twice";

    EXPECT_EQ(advance(at(20ms)), 0);
    EXPECT_EQ(advance(at(50ms)), 1);
    EXPECT_EQ(wheel_.pending(), 0);
}

TEST_F(TimerWheelTest, ADeadlineInThePastFiresOnTheNextAdvance) {
    EXPECT_EQ(advance(at(100ms)), 0);
    Probe timer{record_, &now_, 1};
    wheel_.arm(timer, at(50ms));
    EXPECT_TRUE(timer.armed());

    EXPECT_EQ(advance(at(100ms)), 1) << "the same stamp again still fires it";
    EXPECT_EQ(fired_ids(), (std::vector<int>{1}));
}

// Review Focus 1: a timer that re-arms itself in the past from its own fire runs once per advance.
TEST_F(TimerWheelTest, ReArmFromFireWaitsForTheNextAdvance) {
    Probe timer{record_, &now_, 1};
    timer.rearm_once_from_fire(wheel_, at(0ms));
    wheel_.arm(timer, at(10ms));

    EXPECT_EQ(advance(at(10ms)), 1);
    EXPECT_TRUE(timer.armed()) << "re-armed into the due list";
    EXPECT_EQ(advance(at(10ms)), 1);
    EXPECT_FALSE(timer.armed());
    EXPECT_EQ(fired_ids(), (std::vector<int>{1, 1}));
}

// Review Focus 3: cancelling a timer from another's fire, in the same slot, skips it cleanly.
TEST_F(TimerWheelTest, CancelFromAnotherTimersFireInTheSameSlotSkipsIt) {
    Probe first{record_, &now_, 1};
    Probe victim{record_, &now_, 2};
    Probe third{record_, &now_, 3};
    first.cancel_from_fire(wheel_, victim);
    wheel_.arm(first, at(10ms));
    wheel_.arm(victim, at(10ms));
    wheel_.arm(third, at(10ms));

    EXPECT_EQ(advance(at(10ms)), 2);
    EXPECT_EQ(fired_ids(), (std::vector<int>{1, 3}));
    EXPECT_FALSE(victim.armed());
    EXPECT_EQ(wheel_.pending(), 0);
}

TEST_F(TimerWheelTest, DeadlinesFarOutCrossLevelsAndFireOnTime) {
    Probe level1{record_, &now_, 1};
    Probe level2{record_, &now_, 2};
    Probe level3{record_, &now_, 3};
    wheel_.arm(level1, at(300ms));                       // 256 <= delta < 65536
    wheel_.arm(level2, at(70'000ms));                    // 65536 <= delta < 2^24
    wheel_.arm(level3, at(std::chrono::milliseconds{(std::int64_t{1} << 24) + 5}));  // beyond 2^24 ticks

    EXPECT_EQ(advance(at(299ms)), 0);
    EXPECT_EQ(advance(at(300ms)), 1);
    EXPECT_EQ(advance(at(69'999ms)), 0);
    EXPECT_EQ(advance(at(70'000ms)), 1);
    EXPECT_EQ(advance(at(std::chrono::milliseconds{(std::int64_t{1} << 24) + 4})), 0);
    EXPECT_EQ(advance(at(std::chrono::milliseconds{(std::int64_t{1} << 24) + 5})), 1);
    EXPECT_EQ(fired_ids(), (std::vector<int>{1, 2, 3}));
}

TEST_F(TimerWheelTest, RandomisedArmsAndCancelsFireOnTheFirstAdvanceThatReachesThem) {
    std::mt19937 generator{20261002};
    std::uniform_int_distribution<std::int64_t> deadline_ms{0, 200'000};
    std::uniform_int_distribution<int> coin{0, 9};
    constexpr int count = 400;

    std::vector<std::unique_ptr<Probe>> probes;
    std::vector<TimePoint> deadlines;
    std::vector<bool> cancelled(count, false);
    for (int id = 0; id < count; ++id) {
        probes.push_back(std::make_unique<Probe>(record_, &now_, id));
        deadlines.push_back(at(std::chrono::milliseconds{deadline_ms(generator)}));
        wheel_.arm(*probes.back(), deadlines.back());
    }
    for (int id = 0; id < count; ++id) {
        if (coin(generator) == 0) {
            wheel_.cancel(*probes[static_cast<std::size_t>(id)]);
            cancelled[static_cast<std::size_t>(id)] = true;
        }
    }

    std::uniform_int_distribution<std::int64_t> step_us{1, 3'000'000};
    TimePoint previous = start;
    TimePoint now      = start;
    std::vector<TimePoint> previous_stamp;  // the advance before the one that fired each record
    while (now < at(210'000ms)) {
        previous = now;
        now += std::chrono::microseconds{step_us(generator)};
        const std::size_t before = record_.size();
        advance(now);
        for (std::size_t index = before; index < record_.size(); ++index) {
            previous_stamp.push_back(previous);
        }
    }

    std::vector<int> fired = fired_ids();
    std::vector<int> expected;
    for (int id = 0; id < count; ++id) {
        if (!cancelled[static_cast<std::size_t>(id)]) {
            expected.push_back(id);
        }
    }
    std::ranges::sort(fired);
    EXPECT_EQ(fired, expected) << "every armed timer fired once, no cancelled one did";

    for (std::size_t index = 0; index < record_.size(); ++index) {
        const auto id       = static_cast<std::size_t>(record_[index].id);
        const auto deadline = deadlines[id];
        EXPECT_GE(record_[index].now, deadline) << "timer " << id << " fired early";
        EXPECT_LT(previous_stamp[index], deadline + resolution)
            << "timer " << id << " should have fired on the previous advance";
    }
    EXPECT_EQ(wheel_.pending(), 0);
}
```

Add `test_timer_wheel.cpp` to the `Runtime` test target.

- [ ] **Step 2: See it fail**

```bash
cmake --preset debug > /dev/null && cmake --build --preset debug --target Aloe.Tests.Unit.Runtime 2>&1 | tail -5
```

Expected: `TimerWheel` not found.

- [ ] **Step 3: `timer_wheel.hpp`**

```cpp
#pragma once

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace aloe::runtime {

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
        /// Fires everything due at `now` and returns how many fired. An earlier stamp than the last one fires only the due list.
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
        Timer due_;                        ///< Deadlines already reached, fired first on the next advance.
        std::array<std::array<Timer, slots_per_level>, levels> slots_;
    };

}  // namespace aloe::runtime
```

- [ ] **Step 4: `timer_wheel.cpp`**

```cpp
#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include <timer_wheel.hpp>

namespace aloe::runtime {

    namespace {

        constexpr unsigned bits_per_level    = 8;
        constexpr std::uint64_t slot_mask    = TimerWheel::slots_per_level - 1;
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
            timer.prev       = head.prev;
            timer.next       = &head;
            head.prev->next  = &timer;
            head.prev        = &timer;
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
        : resolution_{resolution}
        , start_{start}
        , now_{start} {
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
        std::size_t fired         = fire_list(due_);
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
```

CMake, before the exported-library block:

```cmake
##############################################################################
# Timers: the intrusive node and the hierarchical wheel
##############################################################################
add_library(${RUNTIME}.Timer STATIC
        timer/timer_wheel.hpp
        timer/timer_wheel.cpp
)
target_include_directories(${RUNTIME}.Timer PUBLIC
        timer/
)
##############################################################################
```

Add `${RUNTIME}.Timer` to the combined library's `LIBRARIES` and `#include <timer_wheel.hpp>` to the umbrella.

- [ ] **Step 5: Build, test, commit**

```bash
cmake --build --preset debug --target Aloe.Tests.Unit.Runtime && ctest --preset debug -R Runtime
```

Expected: all pass. `DeadlinesFarOutCrossLevelsAndFireOnTime` loops over about seventeen million ticks and takes a moment; under sanitizers a few seconds. If it exceeds ten seconds under `asan`, drop the level-3 timer from that test and note it: the algorithm is the same at every level.

```bash
clang-format -i component/runtime/timer/* tests/unit_tests/component/runtime/test_timer_wheel.cpp
./scripts/check-format.sh
git add component/runtime tests/unit_tests/component/runtime
git commit -m "feat(runtime): hierarchical timer wheel with intrusive timers"
```

---

### Task 6: `ShardCounters` and `TaskScope`

Spec: "Task scope", "Counters", "Logging in the runtime".

**Files:**
- Create: `component/runtime/counters/counters.hpp`
- Create: `component/runtime/scope/task_scope.hpp`, `component/runtime/scope/task_scope.cpp`
- Modify: `component/runtime/CMakeLists.txt`, umbrella
- Create: `tests/unit_tests/component/runtime/test_task_scope.cpp`
- Modify: `tests/unit_tests/component/runtime/CMakeLists.txt`

**Interfaces:**
- Consumes: `core::Logger`, `core::ex`.
- Produces:
  - `aloe::runtime::ShardCounters`: twelve `std::uint64_t` fields, see spec "Counters", with `operator==`.
  - `aloe::runtime::TaskScope(core::Logger logger, std::uint16_t index, ShardCounters& counters)`:
    - `template <core::ex::sender Sender, typename Env = detail::EmptyEnv> void spawn(Sender&&, Env = {})`. One allocation. The started operation's receiver environment answers `get_stop_token` with the scope's token and forwards every other query to `Env`. A value completion counts `tasks_completed`, a stopped one `tasks_stopped`, an error one `tasks_failed` plus an `Error` log line.
    - `void request_stop() noexcept`, `bool stop_requested() const noexcept`, `core::ex::inplace_stop_token stop_token() const noexcept`, `std::size_t size() const noexcept`, `bool empty() const noexcept`.
    - `JoinSender join() noexcept`: a sender of `set_value()` that completes when the scope is empty; at most one join operation pending at a time.
  - All of it single-threaded; the destructor asserts `empty()`. The scope remembers the thread of its first `spawn` and, in debug builds, asserts that every completion arrives on it: a task that ended on another shard (threading contract rule 4) fails here, not silently.

- [ ] **Step 1: `counters.hpp`**

```cpp
#pragma once

#include <cstdint>

namespace aloe::runtime {

    /// Per-shard counters, all monotonic. Read on the shard's thread, or after the shard has stopped.
    struct ShardCounters {
        std::uint64_t ticks              = 0;
        std::uint64_t idle_ticks         = 0;  ///< Steps in which nothing was received, run, fired or sent.
        std::uint64_t frames_received    = 0;
        std::uint64_t frames_transmitted = 0;  ///< Accepted by the device.
        std::uint64_t transmit_refused   = 0;  ///< `transmit` returned false, or a frame was dropped at drain.
        std::uint64_t inbox_received     = 0;
        std::uint64_t work_run           = 0;
        std::uint64_t timers_fired       = 0;
        std::uint64_t tasks_spawned      = 0;
        std::uint64_t tasks_completed    = 0;
        std::uint64_t tasks_stopped      = 0;
        std::uint64_t tasks_failed       = 0;

        friend constexpr bool operator==(const ShardCounters&, const ShardCounters&) noexcept = default;
    };

}  // namespace aloe::runtime
```

- [ ] **Step 2: Failing tests**

`tests/unit_tests/component/runtime/test_task_scope.cpp`:

```cpp
#include <aloe/runtime>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <logging_environment.hpp>

namespace {

    // One logging environment per test binary; this file owns it for the Runtime target.
    const auto* const logging = ::testing::AddGlobalTestEnvironment(new aloe::testing::LoggingEnvironment{});

    /// A sender the test completes by hand, which records what its receiver's environment said.
    struct ManualSender {
        using sender_concept        = aloe::core::ex::sender_t;
        using completion_signatures = aloe::core::ex::completion_signatures<aloe::core::ex::set_value_t(),
                                                                            aloe::core::ex::set_error_t(std::exception_ptr),
                                                                            aloe::core::ex::set_stopped_t()>;

        struct Handle {
            virtual ~Handle()                      = default;
            virtual void value() noexcept          = 0;
            virtual void stopped() noexcept        = 0;
            virtual void error() noexcept          = 0;
            aloe::core::ex::inplace_stop_token token;
        };

        std::vector<Handle*>* handles;

        template <aloe::core::ex::receiver Receiver>
        struct Operation : Handle {
            Receiver receiver;
            std::vector<Handle*>* handles;

            Operation(Receiver r, std::vector<Handle*>* h)
                : receiver{std::move(r)}
                , handles{h} {
            }

            void start() & noexcept {
                token = aloe::core::ex::get_stop_token(aloe::core::ex::get_env(receiver));
                if (token.stop_requested()) {
                    aloe::core::ex::set_stopped(std::move(receiver));
                    return;
                }
                handles->push_back(this);
            }

            void value() noexcept override {
                aloe::core::ex::set_value(std::move(receiver));
            }

            void stopped() noexcept override {
                aloe::core::ex::set_stopped(std::move(receiver));
            }

            void error() noexcept override {
                aloe::core::ex::set_error(std::move(receiver), std::make_exception_ptr(std::runtime_error{"boom"}));
            }
        };

        template <aloe::core::ex::receiver Receiver>
        auto connect(Receiver receiver) const -> Operation<Receiver> {
            return Operation<Receiver>{std::move(receiver), handles};
        }
    };

    struct FlagReceiver {
        using receiver_concept = aloe::core::ex::receiver_t;
        bool* flag;

        void set_value() noexcept {
            *flag = true;
        }
    };

    /// An environment answering one query of its own, to prove the scope forwards what it does not answer itself.
    struct TagEnv {
        int tag;

        struct TagQuery {
            template <typename Env>
            [[nodiscard]] int operator()(const Env& env) const noexcept {
                return env.query(*this);
            }
        };

        [[nodiscard]] int query(TagQuery) const noexcept {
            return tag;
        }
    };

    class TaskScopeTest : public testing::Test {
    protected:
        aloe::runtime::ShardCounters counters_;
        aloe::runtime::TaskScope scope_{aloe::core::logger("aloe.runtime"), 0, counters_};
        std::vector<ManualSender::Handle*> handles_;

        [[nodiscard]] ManualSender manual() noexcept {
            return ManualSender{&handles_};
        }
    };

}  // namespace

TEST_F(TaskScopeTest, InlineCompletionLeavesTheScopeEmptyAndCounted) {
    EXPECT_TRUE(scope_.empty());
    scope_.spawn(aloe::core::ex::just());
    EXPECT_TRUE(scope_.empty());
    EXPECT_EQ(counters_.tasks_spawned, 1);
    EXPECT_EQ(counters_.tasks_completed, 1);
}

TEST_F(TaskScopeTest, PendingWorkKeepsTheScopeOpenUntilItCompletes) {
    scope_.spawn(manual());
    scope_.spawn(manual());
    ASSERT_EQ(handles_.size(), 2);
    EXPECT_EQ(scope_.size(), 2);

    handles_[0]->value();
    EXPECT_EQ(scope_.size(), 1);
    handles_[1]->stopped();
    EXPECT_TRUE(scope_.empty());
    EXPECT_EQ(counters_.tasks_completed, 1);
    EXPECT_EQ(counters_.tasks_stopped, 1);
}

TEST_F(TaskScopeTest, RequestStopReachesRunningWorkAndWorkSpawnedAfterwards) {
    scope_.spawn(manual());
    ASSERT_EQ(handles_.size(), 1);
    EXPECT_FALSE(handles_[0]->token.stop_requested());

    scope_.request_stop();
    EXPECT_TRUE(scope_.stop_requested());
    EXPECT_TRUE(handles_[0]->token.stop_requested()) << "the token handed out earlier sees the stop";
    handles_[0]->stopped();

    scope_.spawn(manual());
    EXPECT_EQ(handles_.size(), 1) << "started with stop already requested, so it completed at once";
    EXPECT_TRUE(scope_.empty());
    EXPECT_EQ(counters_.tasks_stopped, 2);
}

TEST_F(TaskScopeTest, AFailedTaskIsCountedAndTheScopeContinues) {
    scope_.spawn(manual());
    scope_.spawn(aloe::core::ex::just_error(std::make_exception_ptr(std::runtime_error{"boom"})));
    EXPECT_EQ(counters_.tasks_failed, 1);
    EXPECT_EQ(scope_.size(), 1) << "the other task is unaffected";

    handles_[0]->error();
    EXPECT_EQ(counters_.tasks_failed, 2);
    EXPECT_TRUE(scope_.empty());
}

TEST_F(TaskScopeTest, JoinCompletesWhenTheLastTaskEndsOrAtOnceWhenEmpty) {
    bool joined = false;
    auto immediate = aloe::core::ex::connect(scope_.join(), FlagReceiver{&joined});
    aloe::core::ex::start(immediate);
    EXPECT_TRUE(joined) << "an empty scope joins at once";

    joined = false;
    scope_.spawn(manual());
    scope_.spawn(manual());
    auto waiting = aloe::core::ex::connect(scope_.join(), FlagReceiver{&joined});
    aloe::core::ex::start(waiting);
    EXPECT_FALSE(joined);
    handles_[0]->value();
    EXPECT_FALSE(joined) << "one task still runs";
    handles_[1]->value();
    EXPECT_TRUE(joined);
}

TEST_F(TaskScopeTest, TheEnvironmentPassedToSpawnReachesTheWork) {
    struct Recording {
        using sender_concept        = aloe::core::ex::sender_t;
        using completion_signatures = aloe::core::ex::completion_signatures<aloe::core::ex::set_value_t()>;
        int* seen;

        template <aloe::core::ex::receiver Receiver>
        struct Operation {
            Receiver receiver;
            int* seen;

            void start() & noexcept {
                *seen = TagEnv::TagQuery{}(aloe::core::ex::get_env(receiver));
                aloe::core::ex::set_value(std::move(receiver));
            }
        };

        template <aloe::core::ex::receiver Receiver>
        auto connect(Receiver receiver) const -> Operation<Receiver> {
            return Operation<Receiver>{std::move(receiver), seen};
        }
    };

    int seen = 0;
    scope_.spawn(Recording{&seen}, TagEnv{.tag = 42});
    EXPECT_EQ(seen, 42);
}
```

Add `test_task_scope.cpp` to the `Runtime` target and link `${SHARED_TESTING_TARGET}.Log` to it.

- [ ] **Step 3: See it fail**

```bash
cmake --preset debug > /dev/null && cmake --build --preset debug --target Aloe.Tests.Unit.Runtime 2>&1 | tail -5
```

- [ ] **Step 4: `task_scope.hpp`**

```cpp
#pragma once

#include <aloe/core>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

#include <counters.hpp>

namespace aloe::runtime {

    class TaskScope;

    namespace detail {

        struct EmptyEnv {};

        /// The receiver environment of spawned work: the scope's stop token, then whatever `Env` answers.
        template <typename Env>
        struct ScopeEnv {
            core::ex::inplace_stop_token token;
            Env env;

            [[nodiscard]] core::ex::inplace_stop_token query(core::ex::get_stop_token_t) const noexcept {
                return token;
            }

            template <typename Query>
                requires requires(const Env& inner, const Query query) { inner.query(query); }
            [[nodiscard]] auto query(const Query query) const noexcept {
                return env.query(query);
            }
        };

        enum class Outcome : std::uint8_t { Value, Stopped };

        template <typename Sender, typename Env>
        struct SpawnOperation;

        template <typename Sender, typename Env>
        struct SpawnReceiver {
            using receiver_concept = core::ex::receiver_t;

            SpawnOperation<Sender, Env>* operation;

            template <typename... Values>
            void set_value(Values&&...) && noexcept;
            void set_error(std::exception_ptr error) && noexcept;
            template <typename Error>
            void set_error(Error&&) && noexcept;
            void set_stopped() && noexcept;
            [[nodiscard]] ScopeEnv<Env> get_env() const noexcept;
        };

        /// The pending join, if any: a node the scope completes when it empties.
        struct JoinBase {
            using Function = void (*)(JoinBase&) noexcept;
            Function complete = nullptr;
        };

    }  // namespace detail

    /**
     * @brief The per-shard counting scope: spawn senders, count them, stop them, know when they are done.
     *
     * Shaped like the standard's `counting_scope`, `spawn` and `join`, with no atomics because one
     * thread uses it. `spawn` allocates the operation state, the one allocation per task until the
     * arena arrives, and starts it with an environment that answers `get_stop_token` with the
     * scope's token and everything else from the environment the caller passed. Completions are
     * counted; an error completion is logged at Error and does not take the shard down.
     * `request_stop` trips the stop source, and every later spawn starts already stopped. Debug
     * builds assert that every completion arrives on the thread of the first spawn: a task that
     * strayed to another shard (threading contract rule 4) fails here.
     */
    class TaskScope {
    public:
        class JoinSender;

        TaskScope(core::Logger logger, std::uint16_t index, ShardCounters& counters) noexcept;
        TaskScope(const TaskScope&)            = delete;
        TaskScope& operator=(const TaskScope&) = delete;
        TaskScope(TaskScope&&)                 = delete;
        TaskScope& operator=(TaskScope&&)      = delete;
        ~TaskScope();

        /// Shard thread only. `Env` answers the queries the work asks of its environment, such as `get_scheduler`.
        template <core::ex::sender Sender, typename Env = detail::EmptyEnv>
        void spawn(Sender&& sender, Env env = {});

        void request_stop() noexcept;

        [[nodiscard]] bool stop_requested() const noexcept {
            return stop_source_.stop_requested();
        }

        [[nodiscard]] core::ex::inplace_stop_token stop_token() const noexcept {
            return stop_source_.get_token();
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return live_;
        }

        [[nodiscard]] bool empty() const noexcept {
            return live_ == 0;
        }

        /// Completes with `set_value()` when the scope is empty, at once if it already is. One pending join at a time.
        [[nodiscard]] JoinSender join() noexcept;

    private:
        template <typename Sender, typename Env>
        friend struct detail::SpawnOperation;
        template <typename Receiver>
        friend struct JoinOperation;

        void finished(detail::Outcome outcome) noexcept;
        void failed(std::exception_ptr error) noexcept;
        void one_less() noexcept;
        void wait(detail::JoinBase& join) noexcept;

        core::Logger logger_;
        std::uint16_t index_;
        ShardCounters* counters_;
        core::ex::inplace_stop_source stop_source_;
        std::size_t live_          = 0;
        detail::JoinBase* joiner_  = nullptr;
        std::optional<std::thread::id> owner_;  ///< The thread of the first spawn; every completion must arrive on it.
    };

    namespace detail {

        template <typename Sender, typename Env>
        struct SpawnOperation {
            TaskScope* scope;
            Env env;
            core::ex::connect_result_t<Sender, SpawnReceiver<Sender, Env>> state;

            SpawnOperation(TaskScope* owner, Sender&& sender, Env environment)
                : scope{owner}
                , env{std::move(environment)}
                , state{core::ex::connect(std::move(sender), SpawnReceiver<Sender, Env>{this})} {
            }

            /// Frees the state first, then tells the scope: the scope may complete a join from there.
            void complete(const Outcome outcome) noexcept {
                TaskScope* owner = scope;
                delete this;
                owner->finished(outcome);
            }

            void fail(std::exception_ptr error) noexcept {
                TaskScope* owner = scope;
                delete this;
                owner->failed(std::move(error));
            }
        };

        template <typename Sender, typename Env>
        template <typename... Values>
        void SpawnReceiver<Sender, Env>::set_value(Values&&...) && noexcept {
            operation->complete(Outcome::Value);
        }

        template <typename Sender, typename Env>
        void SpawnReceiver<Sender, Env>::set_error(std::exception_ptr error) && noexcept {
            operation->fail(std::move(error));
        }

        template <typename Sender, typename Env>
        template <typename Error>
        void SpawnReceiver<Sender, Env>::set_error(Error&&) && noexcept {
            operation->fail(nullptr);
        }

        template <typename Sender, typename Env>
        void SpawnReceiver<Sender, Env>::set_stopped() && noexcept {
            operation->complete(Outcome::Stopped);
        }

        template <typename Sender, typename Env>
        ScopeEnv<Env> SpawnReceiver<Sender, Env>::get_env() const noexcept {
            return ScopeEnv<Env>{operation->scope->stop_token(), operation->env};
        }

    }  // namespace detail

    template <typename Receiver>
    struct JoinOperation : detail::JoinBase {
        TaskScope* scope;
        Receiver receiver;

        JoinOperation(TaskScope* owner, Receiver r) noexcept
            : JoinBase{&JoinOperation::finish}
            , scope{owner}
            , receiver{std::move(r)} {
        }

        void start() & noexcept {
            if (scope->empty()) {
                core::ex::set_value(std::move(receiver));
            } else {
                scope->wait(*this);
            }
        }

        static void finish(detail::JoinBase& base) noexcept {
            core::ex::set_value(std::move(static_cast<JoinOperation&>(base).receiver));
        }
    };

    class TaskScope::JoinSender {
    public:
        using sender_concept        = core::ex::sender_t;
        using completion_signatures = core::ex::completion_signatures<core::ex::set_value_t()>;

        explicit JoinSender(TaskScope& scope) noexcept
            : scope_{&scope} {
        }

        template <core::ex::receiver Receiver>
        [[nodiscard]] auto connect(Receiver receiver) const noexcept -> JoinOperation<Receiver> {
            return JoinOperation<Receiver>{scope_, std::move(receiver)};
        }

    private:
        TaskScope* scope_;
    };

    inline TaskScope::JoinSender TaskScope::join() noexcept {
        return JoinSender{*this};
    }

    template <core::ex::sender Sender, typename Env>
    void TaskScope::spawn(Sender&& sender, Env env) {
        using Plain     = std::remove_cvref_t<Sender>;
        using Operation = detail::SpawnOperation<Plain, Env>;
        if (!owner_) {
            owner_ = std::this_thread::get_id();  // the shard thread by contract; the test's own thread in unit tests
        }
        Plain owned{std::forward<Sender>(sender)};
        auto* operation = new Operation{this, std::move(owned), std::move(env)};
        ++live_;
        ++counters_->tasks_spawned;
        core::ex::start(operation->state);
    }

}  // namespace aloe::runtime
```

- [ ] **Step 5: `task_scope.cpp`**

```cpp
#include <cassert>
#include <cstdint>
#include <exception>
#include <string>
#include <thread>
#include <utility>

#include <task_scope.hpp>

namespace aloe::runtime {

    TaskScope::TaskScope(const core::Logger logger, const std::uint16_t index, ShardCounters& counters) noexcept
        : logger_{logger}
        , index_{index}
        , counters_{&counters} {
    }

    TaskScope::~TaskScope() {
        assert(live_ == 0 && "a TaskScope is destroyed with work still running; drain the shard first");
    }

    void TaskScope::request_stop() noexcept {
        stop_source_.request_stop();
    }

    void TaskScope::finished(const detail::Outcome outcome) noexcept {
        if (outcome == detail::Outcome::Value) {
            ++counters_->tasks_completed;
        } else {
            ++counters_->tasks_stopped;
        }
        one_less();
    }

    void TaskScope::failed(std::exception_ptr error) noexcept {
        ++counters_->tasks_failed;
        std::string what = "an error that is not an exception";
        if (error) {
            try {
                std::rethrow_exception(std::move(error));
            } catch (const std::exception& exception) {
                what = exception.what();
            } catch (...) {
                what = "an exception that is not a std::exception";
            }
        }
        logger_.error<"shard {}: a task failed: {}">(index_, what);
        one_less();
    }

    void TaskScope::one_less() noexcept {
        assert(owner_ == std::this_thread::get_id() &&
               "a task completed off its shard: a shard task awaits only its own shard's senders (threading contract 4)");
        --live_;
        if (live_ == 0 && joiner_ != nullptr) {
            detail::JoinBase* join = std::exchange(joiner_, nullptr);
            join->complete(*join);
        }
    }

    void TaskScope::wait(detail::JoinBase& join) noexcept {
        assert(joiner_ == nullptr && "one join at a time");
        joiner_ = &join;
    }

}  // namespace aloe::runtime
```

CMake blocks, before the exported library:

```cmake
##############################################################################
# Counters
##############################################################################
add_library(${RUNTIME}.Counters INTERFACE
        counters/counters.hpp
)
target_include_directories(${RUNTIME}.Counters INTERFACE
        counters/
)
##############################################################################

##############################################################################
# The per-shard counting scope
##############################################################################
add_library(${RUNTIME}.Scope STATIC
        scope/task_scope.hpp
        scope/task_scope.cpp
)
target_include_directories(${RUNTIME}.Scope PUBLIC
        scope/
)
target_link_libraries(${RUNTIME}.Scope PUBLIC
        ${RUNTIME}.Counters
        Aloe::Common::Core
)
##############################################################################
```

Add both to the combined `LIBRARIES`; add `counters.hpp` and `task_scope.hpp` to the umbrella.

- [ ] **Step 6: Build, test, commit**

```bash
cmake --build --preset debug --target Aloe.Tests.Unit.Runtime && ctest --preset debug -R Runtime
```

Expected: pass. The `AFailedTaskIsCountedAndTheScopeContinues` test prints two `LOG_ERROR` lines to the console; that is the logging working, not a failure. If `ScopeEnv`'s forwarding `query` is ambiguous with the `get_stop_token` one for some query, make the forwarding template `requires (!std::same_as<Query, core::ex::get_stop_token_t>)` as well.

```bash
clang-format -i component/runtime/counters/counters.hpp component/runtime/scope/* tests/unit_tests/component/runtime/test_task_scope.cpp
./scripts/check-format.sh
git add component/runtime tests/unit_tests/component/runtime
git commit -m "feat(runtime): shard counters and the single-threaded task scope"
```

---

### Task 7: `ShardContext`

Spec: "The shard context", "run_once", "Threading contract".

**Files:**
- Create: `component/runtime/context/shard_context.hpp`, `component/runtime/context/shard_context.cpp`
- Modify: `component/runtime/CMakeLists.txt`, umbrella
- Create: `tests/unit_tests/component/runtime/test_shard_context.cpp`
- Modify: `tests/unit_tests/component/runtime/CMakeLists.txt`

**Interfaces:**
- Consumes: `RunQueue`, `Inbox`, `TimerWheel`, `TaskScope`, `ShardCounters`, `core::logger`.
- Produces:
  - `aloe::runtime::ShardContextConfig { std::uint16_t index = 0; std::chrono::nanoseconds timer_resolution = 1ms; }`.
  - `aloe::runtime::ShardContext(const ShardContextConfig&, TimePoint start)`, with `using Clock = std::chrono::steady_clock; using TimePoint = Clock::time_point;`:
    - `static ShardContext* current() noexcept`; `class Current` (RAII: `explicit Current(ShardContext&)`, restores the previous on destruction).
    - `bool run_once(TimePoint now) noexcept`: makes itself current for the call; moves the inbox onto the run queue, advances the wheel, runs the chain; returns whether anything ran or fired.
    - `void request_stop() noexcept`, `bool stop_requested() const noexcept`, `bool drained() const noexcept`.
    - `TimePoint now() const noexcept`, `std::uint16_t index() const noexcept`, `core::Logger logger() const noexcept`.
    - `RunQueue& ready()`, `Inbox& inbox()`, `TimerWheel& timers()`, `TaskScope& scope()`, `ShardCounters& counters()`, `const ShardCounters& counters() const`.

- [ ] **Step 1: Failing tests**

`tests/unit_tests/component/runtime/test_shard_context.cpp`:

```cpp
#include <aloe/runtime>
#include <chrono>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using TimePoint = aloe::runtime::ShardContext::TimePoint;

    constexpr TimePoint start{};

    struct Recorder : aloe::runtime::Work {
        Recorder(std::vector<int>& record, const int id)
            : Work{&Recorder::execute}
            , record_{&record}
            , id_{id} {
        }

        /// On run, push a second node onto the current context's run queue.
        void then_push(aloe::runtime::Work& other) noexcept {
            follow_up_ = &other;
        }

        static void execute(aloe::runtime::Work& work) noexcept {
            auto& self = static_cast<Recorder&>(work);
            self.record_->push_back(self.id_);
            if (self.follow_up_ != nullptr) {
                aloe::runtime::ShardContext::current()->ready().push(*self.follow_up_);
                self.follow_up_ = nullptr;
            }
        }

    private:
        std::vector<int>* record_;
        int id_;
        aloe::runtime::Work* follow_up_ = nullptr;
    };

    struct RecordingTimer : aloe::runtime::Timer {
        RecordingTimer(std::vector<int>& record, const int id)
            : Timer{&RecordingTimer::execute}
            , record_{&record}
            , id_{id} {
        }

        static void execute(aloe::runtime::Timer& timer) noexcept {
            auto& self = static_cast<RecordingTimer&>(timer);
            self.record_->push_back(self.id_);
        }

    private:
        std::vector<int>* record_;
        int id_;
    };

    class ShardContextTest : public testing::Test {
    protected:
        aloe::runtime::ShardContext context_{{.index = 3}, start};
        std::vector<int> record_;
    };

}  // namespace

TEST_F(ShardContextTest, TimersFireBeforeQueuedWorkAndInboxWorkJoinsTheQueueBehindIt) {
    Recorder queued{record_, 1};
    Recorder arrived{record_, 2};
    RecordingTimer due{record_, 3};
    context_.ready().push(queued);
    context_.inbox().push(arrived);
    context_.timers().arm(due, start + 1ms);

    EXPECT_TRUE(context_.run_once(start + 1ms));
    EXPECT_EQ(record_, (std::vector<int>{3, 1, 2}));
    EXPECT_EQ(context_.counters().inbox_received, 1);
    EXPECT_EQ(context_.counters().timers_fired, 1);
    EXPECT_EQ(context_.counters().work_run, 2);
    EXPECT_EQ(context_.now(), start + 1ms);
    EXPECT_EQ(context_.index(), 3);
}

TEST_F(ShardContextTest, WorkPushedDuringAStepRunsOnTheNextOne) {
    Recorder first{record_, 1};
    Recorder second{record_, 2};
    first.then_push(second);
    context_.ready().push(first);

    EXPECT_TRUE(context_.run_once(start));
    EXPECT_EQ(record_, (std::vector<int>{1}));
    EXPECT_FALSE(context_.ready().empty());
    EXPECT_TRUE(context_.run_once(start));
    EXPECT_EQ(record_, (std::vector<int>{1, 2}));
    EXPECT_FALSE(context_.run_once(start)) << "nothing left to do";
}

TEST_F(ShardContextTest, CurrentIsSetDuringAStepAndRestoredAfterwards) {
    EXPECT_EQ(aloe::runtime::ShardContext::current(), nullptr);
    {
        const aloe::runtime::ShardContext::Current current{context_};
        EXPECT_EQ(aloe::runtime::ShardContext::current(), &context_);
        aloe::runtime::ShardContext other{{.index = 4}, start};
        {
            const aloe::runtime::ShardContext::Current nested{other};
            EXPECT_EQ(aloe::runtime::ShardContext::current(), &other);
        }
        EXPECT_EQ(aloe::runtime::ShardContext::current(), &context_);
    }
    EXPECT_EQ(aloe::runtime::ShardContext::current(), nullptr);
}

TEST_F(ShardContextTest, DrainedNeedsAStopRequestAndEmptyQueuesAndScope) {
    EXPECT_FALSE(context_.drained());
    EXPECT_FALSE(context_.stop_requested());

    Recorder pending{record_, 1};
    context_.inbox().push(pending);
    context_.request_stop();
    EXPECT_TRUE(context_.stop_requested());
    EXPECT_TRUE(context_.scope().stop_requested()) << "stop reaches the scope";
    EXPECT_FALSE(context_.drained()) << "the inbox still holds work";

    EXPECT_TRUE(context_.run_once(start));
    EXPECT_TRUE(context_.drained());

    context_.request_stop();
    EXPECT_TRUE(context_.drained()) << "a second stop request changes nothing";
}

TEST_F(ShardContextTest, AnArmedTimerDoesNotHoldTheShardOpen) {
    RecordingTimer later{record_, 1};
    context_.timers().arm(later, start + 10s);
    context_.request_stop();
    EXPECT_TRUE(context_.drained()) << "a timer with no task behind it belongs to the stack and dies with it";
    context_.timers().cancel(later);
}
```

Add it to the `Runtime` target.

- [ ] **Step 2: See it fail**

```bash
cmake --preset debug > /dev/null && cmake --build --preset debug --target Aloe.Tests.Unit.Runtime 2>&1 | tail -5
```

Expected: a compile error naming the type this task introduces.

- [ ] **Step 3: `shard_context.hpp`**

```cpp
#pragma once

#include <aloe/core>
#include <chrono>
#include <cstdint>

#include <counters.hpp>
#include <inbox.hpp>
#include <run_queue.hpp>
#include <task_scope.hpp>
#include <timer_wheel.hpp>

namespace aloe::runtime {

    struct ShardContextConfig {
        std::uint16_t index                       = 0;  ///< The shard's, and its queue's, index.
        std::chrono::nanoseconds timer_resolution = std::chrono::milliseconds{1};
    };

    /**
     * @brief Everything of a shard that does not touch the device.
     *
     * Owns the run queue, the inbox, the timer wheel, the task scope, the tick stamp and the
     * counters. One thread owns a context for its whole life; every member function except
     * `Inbox::push` runs on that thread. A thread-local pointer, `current()`, names the context the
     * calling thread is running, which is how the scheduler tells same-shard from cross-shard.
     *
     * `run_once(now)` is its one verb: record the stamp, move the inbox's contents onto the run
     * queue, advance the wheel, run the work that was queued when the step began. Work queued
     * while the chain runs waits for the next step. Stop is a flag plus the scope's stop request;
     * `drained()` is the loop's exit condition.
     */
    class ShardContext {
    public:
        using Clock     = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;

        /// `start` is where the wheel's tick zero begins; the shard passes the clock, tests pass any stamp.
        ShardContext(const ShardContextConfig& config, TimePoint start);
        ShardContext(const ShardContext&)            = delete;
        ShardContext& operator=(const ShardContext&) = delete;
        ShardContext(ShardContext&&)                 = delete;
        ShardContext& operator=(ShardContext&&)      = delete;
        ~ShardContext()                              = default;

        /// The context the calling thread is running, or null.
        [[nodiscard]] static ShardContext* current() noexcept;

        /// Makes a context current on this thread for the object's lifetime; restores the previous one after.
        class Current {
        public:
            explicit Current(ShardContext& context) noexcept;
            Current(const Current&)            = delete;
            Current& operator=(const Current&) = delete;
            Current(Current&&)                 = delete;
            Current& operator=(Current&&)      = delete;
            ~Current();

        private:
            ShardContext* previous_;
        };

        /// One step. Returns whether anything ran or fired.
        bool run_once(TimePoint now) noexcept;

        /// Shard thread: sets the stop flag and stops the scope. Idempotent.
        void request_stop() noexcept;

        [[nodiscard]] bool stop_requested() const noexcept {
            return stopping_;
        }

        /// Stop requested, scope empty, both queues empty. Armed timers do not count.
        [[nodiscard]] bool drained() const noexcept {
            return stopping_ && scope_.empty() && ready_.empty() && inbox_.empty();
        }

        /// The stamp of the current or last step, never a clock read.
        [[nodiscard]] TimePoint now() const noexcept {
            return now_;
        }

        [[nodiscard]] std::uint16_t index() const noexcept {
            return index_;
        }

        [[nodiscard]] core::Logger logger() const noexcept {
            return logger_;
        }

        [[nodiscard]] RunQueue& ready() noexcept {
            return ready_;
        }

        [[nodiscard]] Inbox& inbox() noexcept {
            return inbox_;
        }

        [[nodiscard]] TimerWheel& timers() noexcept {
            return timers_;
        }

        [[nodiscard]] TaskScope& scope() noexcept {
            return scope_;
        }

        [[nodiscard]] ShardCounters& counters() noexcept {
            return counters_;
        }

        [[nodiscard]] const ShardCounters& counters() const noexcept {
            return counters_;
        }

    private:
        std::uint16_t index_;
        core::Logger logger_;
        ShardCounters counters_;
        TimePoint now_;
        RunQueue ready_;
        Inbox inbox_;
        TimerWheel timers_;
        TaskScope scope_;
        bool stopping_ = false;
    };

}  // namespace aloe::runtime
```

- [ ] **Step 4: `shard_context.cpp`**

```cpp
#include <cstddef>

#include <shard_context.hpp>

namespace aloe::runtime {

    namespace {

        thread_local ShardContext* current_context = nullptr;

    }  // namespace

    ShardContext::ShardContext(const ShardContextConfig& config, const TimePoint start)
        : index_{config.index}
        , logger_{core::logger("aloe.runtime")}
        , now_{start}
        , timers_{config.timer_resolution, start}
        , scope_{logger_, config.index, counters_} {
    }

    ShardContext* ShardContext::current() noexcept {
        return current_context;
    }

    ShardContext::Current::Current(ShardContext& context) noexcept
        : previous_{current_context} {
        current_context = &context;
    }

    ShardContext::Current::~Current() {
        current_context = previous_;
    }

    bool ShardContext::run_once(const TimePoint now) noexcept {
        const Current current{*this};
        now_      = now;
        bool busy = false;
        for (Work* work = inbox_.pop(); work != nullptr; work = inbox_.pop()) {
            ready_.push(*work);
            ++counters_.inbox_received;
            busy = true;
        }
        const std::size_t fired = timers_.advance(now);
        counters_.timers_fired += fired;
        const std::size_t ran = RunQueue::run_chain(ready_.take());
        counters_.work_run += ran;
        return busy || fired > 0 || ran > 0;
    }

    void ShardContext::request_stop() noexcept {
        stopping_ = true;
        scope_.request_stop();
    }

}  // namespace aloe::runtime
```

CMake block:

```cmake
##############################################################################
# The shard context: queues, timers, scope and counters, with run_once
##############################################################################
add_library(${RUNTIME}.Context STATIC
        context/shard_context.hpp
        context/shard_context.cpp
)
target_include_directories(${RUNTIME}.Context PUBLIC
        context/
)
target_link_libraries(${RUNTIME}.Context PUBLIC
        ${RUNTIME}.Work
        ${RUNTIME}.Timer
        ${RUNTIME}.Scope
)
##############################################################################
```

Add to `LIBRARIES` and `#include <shard_context.hpp>` to the umbrella.

- [ ] **Step 5: Build, test, commit**

```bash
cmake --build --preset debug --target Aloe.Tests.Unit.Runtime && ctest --preset debug -R Runtime
clang-format -i component/runtime/context/* tests/unit_tests/component/runtime/test_shard_context.cpp
./scripts/check-format.sh
git add component/runtime tests/unit_tests/component/runtime
git commit -m "feat(runtime): the shard context and its step"
```

---

### Task 8: `Scheduler`, `schedule`, `schedule_after`, `schedule_at`, `spawn`

Spec: "Scheduler and senders", "Threading contract". Review Focus 2 and 3.

**Files:**
- Create: `component/runtime/scheduler/scheduler.hpp`
- Modify: `component/runtime/CMakeLists.txt`, umbrella
- Create: `tests/unit_tests/component/runtime/test_scheduler.cpp`
- Modify: `tests/unit_tests/component/runtime/CMakeLists.txt`

**Interfaces:**
- Consumes: `ShardContext`, `Work`, `Timer`, `TaskScope::spawn`, `core::completion_behavior`, `core::get_completion_behavior_t`.
- Produces `aloe::runtime::Scheduler`:
  - `explicit Scheduler(ShardContext&) noexcept`; copyable; `operator==`; not default-constructible; `using scheduler_concept = core::ex::scheduler_t`; `using TimePoint = ShardContext::TimePoint`.
  - `schedule() const noexcept` → a sender of `set_value()` or `set_stopped()`, completing on the shard.
  - `schedule_after(std::chrono::nanoseconds) const noexcept`, `schedule_at(TimePoint) const noexcept` → senders of `set_value()` or `set_stopped()`, completing on the shard when the wheel fires, or stopped when the receiver's stop token is tripped, on the shard thread.
  - `now() const noexcept` → the tick stamp.
  - `template <core::ex::sender S> void spawn(S&&) const`: shard thread only; spawns into the context's scope with an environment answering `get_scheduler` and `get_start_scheduler` with this scheduler.
  - `context() const noexcept -> ShardContext&`.
  - All three senders' attributes answer `get_completion_scheduler<set_value_t>` and `<set_stopped_t>` with the `Scheduler`, and `core::get_completion_behavior_t<Tag>` with `asynchronous_affine`.

- [ ] **Step 1: Failing tests**

`tests/unit_tests/component/runtime/test_scheduler.cpp`:

```cpp
#include <aloe/runtime>
#include <chrono>
#include <concepts>
#include <thread>
#include <tuple>
#include <utility>

#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using TimePoint = aloe::runtime::ShardContext::TimePoint;

    constexpr TimePoint start{};

    struct Outcome {
        bool value   = false;
        bool stopped = false;
        std::thread::id thread;
    };

    /// Records how and where it completed; its environment carries the stop token the test controls.
    struct RecordingReceiver {
        using receiver_concept = aloe::core::ex::receiver_t;

        struct Env {
            aloe::core::ex::inplace_stop_token token;

            [[nodiscard]] aloe::core::ex::inplace_stop_token query(aloe::core::ex::get_stop_token_t) const noexcept {
                return token;
            }
        };

        Outcome* outcome;
        aloe::core::ex::inplace_stop_token token;

        void set_value() noexcept {
            outcome->value  = true;
            outcome->thread = std::this_thread::get_id();
        }

        void set_stopped() noexcept {
            outcome->stopped = true;
            outcome->thread  = std::this_thread::get_id();
        }

        [[nodiscard]] Env get_env() const noexcept {
            return Env{token};
        }
    };

    class SchedulerTest : public testing::Test {
    protected:
        aloe::runtime::ShardContext context_{{.index = 0}, start};
        aloe::runtime::Scheduler scheduler_{context_};
        aloe::core::ex::inplace_stop_source stop_;
        Outcome outcome_;

        [[nodiscard]] RecordingReceiver receiver() noexcept {
            return RecordingReceiver{&outcome_, stop_.get_token()};
        }
    };

    static_assert(aloe::core::ex::scheduler<aloe::runtime::Scheduler>);
    static_assert(!std::default_initializable<aloe::runtime::Scheduler>);

}  // namespace

TEST_F(SchedulerTest, EqualityFollowsTheContext) {
    aloe::runtime::ShardContext other{{.index = 1}, start};
    EXPECT_EQ(scheduler_, aloe::runtime::Scheduler{context_});
    EXPECT_NE(scheduler_, aloe::runtime::Scheduler{other});
    EXPECT_EQ(&scheduler_.context(), &context_);
}

TEST_F(SchedulerTest, SameShardScheduleUsesTheRunQueueAndNeverTheInbox) {
    const aloe::runtime::ShardContext::Current current{context_};
    auto operation = aloe::core::ex::connect(scheduler_.schedule(), receiver());
    aloe::core::ex::start(operation);
    EXPECT_FALSE(context_.ready().empty());
    EXPECT_TRUE(context_.inbox().empty());

    EXPECT_TRUE(context_.run_once(start));
    EXPECT_TRUE(outcome_.value);
    EXPECT_EQ(context_.counters().work_run, 1);
    EXPECT_EQ(context_.counters().inbox_received, 0);
}

TEST_F(SchedulerTest, CrossThreadScheduleGoesThroughTheInboxAndCompletesOnTheShard) {
    auto operation = aloe::core::ex::connect(scheduler_.schedule(), receiver());
    {
        std::jthread elsewhere{[&] { aloe::core::ex::start(operation); }};
    }
    EXPECT_FALSE(context_.inbox().empty());
    EXPECT_TRUE(context_.ready().empty());

    EXPECT_TRUE(context_.run_once(start));
    EXPECT_TRUE(outcome_.value);
    EXPECT_EQ(outcome_.thread, std::this_thread::get_id()) << "completed on the thread that ran the step";
    EXPECT_EQ(context_.counters().inbox_received, 1);
}

// Review Focus 2: stop requested after start, before the shard gets to the work.
TEST_F(SchedulerTest, StopRequestedBeforeTheWorkRunsCompletesStopped) {
    const aloe::runtime::ShardContext::Current current{context_};
    auto operation = aloe::core::ex::connect(scheduler_.schedule(), receiver());
    aloe::core::ex::start(operation);
    stop_.request_stop();

    EXPECT_TRUE(context_.run_once(start));
    EXPECT_TRUE(outcome_.stopped);
    EXPECT_FALSE(outcome_.value);
}

TEST_F(SchedulerTest, ScheduleAfterFiresOnTheFirstStepThatReachesItWithNoPush) {
    const aloe::runtime::ShardContext::Current current{context_};
    auto operation = aloe::core::ex::connect(scheduler_.schedule_after(5ms), receiver());
    aloe::core::ex::start(operation);
    EXPECT_EQ(context_.timers().pending(), 1);
    EXPECT_TRUE(context_.ready().empty());

    EXPECT_FALSE(context_.run_once(start + 4ms));
    EXPECT_FALSE(outcome_.value);
    EXPECT_TRUE(context_.run_once(start + 5ms));
    EXPECT_TRUE(outcome_.value);
    EXPECT_EQ(context_.counters().timers_fired, 1);
    EXPECT_EQ(context_.counters().work_run, 0) << "a timer completion is not a run-queue push";
}

TEST_F(SchedulerTest, ScheduleAtIsAbsoluteAndNowIsTheStamp) {
    const aloe::runtime::ShardContext::Current current{context_};
    std::ignore = context_.run_once(start + 7ms);
    EXPECT_EQ(scheduler_.now(), start + 7ms);

    auto operation = aloe::core::ex::connect(scheduler_.schedule_at(start + 20ms), receiver());
    aloe::core::ex::start(operation);
    EXPECT_FALSE(context_.run_once(start + 19ms));
    EXPECT_TRUE(context_.run_once(start + 20ms));
    EXPECT_TRUE(outcome_.value);
}

// Review Focus 3: a stop request cancels an armed timer on the shard thread and completes stopped.
TEST_F(SchedulerTest, AStopRequestCancelsAnArmedTimer) {
    const aloe::runtime::ShardContext::Current current{context_};
    auto operation = aloe::core::ex::connect(scheduler_.schedule_after(10ms), receiver());
    aloe::core::ex::start(operation);
    EXPECT_EQ(context_.timers().pending(), 1);

    stop_.request_stop();
    EXPECT_TRUE(outcome_.stopped);
    EXPECT_EQ(context_.timers().pending(), 0);
    EXPECT_FALSE(context_.run_once(start + 10ms)) << "nothing left to fire";
    EXPECT_FALSE(outcome_.value);
}

TEST_F(SchedulerTest, ATimerStartedOffTheShardHopsThroughTheInboxThenArms) {
    auto operation = aloe::core::ex::connect(scheduler_.schedule_after(5ms), receiver());
    {
        std::jthread elsewhere{[&] { aloe::core::ex::start(operation); }};
    }
    EXPECT_FALSE(context_.inbox().empty());
    EXPECT_EQ(context_.timers().pending(), 0);

    EXPECT_TRUE(context_.run_once(start));
    EXPECT_EQ(context_.timers().pending(), 1) << "armed on arrival, relative to the shard's stamp";
    EXPECT_FALSE(outcome_.value);
    EXPECT_TRUE(context_.run_once(start + 5ms));
    EXPECT_TRUE(outcome_.value);
}

TEST_F(SchedulerTest, SpawnRunsOnTheShardWithTheSchedulerInTheEnvironment) {
    struct SeesScheduler {
        using sender_concept        = aloe::core::ex::sender_t;
        using completion_signatures = aloe::core::ex::completion_signatures<aloe::core::ex::set_value_t()>;
        bool* matched;
        aloe::runtime::Scheduler expected;

        template <aloe::core::ex::receiver Receiver>
        struct Operation {
            Receiver receiver;
            bool* matched;
            aloe::runtime::Scheduler expected;

            void start() & noexcept {
                *matched = aloe::core::ex::get_scheduler(aloe::core::ex::get_env(receiver)) == expected;
                aloe::core::ex::set_value(std::move(receiver));
            }
        };

        template <aloe::core::ex::receiver Receiver>
        auto connect(Receiver receiver) const -> Operation<Receiver> {
            return Operation<Receiver>{std::move(receiver), matched, expected};
        }
    };

    const aloe::runtime::ShardContext::Current current{context_};
    bool matched = false;
    scheduler_.spawn(SeesScheduler{&matched, scheduler_});
    EXPECT_TRUE(matched);
    EXPECT_EQ(context_.counters().tasks_completed, 1);
    EXPECT_TRUE(context_.scope().empty());
}
```

Add it to the `Runtime` target, which needs `Threads::Threads` now; add that to its link libraries.

- [ ] **Step 2: See it fail**

```bash
cmake --preset debug > /dev/null && cmake --build --preset debug --target Aloe.Tests.Unit.Runtime 2>&1 | tail -5
```

Expected: a compile error naming the type this task introduces.

- [ ] **Step 3: `scheduler.hpp`**

```cpp
#pragma once

#include <aloe/core>
#include <cassert>
#include <chrono>
#include <optional>
#include <utility>

#include <shard_context.hpp>
#include <timer_wheel.hpp>
#include <work.hpp>

namespace aloe::runtime {

    class Scheduler;

    namespace detail {

        /// Attributes of every shard sender: it completes on its scheduler, asynchronously, so a
        /// task started on that scheduler awaits it without rescheduling.
        struct ShardSenderAttributes {
            ShardContext* context;

            template <typename Tag>
            [[nodiscard]] Scheduler query(core::ex::get_completion_scheduler_t<Tag>) const noexcept;

            template <typename Tag>
            [[nodiscard]] constexpr auto query(core::get_completion_behavior_t<Tag>) const noexcept {
                return core::completion_behavior::asynchronous_affine;
            }
        };

        /// The operation state of `schedule()`: a Work node the shard runs.
        template <typename Receiver>
        struct ScheduleOperation : Work {
            ShardContext* context;
            Receiver receiver;

            ScheduleOperation(ShardContext* owner, Receiver r) noexcept
                : Work{&ScheduleOperation::execute}
                , context{owner}
                , receiver{std::move(r)} {
            }

            void start() & noexcept {
                if (ShardContext::current() == context) {
                    context->ready().push(*this);
                } else {
                    context->inbox().push(*this);
                }
            }

            static void execute(Work& work) noexcept {
                auto& self = static_cast<ScheduleOperation&>(work);
                if (core::ex::get_stop_token(core::ex::get_env(self.receiver)).stop_requested()) {
                    core::ex::set_stopped(std::move(self.receiver));
                } else {
                    core::ex::set_value(std::move(self.receiver));
                }
            }
        };

        struct ScheduleSender {
            using sender_concept        = core::ex::sender_t;
            using completion_signatures = core::ex::completion_signatures<core::ex::set_value_t(), core::ex::set_stopped_t()>;

            ShardContext* context;

            template <core::ex::receiver Receiver>
            [[nodiscard]] auto connect(Receiver receiver) const noexcept -> ScheduleOperation<Receiver> {
                return ScheduleOperation<Receiver>{context, std::move(receiver)};
            }

            [[nodiscard]] ShardSenderAttributes get_env() const noexcept {
                return ShardSenderAttributes{context};
            }
        };

        /**
         * The operation state of `schedule_after` and `schedule_at`: a Timer in the wheel, and a Work
         * node for the hop through the inbox when started off the shard. Started on the shard it arms
         * at once, `after` relative to the tick stamp. A stop request, which by contract arrives on
         * the shard thread, cancels the timer and completes stopped.
         */
        template <typename Receiver>
        struct TimerOperation : Work, Timer {
            using StopToken = core::ex::stop_token_of_t<core::ex::env_of_t<Receiver>>;

            struct OnStop {
                TimerOperation* self;

                void operator()() noexcept {
                    self->cancel();
                }
            };

            using Callback = core::ex::stop_callback_for_t<StopToken, OnStop>;

            ShardContext* context;
            Receiver receiver;
            std::chrono::nanoseconds after;
            std::optional<ShardContext::TimePoint> at;
            std::optional<Callback> callback;

            TimerOperation(ShardContext* owner,
                           Receiver r,
                           const std::chrono::nanoseconds delay,
                           const std::optional<ShardContext::TimePoint> deadline) noexcept
                : Work{&TimerOperation::arrive}
                , Timer{&TimerOperation::fired}
                , context{owner}
                , receiver{std::move(r)}
                , after{delay}
                , at{deadline} {
            }

            void start() & noexcept {
                if (ShardContext::current() == context) {
                    arm();
                } else {
                    context->inbox().push(static_cast<Work&>(*this));
                }
            }

            static void arrive(Work& work) noexcept {
                static_cast<TimerOperation&>(work).arm();
            }

            void arm() noexcept {
                const StopToken token = core::ex::get_stop_token(core::ex::get_env(receiver));
                if (token.stop_requested()) {
                    core::ex::set_stopped(std::move(receiver));
                    return;
                }
                const ShardContext::TimePoint when = at.has_value() ? *at : context->now() + after;
                context->timers().arm(static_cast<Timer&>(*this), when);
                callback.emplace(token, OnStop{this});
            }

            void cancel() noexcept {
                assert(ShardContext::current() == context && "a shard's operation is stopped on its own thread");
                Timer& timer = static_cast<Timer&>(*this);
                if (!timer.armed()) {
                    return;
                }
                context->timers().cancel(timer);
                callback.reset();
                core::ex::set_stopped(std::move(receiver));
            }

            static void fired(Timer& timer) noexcept {
                auto& self = static_cast<TimerOperation&>(timer);
                self.callback.reset();
                core::ex::set_value(std::move(self.receiver));
            }
        };

        struct TimerSender {
            using sender_concept        = core::ex::sender_t;
            using completion_signatures = core::ex::completion_signatures<core::ex::set_value_t(), core::ex::set_stopped_t()>;

            ShardContext* context;
            std::chrono::nanoseconds after;
            std::optional<ShardContext::TimePoint> at;

            template <core::ex::receiver Receiver>
            [[nodiscard]] auto connect(Receiver receiver) const noexcept -> TimerOperation<Receiver> {
                return TimerOperation<Receiver>{context, std::move(receiver), after, at};
            }

            [[nodiscard]] ShardSenderAttributes get_env() const noexcept {
                return ShardSenderAttributes{context};
            }
        };

        /// The environment spawned work starts with: the scheduler, under both names stdexec asks for.
        struct SchedulerEnv;

    }  // namespace detail

    /**
     * @brief A shard's scheduler: one pointer to its context, a value type, never type-erased.
     *
     * Models the stdexec scheduler and adds the timed shape: `now()`, `schedule_after`,
     * `schedule_at`. Same-shard `schedule()` pushes onto the run queue with no atomics; from any
     * other thread it goes through the inbox. Timers arm into the shard's wheel. Every sender
     * completes on the shard and says so, so a task bound to this scheduler never reschedules.
     * Deliberately not default-constructible: a task started without a shard fails to compile.
     */
    class Scheduler {
    public:
        using scheduler_concept = core::ex::scheduler_t;
        using TimePoint         = ShardContext::TimePoint;

        explicit Scheduler(ShardContext& context) noexcept
            : context_{&context} {
        }

        [[nodiscard]] detail::ScheduleSender schedule() const noexcept {
            return detail::ScheduleSender{context_};
        }

        /// Relative to the tick stamp at the moment the operation arms on the shard.
        [[nodiscard]] detail::TimerSender schedule_after(const std::chrono::nanoseconds delay) const noexcept {
            return detail::TimerSender{context_, delay, std::nullopt};
        }

        [[nodiscard]] detail::TimerSender schedule_at(const TimePoint deadline) const noexcept {
            return detail::TimerSender{context_, std::chrono::nanoseconds::zero(), deadline};
        }

        /// The tick stamp, never a clock read.
        [[nodiscard]] TimePoint now() const noexcept {
            return context_->now();
        }

        /// Spawns into the shard's scope with this scheduler in the environment. Shard thread only.
        template <core::ex::sender Sender>
        void spawn(Sender&& sender) const;

        [[nodiscard]] ShardContext& context() const noexcept {
            return *context_;
        }

        [[nodiscard]] constexpr auto query(core::ex::get_forward_progress_guarantee_t) const noexcept {
            return core::ex::forward_progress_guarantee::parallel;
        }

        friend bool operator==(const Scheduler&, const Scheduler&) noexcept = default;

    private:
        ShardContext* context_;
    };

    namespace detail {

        struct SchedulerEnv {
            Scheduler scheduler;

            [[nodiscard]] Scheduler query(core::ex::get_scheduler_t) const noexcept {
                return scheduler;
            }

            [[nodiscard]] Scheduler query(core::ex::get_start_scheduler_t) const noexcept {
                return scheduler;
            }
        };

        template <typename Tag>
        Scheduler ShardSenderAttributes::query(core::ex::get_completion_scheduler_t<Tag>) const noexcept {
            return Scheduler{*context};
        }

    }  // namespace detail

    template <core::ex::sender Sender>
    void Scheduler::spawn(Sender&& sender) const {
        assert(ShardContext::current() == context_ && "spawn on the shard's own thread; hop with schedule() first");
        context_->scope().spawn(std::forward<Sender>(sender), detail::SchedulerEnv{*this});
    }

}  // namespace aloe::runtime
```

CMake block:

```cmake
##############################################################################
# The scheduler and its senders
##############################################################################
add_library(${RUNTIME}.Scheduler INTERFACE
        scheduler/scheduler.hpp
)
target_include_directories(${RUNTIME}.Scheduler INTERFACE
        scheduler/
)
target_link_libraries(${RUNTIME}.Scheduler INTERFACE
        ${RUNTIME}.Context
)
##############################################################################
```

Add to `LIBRARIES` and the umbrella.

- [ ] **Step 4: Build, test, both compilers**

```bash
cmake --build --preset debug --target Aloe.Tests.Unit.Runtime && ctest --preset debug -R Runtime
cmake --build --preset gcc-debug --target Aloe.Tests.Unit.Runtime && ctest --preset gcc-debug -R Runtime
```

Expected: pass. Things that can go wrong, and what they mean:
- `ScheduleOperation<Receiver>` is not an operation state: stdexec needs `start()` as a member called on an lvalue; the `&` qualifier above is right, check the receiver satisfies `receiver_of` for both completion signatures.
- The attributes query for `get_completion_scheduler_t<Tag>` must be `noexcept` and return a scheduler; if stdexec also asks for `set_error_t`, which these senders never send, the template answers it harmlessly.
- `stop_callback_for_t<never_stop_token, OnStop>` must be constructible from `(token, OnStop)`; it is, as `never_stop_token::callback_type` takes and ignores both.

- [ ] **Step 5: Format, commit**

```bash
clang-format -i component/runtime/scheduler/scheduler.hpp tests/unit_tests/component/runtime/test_scheduler.cpp
./scripts/check-format.sh
git add component/runtime tests/unit_tests/component/runtime
git commit -m "feat(runtime): the shard scheduler with schedule, timer senders and spawn"
```

---

### Task 9: `ShardEnvironment` and `runtime::task<T>`

Spec: "The shard task". Done-when 3.

**Files:**
- Create: `component/runtime/task/shard_task.hpp`
- Modify: `component/runtime/CMakeLists.txt`, umbrella
- Create: `tests/unit_tests/component/runtime/test_shard_task.cpp`
- Modify: `tests/unit_tests/component/runtime/CMakeLists.txt`

**Interfaces:**
- Consumes: `Scheduler`, `core::TaskEnvironment`, `core::task`.
- Produces: `aloe::runtime::ShardEnvironment = core::TaskEnvironment<Scheduler, core::ex::inplace_stop_source>` and `template <typename T> using aloe::runtime::task = core::task<T, ShardEnvironment>`.

- [ ] **Step 1: Failing tests**

`tests/unit_tests/component/runtime/test_shard_task.cpp`:

```cpp
#include <aloe/runtime>
#include <chrono>
#include <stdexcept>

#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using TimePoint = aloe::runtime::ShardContext::TimePoint;

    constexpr TimePoint start{};

    class ShardTaskTest : public testing::Test {
    protected:
        aloe::runtime::ShardContext context_{{.index = 0}, start};
        aloe::runtime::Scheduler scheduler_{context_};
        aloe::runtime::ShardContext::Current current_{context_};
    };

    aloe::runtime::task<void> wait_once(aloe::runtime::Scheduler scheduler) {
        co_await scheduler.schedule_after(1ms);
    }

    aloe::runtime::task<int> child(aloe::runtime::Scheduler expected, bool* stop_seen) {
        const auto here  = co_await aloe::core::ex::read_env(aloe::core::ex::get_scheduler);
        const auto token = co_await aloe::core::ex::read_env(aloe::core::ex::get_stop_token);
        *stop_seen       = token.stop_possible();
        co_return here == expected ? 1 : 0;
    }

    aloe::runtime::task<void> parent(aloe::runtime::Scheduler expected, int* matched, bool* stop_seen) {
        *matched = co_await child(expected, stop_seen);
    }

    aloe::runtime::task<void> parked(aloe::runtime::Scheduler scheduler) {
        co_await scheduler.schedule_after(10ms);
    }

    aloe::runtime::task<void> throws() {
        co_await aloe::core::ex::just();
        throw std::runtime_error{"boom"};
    }

    aloe::runtime::task<void> reschedules(aloe::runtime::Scheduler scheduler) {
        co_await scheduler.schedule();
    }

    aloe::runtime::task<int> answers() {
        co_return 42;
    }

}  // namespace

// Done-when 3: the proof that the concrete scheduler keeps the task from rescheduling.
TEST_F(ShardTaskTest, ATaskAwaitingATimerCostsOneWheelEntryAndNoRunQueuePush) {
    scheduler_.spawn(wait_once(scheduler_));
    EXPECT_EQ(context_.timers().pending(), 1);
    EXPECT_TRUE(context_.ready().empty());
    EXPECT_EQ(context_.scope().size(), 1);

    EXPECT_TRUE(context_.run_once(start + 1ms));
    EXPECT_EQ(context_.counters().timers_fired, 1);
    EXPECT_EQ(context_.counters().work_run, 0);
    EXPECT_EQ(context_.counters().tasks_completed, 1);
    EXPECT_TRUE(context_.scope().empty());
}

TEST_F(ShardTaskTest, AChildTaskInheritsSchedulerAndStopToken) {
    int matched    = 0;
    bool stop_seen = false;
    scheduler_.spawn(parent(scheduler_, &matched, &stop_seen));
    EXPECT_EQ(matched, 1);
    EXPECT_TRUE(stop_seen) << "the scope's stop token, not an unstoppable one, reaches the child";
    EXPECT_EQ(context_.counters().work_run, 0) << "awaiting a child task reschedules nothing";
}

TEST_F(ShardTaskTest, AStopRequestUnwindsATaskParkedOnATimer) {
    scheduler_.spawn(parked(scheduler_));
    EXPECT_EQ(context_.timers().pending(), 1);

    context_.request_stop();
    EXPECT_EQ(context_.timers().pending(), 0) << "the timer was cancelled";
    EXPECT_EQ(context_.counters().tasks_stopped, 1);
    EXPECT_TRUE(context_.scope().empty());
    EXPECT_TRUE(context_.drained());
}

TEST_F(ShardTaskTest, AnExceptionInATaskIsCountedAsFailed) {
    scheduler_.spawn(throws());
    EXPECT_EQ(context_.counters().tasks_failed, 1);
    EXPECT_TRUE(context_.scope().empty());
}

TEST_F(ShardTaskTest, ATaskAwaitingScheduleRunsOnTheNextStep) {
    scheduler_.spawn(reschedules(scheduler_));
    EXPECT_FALSE(context_.ready().empty());
    EXPECT_EQ(context_.counters().tasks_completed, 0);

    EXPECT_TRUE(context_.run_once(start));
    EXPECT_EQ(context_.counters().work_run, 1);
    EXPECT_EQ(context_.counters().tasks_completed, 1);
}

TEST_F(ShardTaskTest, ATaskReturningAValueCompletesAndTheValueIsDropped) {
    scheduler_.spawn(answers());
    EXPECT_EQ(context_.counters().tasks_completed, 1);
}

// Spec, "Task scope": every spawn after request_stop starts with stop already requested; here for a task.
TEST_F(ShardTaskTest, ATaskSpawnedAfterStopNeverArmsItsTimerAndCompletesStopped) {
    context_.request_stop();
    scheduler_.spawn(parked(scheduler_));
    EXPECT_EQ(context_.timers().pending(), 0) << "the timer sender saw the stop before arming";
    EXPECT_EQ(context_.counters().tasks_stopped, 1);
    EXPECT_TRUE(context_.scope().empty());
    EXPECT_TRUE(context_.drained());
}
```

Add it to the `Runtime` target.

- [ ] **Step 2: See it fail**

```bash
cmake --preset debug > /dev/null && cmake --build --preset debug --target Aloe.Tests.Unit.Runtime 2>&1 | tail -5
```

Expected: a compile error naming the type this task introduces.

- [ ] **Step 3: `shard_task.hpp`**

```cpp
#pragma once

#include <aloe/core>

#include <scheduler.hpp>

namespace aloe::runtime {

    /**
     * @brief The environment of a shard task: the concrete Scheduler, an in-place stop source,
     * `std::exception_ptr` errors, and the default allocator until the connection arena arrives.
     *
     * Because the scheduler type is concrete and its senders complete on the shard, a task with
     * this environment holds one pointer and never reschedules around a shard sender.
     */
    using ShardEnvironment = core::TaskEnvironment<Scheduler, core::ex::inplace_stop_source>;

    /// A coroutine task bound to a shard. Start it with `Scheduler::spawn`, or await it from another shard task.
    template <typename T>
    using task = core::task<T, ShardEnvironment>;

}  // namespace aloe::runtime
```

CMake block:

```cmake
##############################################################################
# The shard task
##############################################################################
add_library(${RUNTIME}.Task INTERFACE
        task/shard_task.hpp
)
target_include_directories(${RUNTIME}.Task INTERFACE
        task/
)
target_link_libraries(${RUNTIME}.Task INTERFACE
        ${RUNTIME}.Scheduler
)
##############################################################################
```

Add to `LIBRARIES` and the umbrella.

- [ ] **Step 4: Build, test on both compilers, commit**

```bash
cmake --build --preset debug --target Aloe.Tests.Unit.Runtime && ctest --preset debug -R Runtime
cmake --build --preset gcc-debug --target Aloe.Tests.Unit.Runtime && ctest --preset gcc-debug -R Runtime
```

Expected: pass. If `ATaskAwaitingATimerCostsOneWheelEntryAndNoRunQueuePush` reports `work_run == 1`, the task wrapped the timer in `affine`: check that `ShardSenderAttributes::query(core::get_completion_behavior_t<Tag>)` is found for `set_value_t` on the `schedule()` sender, which is the one the task inspects. If `AChildTaskInheritsSchedulerAndStopToken` fails on `stop_seen`, the child did not get the parent's stop token; read `__task.hpp` around `__stop_callback_box` and record what the environment needs.

```bash
clang-format -i component/runtime/task/shard_task.hpp tests/unit_tests/component/runtime/test_shard_task.cpp
./scripts/check-format.sh
git add component/runtime tests/unit_tests/component/runtime
git commit -m "feat(runtime): the shard task environment and task alias"
```

---

### Task 10: `ShardQueue`, `IsStack`, `Shard`, and the echo stack

Spec: "The shard": ShardQueue, Stack contract, Tick, Idle policy, Configuration. Review Focus 4.

**Files:**
- Create: `component/runtime/shard/shard_queue.hpp`, `component/runtime/shard/shard.hpp`
- Modify: `component/runtime/CMakeLists.txt`, umbrella
- Create: `tests/shared/runtime/echo_stack.hpp`; modify `tests/shared/CMakeLists.txt`
- Create: `tests/unit_tests/component/runtime/test_shard.cpp`; modify its CMake file

**Interfaces:**
- Consumes: `device::IsDevice`, `ShardContext`, `Scheduler`, `ShardCounters`.
- Produces:
  - `aloe::runtime::ShardQueue<Device>(Device&, std::uint16_t queue, std::size_t ring_capacity, ShardCounters&)`: `using Packet`; `std::optional<Packet> allocate() noexcept`; `std::size_t receive(std::span<Packet>) noexcept`; `bool transmit(Packet&&) noexcept` (false leaves the packet with the caller and counts `transmit_refused`); `std::size_t flush() noexcept` (counts `frames_transmitted`); `void discard() noexcept` (drops the ring, counting `transmit_refused`); `pending()`, `capacity()`, `index()`, `mac()`, `mtu()`, `capabilities()`, `steering()`, `queue_count()`, `const Device& device()`.
  - `aloe::runtime::IdlePolicy { Spin, Yield }`; `aloe::runtime::ShardConfig { timer_resolution = 1ms; receive_burst = 64; transmit_ring = 512; idle = Spin; yield_after = 1000; }`.
  - `aloe::runtime::IsStack<S, Device>`: `S& s; s.on_receive(std::span<typename Device::Packet>) -> void`.
  - `aloe::runtime::Shard<Device, Stack>(const ShardConfig&, Device&, std::uint16_t queue, TimePoint start, Args&&...)`: constructs `Stack{context, queue, args...}`; `bool step(TimePoint) noexcept`; `void run() noexcept`; `context()`, `scheduler()`, `queue()`, `stack()`, `config()`.
  - `aloe::testing::EchoStack<Device>(ShardContext&, ShardQueue<Device>&)`: `on_receive` answers frames addressed to the port's MAC: swaps the Ethernet addresses, stamps the shard index big-endian into the last two bytes, transmits. Other frames and short frames are counted `dropped()` and left to the shard. `echoed()`, `dropped()`, `refused()`. `aloe::testing::stamp_of(std::span<const std::byte>) -> std::uint16_t`.

- [ ] **Step 1: The echo stack**

`tests/shared/runtime/echo_stack.hpp`:

```cpp
#pragma once

#include <aloe/device>
#include <aloe/runtime>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace aloe::testing {

    /**
     * @brief The simplest stack: swap the Ethernet addresses, stamp the shard index into the last
     * two bytes of the payload, send the frame back on the queue it came from.
     *
     * Only frames addressed to the port's own MAC are answered. Anything else, and anything shorter
     * than an Ethernet header plus two bytes, is counted dropped and left in the burst for the shard
     * to free. That is what lets an echo on a loopback device terminate: the reply comes back
     * addressed to the peer and is dropped. The stamp is how a test learns which shard answered.
     */
    template <device::IsDevice Device>
    class EchoStack {
    public:
        using Packet = typename Device::Packet;

        EchoStack(runtime::ShardContext& context, runtime::ShardQueue<Device>& queue) noexcept
            : context_{&context}
            , queue_{&queue} {
        }

        void on_receive(std::span<Packet> burst) noexcept {
            for (Packet& packet : burst) {
                const std::span<std::byte> data = packet.data();
                if (data.size() < device::ethernet_header_size + 2 || !addressed_to_me(data)) {
                    ++dropped_;
                    continue;
                }
                std::swap_ranges(data.begin(), data.begin() + 6, data.begin() + 6);
                device::store_be16(data.last(2), context_->index());
                if (queue_->transmit(std::move(packet))) {
                    ++echoed_;
                } else {
                    ++refused_;
                }
            }
        }

        [[nodiscard]] std::uint64_t echoed() const noexcept {
            return echoed_;
        }

        [[nodiscard]] std::uint64_t dropped() const noexcept {
            return dropped_;
        }

        [[nodiscard]] std::uint64_t refused() const noexcept {
            return refused_;
        }

    private:
        [[nodiscard]] bool addressed_to_me(const std::span<const std::byte> frame) const noexcept {
            device::MacAddress::Bytes destination{};
            std::ranges::copy(frame.first(device::MacAddress::size), destination.begin());
            return device::MacAddress{destination} == queue_->mac();
        }

        runtime::ShardContext* context_;
        runtime::ShardQueue<Device>* queue_;
        std::uint64_t echoed_  = 0;
        std::uint64_t dropped_ = 0;
        std::uint64_t refused_ = 0;
    };

    /// The shard index an echo stamped into a frame.
    [[nodiscard]] inline std::uint16_t stamp_of(const std::span<const std::byte> frame) {
        return device::load_be16(frame.last(2));
    }

}  // namespace aloe::testing
```

`tests/shared/CMakeLists.txt`:

```cmake
##############################################################################
# The Ethernet echo stack the runtime tests and examples run
##############################################################################
add_library(${SHARED_TESTING_TARGET}.Runtime INTERFACE
        runtime/echo_stack.hpp
)
target_include_directories(${SHARED_TESTING_TARGET}.Runtime INTERFACE
        runtime/
)
target_link_libraries(${SHARED_TESTING_TARGET}.Runtime INTERFACE
        Aloe::Component::Runtime
        Aloe::Component::Device
)
##############################################################################
```

- [ ] **Step 2: Failing tests**

`tests/unit_tests/component/runtime/test_shard.cpp`:

```cpp
#include <aloe/fabric>
#include <aloe/runtime>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <echo_stack.hpp>
#include <frames.hpp>
#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using TimePoint = aloe::runtime::ShardContext::TimePoint;

    constexpr TimePoint start{};
    constexpr aloe::device::MacAddress server{0x02, 0, 0, 0, 0, 0x01};
    constexpr aloe::device::MacAddress client{0x02, 0, 0, 0, 0, 0x02};

    /// A device that accepts at most `accept` packets per transmit call; everything else is the port's.
    class Throttled {
    public:
        using Packet = aloe::fabric::Packet;

        explicit Throttled(aloe::fabric::Port& port) noexcept
            : port_{&port} {
        }

        std::size_t accept = 64;

        [[nodiscard]] std::uint16_t queue_count() const noexcept {
            return port_->queue_count();
        }

        [[nodiscard]] aloe::device::MacAddress mac() const noexcept {
            return port_->mac();
        }

        [[nodiscard]] std::uint16_t mtu() const noexcept {
            return port_->mtu();
        }

        [[nodiscard]] bool link_up() const noexcept {
            return port_->link_up();
        }

        [[nodiscard]] const aloe::device::Capabilities& capabilities() const noexcept {
            return port_->capabilities();
        }

        [[nodiscard]] const aloe::device::RssDescription& steering() const noexcept {
            return port_->steering();
        }

        [[nodiscard]] std::optional<Packet> allocate(const std::uint16_t queue) noexcept {
            return port_->allocate(queue);
        }

        [[nodiscard]] std::size_t receive(const std::uint16_t queue, std::span<Packet> out) noexcept {
            return port_->receive(queue, out);
        }

        [[nodiscard]] std::size_t transmit(const std::uint16_t queue, std::span<Packet> in) noexcept {
            return port_->transmit(queue, in.first(std::min(in.size(), accept)));
        }

        [[nodiscard]] aloe::device::QueueCounters counters(const std::uint16_t queue) const noexcept {
            return port_->counters(queue);
        }

    private:
        aloe::fabric::Port* port_;
    };

    static_assert(aloe::device::IsDevice<Throttled>);
    static_assert(aloe::runtime::IsStack<aloe::testing::EchoStack<aloe::fabric::Port>, aloe::fabric::Port>);

    void send(aloe::fabric::Port& from, const std::span<const std::byte> frame) {
        auto packet = from.allocate(0);
        ASSERT_TRUE(packet.has_value());
        ASSERT_TRUE(aloe::testing::fill(*packet, frame));
        std::array<aloe::fabric::Packet, 1> burst{std::move(*packet)};
        ASSERT_EQ(from.transmit(0, burst), 1);
    }

    std::vector<std::vector<std::byte>> drain(aloe::fabric::Port& port) {
        std::vector<std::vector<std::byte>> frames;
        std::array<aloe::fabric::Packet, 16> burst;
        for (std::size_t count = port.receive(0, burst); count > 0; count = port.receive(0, burst)) {
            for (std::size_t index = 0; index < count; ++index) {
                frames.push_back(aloe::testing::bytes_of(burst[index]));
                burst[index] = aloe::fabric::Packet{};
            }
        }
        return frames;
    }

    class ShardTest : public testing::Test {
    protected:
        aloe::fabric::Fabric fabric_;
        aloe::fabric::Port& server_ = fabric_.add_port({.mac = server, .queues = 1, .pool_size = 16});
        aloe::fabric::Port& client_ = fabric_.add_port({.mac = client, .queues = 1, .pool_size = 64});
        aloe::runtime::ShardConfig config_{.receive_burst = 8, .transmit_ring = 4, .idle = aloe::runtime::IdlePolicy::Yield};
    };

    aloe::runtime::task<void> parked(aloe::runtime::Scheduler scheduler) {
        co_await scheduler.schedule_after(10s);
    }

}  // namespace

TEST_F(ShardTest, TheEchoRepliesInTheTickTheFrameArrived) {
    aloe::runtime::Shard<aloe::fabric::Port, aloe::testing::EchoStack<aloe::fabric::Port>> shard{config_, server_, 0, start};
    const auto frame =
        aloe::testing::ethernet_frame(server, client, aloe::testing::ethertype_experimental, aloe::testing::pattern(30));
    send(client_, frame);

    EXPECT_TRUE(shard.step(start));
    const auto replies = drain(client_);
    ASSERT_EQ(replies.size(), 1);
    const auto& reply = replies[0];
    EXPECT_EQ(std::vector<std::byte>(reply.begin(), reply.begin() + 6), std::vector<std::byte>(frame.begin() + 6, frame.begin() + 12))
        << "destination is the old source";
    EXPECT_EQ(std::vector<std::byte>(reply.begin() + 6, reply.begin() + 12), std::vector<std::byte>(frame.begin(), frame.begin() + 6))
        << "source is the old destination";
    EXPECT_EQ(aloe::testing::stamp_of(reply), 0);
    EXPECT_EQ(std::vector<std::byte>(reply.begin() + 12, reply.end() - 2), std::vector<std::byte>(frame.begin() + 12, frame.end() - 2));
    EXPECT_EQ(shard.context().counters().frames_received, 1);
    EXPECT_EQ(shard.context().counters().frames_transmitted, 1);
    EXPECT_EQ(shard.context().counters().ticks, 1);
    EXPECT_EQ(shard.stack().echoed(), 1);
}

TEST_F(ShardTest, PacketsTheStackLeavesAreFreedSoThePoolNeverRunsDry) {
    aloe::runtime::Shard<aloe::fabric::Port, aloe::testing::EchoStack<aloe::fabric::Port>> shard{config_, server_, 0, start};
    const auto too_short = aloe::testing::ethernet_frame(server, client, aloe::testing::ethertype_experimental, aloe::testing::pattern(1));
    constexpr std::size_t rounds = 5;
    constexpr std::size_t per_round = 12;  // less than the 16-packet pool, more than one 8-packet burst

    for (std::size_t round = 0; round < rounds; ++round) {
        for (std::size_t index = 0; index < per_round; ++index) {
            send(client_, too_short);
        }
        for (int step = 0; step < 4; ++step) {
            std::ignore = shard.step(start);
        }
    }
    EXPECT_EQ(shard.context().counters().frames_received, rounds * per_round) << "a leaked packet would have starved the pool";
    EXPECT_EQ(shard.stack().dropped(), rounds * per_round);
    EXPECT_EQ(shard.context().counters().frames_transmitted, 0);
    EXPECT_TRUE(drain(client_).empty());
}

// Review Focus 4: the ring fills, the device refuses, the caller keeps its packet.
TEST_F(ShardTest, TheTransmitRingFlushesWhenFullAndRefusesWhenTheDeviceDoes) {
    Throttled throttled{server_};
    aloe::runtime::Shard<Throttled, aloe::testing::EchoStack<Throttled>> shard{config_, throttled, 0, start};
    const auto frame = aloe::testing::ethernet_frame(server, client, aloe::testing::ethertype_experimental, aloe::testing::pattern(20));
    for (int index = 0; index < 6; ++index) {
        send(client_, frame);
    }

    throttled.accept = 0;
    EXPECT_TRUE(shard.step(start));
    EXPECT_EQ(shard.stack().echoed(), 4) << "the ring holds four";
    EXPECT_EQ(shard.stack().refused(), 2) << "the fifth filled it, the flush took nothing, so it and the sixth were refused";
    EXPECT_EQ(shard.context().counters().transmit_refused, 2);
    EXPECT_EQ(shard.queue().pending(), 4);
    EXPECT_TRUE(drain(client_).empty());

    throttled.accept = 64;
    EXPECT_TRUE(shard.step(start)) << "the flush at the end of the tick sent the ring";
    EXPECT_EQ(shard.queue().pending(), 0);
    EXPECT_EQ(shard.context().counters().frames_transmitted, 4);
    EXPECT_EQ(drain(client_).size(), 4);
}

TEST_F(ShardTest, StopUnwindsParkedTasksAndTheShardDrains) {
    aloe::runtime::Shard<aloe::fabric::Port, aloe::testing::EchoStack<aloe::fabric::Port>> shard{config_, server_, 0, start};
    {
        const aloe::runtime::ShardContext::Current current{shard.context()};
        shard.scheduler().spawn(parked(shard.scheduler()));
    }
    EXPECT_EQ(shard.context().scope().size(), 1);
    EXPECT_FALSE(shard.context().drained());

    shard.context().request_stop();
    EXPECT_EQ(shard.context().counters().tasks_stopped, 1);
    EXPECT_TRUE(shard.context().drained());
}

TEST_F(ShardTest, AnEmptyStepIsIdle) {
    aloe::runtime::Shard<aloe::fabric::Port, aloe::testing::EchoStack<aloe::fabric::Port>> shard{config_, server_, 0, start};
    EXPECT_FALSE(shard.step(start));
    EXPECT_EQ(shard.context().counters().ticks, 1);
    EXPECT_EQ(shard.context().counters().idle_ticks, 1);
    EXPECT_EQ(shard.config().idle, aloe::runtime::IdlePolicy::Yield);
}
```

Add `test_shard.cpp` to the `Runtime` target and link `Aloe::Component::Fabric`, `${SHARED_TESTING_TARGET}.Device` and `${SHARED_TESTING_TARGET}.Runtime` to it.

- [ ] **Step 3: See it fail**

```bash
cmake --preset debug > /dev/null && cmake --build --preset debug --target Aloe.Tests.Unit.Runtime 2>&1 | tail -5
```

Expected: a compile error naming the type this task introduces.

- [ ] **Step 4: `shard_queue.hpp`**

```cpp
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <address.hpp>
#include <counters.hpp>
#include <device.hpp>
#include <rss.hpp>

namespace aloe::runtime {

    /**
     * @brief One queue of a device as the stack sees it: allocate, transmit, and the device's facts.
     *
     * Owns a bounded transmit ring. `transmit` appends; when the ring is full it flushes to the
     * device first and retries once; if the device still refuses it returns false and the caller
     * keeps the packet. That false is the backpressure signal TCP treats as loss. The shard flushes
     * the ring at the end of every tick. Nothing here blocks, throws or allocates after construction.
     */
    template <device::IsDevice Device>
    class ShardQueue {
    public:
        using Packet = typename Device::Packet;

        ShardQueue(Device& device, const std::uint16_t queue, const std::size_t ring_capacity, ShardCounters& counters)
            : device_{&device}
            , queue_{queue}
            , counters_{&counters}
            , ring_(ring_capacity) {
        }

        ShardQueue(const ShardQueue&)            = delete;
        ShardQueue& operator=(const ShardQueue&) = delete;
        ShardQueue(ShardQueue&&)                 = delete;
        ShardQueue& operator=(ShardQueue&&)      = delete;
        ~ShardQueue()                            = default;

        [[nodiscard]] std::optional<Packet> allocate() noexcept {
            return device_->allocate(queue_);
        }

        /// Fills `out` from the front and returns how many; the slots must hold empty packets.
        [[nodiscard]] std::size_t receive(std::span<Packet> out) noexcept {
            return device_->receive(queue_, out);
        }

        /// False when the ring is full and the device takes nothing; the packet then stays with the caller.
        [[nodiscard]] bool transmit(Packet&& packet) noexcept {
            if (size_ == ring_.size()) {
                flush();
                if (size_ == ring_.size()) {
                    ++counters_->transmit_refused;
                    return false;
                }
            }
            ring_[(head_ + size_) % ring_.size()] = std::move(packet);
            ++size_;
            return true;
        }

        /// Hands the ring's front to the device, at most two calls because the ring wraps; returns how many it accepted.
        /// A partial accept ends the flush. Ethdev takes at most `Port::max_burst` per call, so a ring holding more
        /// than that drains over several ticks; harmless for the echo, noted in the spec's "Open questions" for TCP.
        std::size_t flush() noexcept {
            std::size_t accepted_total = 0;
            while (size_ > 0) {
                const std::size_t contiguous = std::min(size_, ring_.size() - head_);
                const std::size_t accepted   = device_->transmit(queue_, std::span<Packet>{ring_}.subspan(head_, contiguous));
                accepted_total += accepted;
                head_ = (head_ + accepted) % ring_.size();
                size_ -= accepted;
                if (accepted < contiguous) {
                    break;
                }
            }
            counters_->frames_transmitted += accepted_total;
            return accepted_total;
        }

        /// Drops whatever the device would not take, counting it as refused. The shard calls it once at drain.
        void discard() noexcept {
            while (size_ > 0) {
                ring_[head_] = Packet{};
                head_        = (head_ + 1) % ring_.size();
                --size_;
                ++counters_->transmit_refused;
            }
        }

        [[nodiscard]] std::size_t pending() const noexcept {
            return size_;
        }

        [[nodiscard]] std::size_t capacity() const noexcept {
            return ring_.size();
        }

        [[nodiscard]] std::uint16_t index() const noexcept {
            return queue_;
        }

        [[nodiscard]] device::MacAddress mac() const noexcept {
            return device_->mac();
        }

        [[nodiscard]] std::uint16_t mtu() const noexcept {
            return device_->mtu();
        }

        [[nodiscard]] const device::Capabilities& capabilities() const noexcept {
            return device_->capabilities();
        }

        [[nodiscard]] const device::RssDescription& steering() const noexcept {
            return device_->steering();
        }

        [[nodiscard]] std::uint16_t queue_count() const noexcept {
            return device_->queue_count();
        }

        [[nodiscard]] const Device& device() const noexcept {
            return *device_;
        }

    private:
        Device* device_;
        std::uint16_t queue_;
        ShardCounters* counters_;
        std::vector<Packet> ring_;
        std::size_t head_ = 0;
        std::size_t size_ = 0;
    };

}  // namespace aloe::runtime
```

- [ ] **Step 5: `shard.hpp`**

```cpp
#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <device.hpp>
#include <scheduler.hpp>
#include <shard_context.hpp>
#include <shard_queue.hpp>

namespace aloe::runtime {

    /// What a shard does when a step finds nothing to do.
    enum class IdlePolicy : std::uint8_t {
        Spin,   ///< Poll again at once: the production default on an isolated core.
        Yield,  ///< After `yield_after` empty steps, yield the thread once per empty step: tests and CI.
    };

    struct ShardConfig {
        std::chrono::nanoseconds timer_resolution = std::chrono::milliseconds{1};
        std::size_t receive_burst                 = 64;  ///< Packets one receive call may bring; ethdev moves at most 64.
        std::size_t transmit_ring                 = 512;
        IdlePolicy idle                           = IdlePolicy::Spin;
        std::uint32_t yield_after                 = 1000;
    };

    /**
     * @brief The layer above a shard.
     *
     * Constructed by the shard from `ShardContext&`, `ShardQueue<Device>&` and the caller's arguments.
     * `on_receive` is called on the shard thread, inside a tick, with a non-empty burst, and must
     * not throw; the stack moves out the packets it keeps and the shard frees the rest. Timers,
     * tasks and scheduling go through the context.
     */
    template <typename S, typename Device>
    concept IsStack = device::IsDevice<Device> && requires(S& stack, std::span<typename Device::Packet> burst) {
        { stack.on_receive(burst) } -> std::same_as<void>;
    };

    /**
     * @brief One shard: a context, one device queue and the stack, with the loop.
     *
     * One tick, `step(now)`: receive a burst, hand it to the stack, `run_once`, flush the transmit
     * ring. Packet work goes before timers and tasks so a reply leaves in the tick its request
     * arrived. `run()` loops `step(clock::now())` until the context is drained, then flushes once
     * more and drops what the device still refuses. Tests call `step` with stamps of their choosing.
     */
    template <device::IsDevice Device, typename Stack>
        requires IsStack<Stack, Device>
    class Shard {
    public:
        using Packet    = typename Device::Packet;
        using Clock     = ShardContext::Clock;
        using TimePoint = ShardContext::TimePoint;

        template <typename... Args>
            requires std::constructible_from<Stack, ShardContext&, ShardQueue<Device>&, Args...>
        Shard(const ShardConfig& config, Device& device, const std::uint16_t queue, const TimePoint start, Args&&... args)
            : config_{config}
            , context_{{.index = queue, .timer_resolution = config.timer_resolution}, start}
            , queue_{device, queue, config.transmit_ring, context_.counters()}
            , stack_{context_, queue_, std::forward<Args>(args)...}
            , burst_(config.receive_burst) {
        }

        Shard(const Shard&)            = delete;
        Shard& operator=(const Shard&) = delete;
        Shard(Shard&&)                 = delete;
        Shard& operator=(Shard&&)      = delete;
        ~Shard()                       = default;

        /// One tick. Returns whether anything was received, run, fired or sent.
        bool step(const TimePoint now) noexcept {
            const ShardContext::Current current{context_};
            bool busy                  = false;
            const std::size_t received = queue_.receive(burst_);
            if (received > 0) {
                context_.counters().frames_received += received;
                const std::span<Packet> frames{burst_.data(), received};
                stack_.on_receive(frames);
                for (Packet& packet : frames) {
                    if (!packet.empty()) {
                        packet = Packet{};
                    }
                }
                busy = true;
            }
            busy = context_.run_once(now) || busy;
            busy = queue_.flush() > 0 || busy;
            ++context_.counters().ticks;
            if (!busy) {
                ++context_.counters().idle_ticks;
            }
            return busy;
        }

        /// Ticks until the context is drained. Returns with the transmit ring empty.
        void run() noexcept {
            std::uint32_t idle = 0;
            while (!context_.drained()) {
                if (step(Clock::now())) {
                    idle = 0;
                } else if (config_.idle == IdlePolicy::Yield && ++idle >= config_.yield_after) {
                    std::this_thread::yield();
                }
            }
            std::ignore = queue_.flush();
            queue_.discard();
        }

        [[nodiscard]] ShardContext& context() noexcept {
            return context_;
        }

        [[nodiscard]] const ShardContext& context() const noexcept {
            return context_;
        }

        [[nodiscard]] Scheduler scheduler() noexcept {
            return Scheduler{context_};
        }

        [[nodiscard]] ShardQueue<Device>& queue() noexcept {
            return queue_;
        }

        [[nodiscard]] Stack& stack() noexcept {
            return stack_;
        }

        [[nodiscard]] const ShardConfig& config() const noexcept {
            return config_;
        }

    private:
        ShardConfig config_;
        ShardContext context_;
        ShardQueue<Device> queue_;
        Stack stack_;  ///< Declared after the two it holds, so it is destroyed first.
        std::vector<Packet> burst_;
    };

}  // namespace aloe::runtime
```

CMake block:

```cmake
##############################################################################
# The shard: one device queue, the stack, and the tick
##############################################################################
add_library(${RUNTIME}.Shard INTERFACE
        shard/shard_queue.hpp
        shard/shard.hpp
)
target_include_directories(${RUNTIME}.Shard INTERFACE
        shard/
)
target_link_libraries(${RUNTIME}.Shard INTERFACE
        ${RUNTIME}.Scheduler
        Aloe::Component::Device
        Threads::Threads
)
##############################################################################
```

Add to `LIBRARIES`; add `shard.hpp` and `shard_queue.hpp` to the umbrella (alphabetical: `shard.hpp`, `shard_context.hpp`, `shard_queue.hpp`, `shard_task.hpp`).

- [ ] **Step 6: Build, test, commit**

```bash
cmake --preset debug > /dev/null && cmake --build --preset debug --target Aloe.Tests.Unit.Runtime && ctest --preset debug -R Runtime
cmake --build --preset gcc-debug --target Aloe.Tests.Unit.Runtime && ctest --preset gcc-debug -R Runtime
clang-format -i component/runtime/shard/* tests/shared/runtime/echo_stack.hpp tests/unit_tests/component/runtime/test_shard.cpp
./scripts/check-format.sh
git add component/runtime tests/shared tests/unit_tests/component/runtime
git commit -m "feat(runtime): the shard, its queue view, the stack contract and the echo stack"
```

---

### Task 11: `Runtime`

Spec: "The runtime": Launch, Stop and drain, From outside; "Error handling"; "Logging in the runtime". Review Focus 5.

**Files:**
- Create: `component/runtime/runtime/runtime.hpp`
- Modify: `component/runtime/CMakeLists.txt`, umbrella
- Create: `tests/unit_tests/component/runtime/test_runtime.cpp`; modify its CMake file

**Interfaces:**
- Consumes: `Shard`, `Scheduler`, `Work`, `core::Logger`.
- Produces:
  - `aloe::runtime::RuntimeError : std::runtime_error`.
  - `aloe::runtime::ShardThread { std::optional<unsigned> cpu; std::string name; }`.
  - `aloe::runtime::RuntimeConfig { ShardConfig shard; std::vector<ShardThread> threads; std::function<void()> thread_hook; }`.
  - `aloe::runtime::Runtime<Device, Stack>`: `template <typename... Args> Runtime(const RuntimeConfig&, Device&, const Args&... stack_args)` (one shard per queue, args copied into every stack); `void start()`; `void stop() noexcept`; `void join()`; `Scheduler scheduler(std::uint16_t) const noexcept`; `template <core::ex::sender S> void spawn(std::uint16_t, S&&)`; `Shard<Device, Stack>& shard(std::uint16_t) noexcept`; `const ShardCounters& counters(std::uint16_t) const noexcept`; `std::uint16_t shard_count() const noexcept`. Destructor stops and joins. A thread that cannot be created is a `RuntimeError` like every other `start` failure; `spawn` after `join` asserts in debug builds.

- [ ] **Step 1: Failing tests**

`tests/unit_tests/component/runtime/test_runtime.cpp`:

```cpp
#include <aloe/fabric>
#include <aloe/runtime>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <echo_stack.hpp>
#include <frames.hpp>
#include <gtest/gtest.h>
#include <sched.h>

namespace {

    using namespace std::chrono_literals;

    constexpr std::uint16_t queues = 4;
    constexpr aloe::device::MacAddress server{0x02, 0, 0, 0, 0, 0x01};
    constexpr aloe::device::MacAddress client{0x02, 0, 0, 0, 0, 0x02};
    constexpr aloe::device::Ipv4Address server_ip{10, 0, 0, 2};
    constexpr aloe::device::Ipv4Address client_ip{10, 0, 0, 1};
    constexpr auto patience = 5s;

    using EchoRuntime = aloe::runtime::Runtime<aloe::fabric::Port, aloe::testing::EchoStack<aloe::fabric::Port>>;

    /// Polls `condition` until it holds or `patience` runs out.
    [[nodiscard]] bool eventually(const std::function<bool()>& condition) {
        const auto deadline = std::chrono::steady_clock::now() + patience;
        while (std::chrono::steady_clock::now() < deadline) {
            if (condition()) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return condition();
    }

    aloe::runtime::task<void> mark(std::atomic<int>* slot, std::atomic<int>* ran_on) {
        const auto scheduler = co_await aloe::core::ex::read_env(aloe::core::ex::get_scheduler);
        ran_on->store(scheduler.context().index());
        slot->store(1);
    }

    aloe::runtime::task<void> park(aloe::runtime::Scheduler scheduler, std::atomic<int>* parked) {
        parked->store(1);
        co_await scheduler.schedule_after(10s);
    }

    class RuntimeTest : public testing::Test {
    protected:
        aloe::fabric::Fabric fabric_;
        aloe::fabric::Port& server_ = fabric_.add_port({.mac = server, .queues = queues, .pool_size = 64});
        aloe::fabric::Port& client_ = fabric_.add_port({.mac = client, .queues = 1, .pool_size = 64});
        aloe::runtime::RuntimeConfig config_{
            .shard = {.idle = aloe::runtime::IdlePolicy::Yield, .yield_after = 10}, .threads = {}, .thread_hook = {}};
    };

}  // namespace

TEST_F(RuntimeTest, OneShardPerQueueRunsSpawnedWorkOnTheRightShardThenStopsAndJoins) {
    EchoRuntime runtime{config_, server_};
    EXPECT_EQ(runtime.shard_count(), queues);
    runtime.start();

    std::array<std::atomic<int>, queues> done{};
    std::array<std::atomic<int>, queues> ran_on{};
    for (std::uint16_t index = 0; index < queues; ++index) {
        ran_on[index].store(-1);
        runtime.spawn(index, mark(&done[index], &ran_on[index]));
    }
    ASSERT_TRUE(eventually([&] {
        for (const auto& flag : done) {
            if (flag.load() == 0) {
                return false;
            }
        }
        return true;
    }));
    for (std::uint16_t index = 0; index < queues; ++index) {
        EXPECT_EQ(ran_on[index].load(), static_cast<int>(index)) << "spawned onto shard " << index;
    }

    runtime.stop();
    runtime.join();
    for (std::uint16_t index = 0; index < queues; ++index) {
        EXPECT_EQ(runtime.counters(index).tasks_completed, 1) << "shard " << index;
        EXPECT_GT(runtime.counters(index).ticks, 0U);
    }
}

TEST_F(RuntimeTest, TheHookRunsOnceOnEveryShardThreadBeforeStartReturns) {
    std::atomic<int> hook_calls{0};
    config_.thread_hook = [&hook_calls] { ++hook_calls; };
    EchoRuntime runtime{config_, server_};
    runtime.start();
    EXPECT_EQ(hook_calls.load(), queues);
}

TEST_F(RuntimeTest, AFailingHookOrAnImpossibleCpuThrowsFromStartAndLeavesNothingRunning) {
    {
        config_.thread_hook = [] { throw std::runtime_error{"no lcore for you"}; };
        EchoRuntime runtime{config_, server_};
        EXPECT_THROW(runtime.start(), aloe::runtime::RuntimeError);
    }
    {
        config_.thread_hook = {};
        config_.threads.assign(queues, aloe::runtime::ShardThread{.cpu = static_cast<unsigned>(CPU_SETSIZE - 1), .name = "nowhere"});
        EchoRuntime runtime{config_, server_};
        EXPECT_THROW(runtime.start(), aloe::runtime::RuntimeError);
    }
}

TEST_F(RuntimeTest, ConstructionRejectsAThreadListThatDoesNotMatchTheQueues) {
    config_.threads.assign(queues - 1, aloe::runtime::ShardThread{});
    EXPECT_THROW((EchoRuntime{config_, server_}), aloe::runtime::RuntimeError);
}

TEST_F(RuntimeTest, StartTwiceThrows) {
    EchoRuntime runtime{config_, server_};
    runtime.start();
    EXPECT_THROW(runtime.start(), aloe::runtime::RuntimeError);
}

// Review Focus 5: stop with tasks parked on timers returns promptly, every task stopped.
TEST_F(RuntimeTest, StopUnwindsParkedTasksOnEveryShardAndJoinReturnsPromptly) {
    EchoRuntime runtime{config_, server_};
    runtime.start();
    std::array<std::atomic<int>, queues> parked{};
    for (std::uint16_t index = 0; index < queues; ++index) {
        runtime.spawn(index, park(runtime.scheduler(index), &parked[index]));
    }
    ASSERT_TRUE(eventually([&] {
        for (const auto& flag : parked) {
            if (flag.load() == 0) {
                return false;
            }
        }
        return true;
    }));

    const auto before = std::chrono::steady_clock::now();
    runtime.stop();
    runtime.join();
    EXPECT_LT(std::chrono::steady_clock::now() - before, 2s) << "the ten-second timers were cancelled, not awaited";
    for (std::uint16_t index = 0; index < queues; ++index) {
        EXPECT_EQ(runtime.counters(index).tasks_stopped, 1) << "shard " << index;
    }
}

TEST_F(RuntimeTest, AFrameIsAnsweredByTheShardItsHashSelects) {
    EchoRuntime runtime{config_, server_};
    runtime.start();
    const aloe::testing::Ipv4Spec spec{.destination_mac  = server,
                                       .source_mac       = client,
                                       .source           = client_ip,
                                       .destination      = server_ip,
                                       .source_port      = 40000,
                                       .destination_port = 80,
                                       .protocol         = aloe::device::Ipv4Protocol::Udp};
    const auto frame = aloe::testing::ipv4_frame(spec, aloe::testing::pattern(16));
    const std::uint16_t expected = aloe::device::queue_for(server_.steering(), aloe::testing::flow_of(spec));

    auto packet = client_.allocate(0);
    ASSERT_TRUE(packet.has_value());
    ASSERT_TRUE(aloe::testing::fill(*packet, frame));
    std::array<aloe::fabric::Packet, 1> burst{std::move(*packet)};
    ASSERT_EQ(client_.transmit(0, burst), 1);

    std::optional<std::vector<std::byte>> reply;
    ASSERT_TRUE(eventually([&] {
        std::array<aloe::fabric::Packet, 1> received;
        if (client_.receive(0, received) == 1) {
            reply = aloe::testing::bytes_of(received[0]);
            return true;
        }
        return false;
    }));
    EXPECT_EQ(aloe::testing::stamp_of(*reply), expected);
}
```

Add it to the `Runtime` target.

- [ ] **Step 2: See it fail**

```bash
cmake --preset debug > /dev/null && cmake --build --preset debug --target Aloe.Tests.Unit.Runtime 2>&1 | tail -5
```

Expected: a compile error naming the type this task introduces.

- [ ] **Step 3: `runtime.hpp`**

```cpp
#pragma once

#include <aloe/core>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <device.hpp>
#include <pthread.h>
#include <sched.h>
#include <scheduler.hpp>
#include <shard.hpp>
#include <work.hpp>

namespace aloe::runtime {

    /// Thrown by construction and `start`, never on the hot path.
    class RuntimeError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    struct ShardThread {
        std::optional<unsigned> cpu = std::nullopt;  ///< Pin the shard thread here; none means unpinned.
        std::string name{};                          ///< Thread name; empty means `aloe-shard-N`.
    };

    /// Every member has a default, so a designated initialiser may name only what it changes.
    struct RuntimeConfig {
        ShardConfig shard{};
        std::vector<ShardThread> threads{};   ///< One per queue, or empty for unpinned defaults.
        std::function<void()> thread_hook{};  ///< Runs first on every shard thread; `ethdev::register_thread` for DPDK.
    };

    /**
     * @brief N shards over an N-queue device, one thread each.
     *
     * `start` launches the threads and returns once every one has run its hook; a hook that throws
     * or a CPU that cannot be pinned stops what already started and throws. `stop` posts a stop into
     * every inbox, from any thread, idempotent; shards then drain and their threads end. `join`
     * waits. The destructor does both. `scheduler(i)` works from any thread, `spawn(i, sender)` hops
     * to shard i and spawns there. `shard(i)` and `counters(i)` are for the shard's own thread or
     * for after `join`.
     */
    template <device::IsDevice Device, typename Stack>
        requires IsStack<Stack, Device>
    class Runtime {
    public:
        using ShardType = Shard<Device, Stack>;

        /// One shard per device queue. `stack_args` are copied into every stack.
        template <typename... Args>
        Runtime(const RuntimeConfig& config, Device& device, const Args&... stack_args)
            : config_{config} {
            const std::uint16_t queues = device.queue_count();
            if (queues == 0) {
                throw RuntimeError{"a runtime needs a device with at least one queue"};
            }
            if (!config.threads.empty() && config.threads.size() != queues) {
                throw RuntimeError{"one ShardThread per queue, or none"};
            }
            const ShardContext::TimePoint start = ShardContext::Clock::now();
            shards_.reserve(queues);
            stops_.reserve(queues);
            for (std::uint16_t queue = 0; queue < queues; ++queue) {
                shards_.push_back(std::make_unique<ShardType>(config.shard, device, queue, start, stack_args...));
                stops_.push_back(std::make_unique<StopWork>(shards_.back()->context()));
            }
        }

        Runtime(const Runtime&)            = delete;
        Runtime& operator=(const Runtime&) = delete;
        Runtime(Runtime&&)                 = delete;
        Runtime& operator=(Runtime&&)      = delete;

        ~Runtime() {
            stop();
            join();
        }

        void start() {
            if (started_) {
                throw RuntimeError{"a runtime starts once"};
            }
            started_ = true;
            std::vector<std::future<void>> ready;
            ready.reserve(shards_.size());
            threads_.reserve(shards_.size());
            for (std::uint16_t index = 0; index < shards_.size(); ++index) {
                std::promise<void> promise;
                ready.push_back(promise.get_future());
                auto body = [this, index, promise = std::move(promise)]() mutable {
                    try {
                        prepare_thread(index);
                        promise.set_value();
                    } catch (...) {
                        promise.set_exception(std::current_exception());
                        return;
                    }
                    shards_[index]->run();
                    const ShardCounters& counters = shards_[index]->context().counters();
                    shards_[index]->context().logger().info<"shard {} drained after {} ticks: {} frames in, {} out, {} tasks ran, {} failed">(
                        index, counters.ticks, counters.frames_received, counters.frames_transmitted,
                        counters.tasks_completed + counters.tasks_stopped, counters.tasks_failed);
                };
                try {
                    threads_.emplace_back(std::move(body));
                } catch (const std::system_error& error) {
                    // Spec, "Error handling": a thread that fails to start is a RuntimeError, like every other start failure.
                    stop();
                    join();
                    throw RuntimeError{"shard " + std::to_string(index) + ": thread could not be created: " + error.what()};
                }
            }
            std::exception_ptr first_failure;
            for (std::future<void>& future : ready) {
                try {
                    future.get();
                } catch (...) {
                    if (!first_failure) {
                        first_failure = std::current_exception();
                    }
                }
            }
            if (first_failure) {
                stop();
                join();
                try {
                    std::rethrow_exception(first_failure);
                } catch (const RuntimeError& error) {
                    log().error<"a shard thread failed to start: {}">(error.what());
                    throw;
                } catch (const std::exception& exception) {
                    log().error<"a shard thread failed to start: {}">(exception.what());
                    throw RuntimeError{std::string{"a shard thread failed to start: "} + exception.what()};
                }
            }
        }

        /// Any thread. Idempotent.
        void stop() noexcept {
            if (stopped_.exchange(true)) {
                return;
            }
            log().info<"stop requested for {} shards">(shards_.size());
            for (std::unique_ptr<StopWork>& pending : stops_) {  // not `stop`: GCC's -Wshadow sees the member function
                pending->context->inbox().push(*pending);
            }
        }

        void join() {
            for (std::jthread& thread : threads_) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
            joined_ = !threads_.empty();
        }

        [[nodiscard]] Scheduler scheduler(const std::uint16_t index) const noexcept {
            return Scheduler{shards_[index]->context()};
        }

        /// Any thread. Posts the sender to shard `index`, which spawns it into its scope.
        template <core::ex::sender Sender>
        void spawn(const std::uint16_t index, Sender&& sender) {
            assert(!joined_ && "spawn after join: nothing reads the inbox any more and the work would leak");
            using Plain = std::remove_cvref_t<Sender>;
            auto* work  = new SpawnWork<Plain>{shards_[index]->context(), Plain{std::forward<Sender>(sender)}};
            shards_[index]->context().inbox().push(*work);
        }

        [[nodiscard]] ShardType& shard(const std::uint16_t index) noexcept {
            return *shards_[index];
        }

        [[nodiscard]] const ShardCounters& counters(const std::uint16_t index) const noexcept {
            return shards_[index]->context().counters();
        }

        [[nodiscard]] std::uint16_t shard_count() const noexcept {
            return static_cast<std::uint16_t>(shards_.size());
        }

    private:
        struct StopWork : Work {
            explicit StopWork(ShardContext& owner) noexcept
                : Work{&StopWork::execute}
                , context{&owner} {
            }

            static void execute(Work& work) noexcept {
                static_cast<StopWork&>(work).context->request_stop();
            }

            ShardContext* context;
        };

        template <typename Sender>
        struct SpawnWork : Work {
            SpawnWork(ShardContext& owner, Sender s)
                : Work{&SpawnWork::execute}
                , context{&owner}
                , sender{std::move(s)} {
            }

            static void execute(Work& work) noexcept {
                auto* self = static_cast<SpawnWork*>(&work);
                Scheduler{*self->context}.spawn(std::move(self->sender));
                delete self;
            }

            ShardContext* context;
            Sender sender;
        };

        void prepare_thread(const std::uint16_t index) {
            const ShardThread options = config_.threads.empty() ? ShardThread{} : config_.threads[index];
            const std::string name    = options.name.empty() ? "aloe-shard-" + std::to_string(index) : options.name;
            std::ignore = pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
            if (options.cpu) {
                if (*options.cpu >= CPU_SETSIZE) {
                    throw RuntimeError{"shard " + std::to_string(index) + ": cpu " + std::to_string(*options.cpu) + " is out of range"};
                }
                cpu_set_t set;
                CPU_ZERO(&set);
                CPU_SET(*options.cpu, &set);
                if (const int error = pthread_setaffinity_np(pthread_self(), sizeof(set), &set); error != 0) {
                    throw RuntimeError{"shard " + std::to_string(index) + ": cannot pin to cpu " + std::to_string(*options.cpu) + ": " + std::strerror(error)};
                }
            }
            if (config_.thread_hook) {
                config_.thread_hook();
            }
            shards_[index]->context().logger().info<"shard {} starting as {} on cpu {}">(
                index, name, options.cpu ? static_cast<int>(*options.cpu) : -1);
        }

        /// The runtime's cold-path logger: the first shard's, which is the module's.
        [[nodiscard]] core::Logger log() const noexcept {
            return shards_.front()->context().logger();
        }

        RuntimeConfig config_;
        std::vector<std::unique_ptr<ShardType>> shards_;
        std::vector<std::unique_ptr<StopWork>> stops_;
        std::vector<std::jthread> threads_;
        std::atomic<bool> stopped_{false};
        bool started_ = false;
        bool joined_  = false;
    };

}  // namespace aloe::runtime
```

CMake block:

```cmake
##############################################################################
# The runtime: N shards on N threads
##############################################################################
add_library(${RUNTIME}.Runtime INTERFACE
        runtime/runtime.hpp
)
target_include_directories(${RUNTIME}.Runtime INTERFACE
        runtime/
)
target_link_libraries(${RUNTIME}.Runtime INTERFACE
        ${RUNTIME}.Shard
        Threads::Threads
)
##############################################################################
```

Add to `LIBRARIES` and `#include <runtime.hpp>` to the umbrella.

- [ ] **Step 4: Build and test under debug, gcc-debug and tsan**

```bash
cmake --build --preset debug --target Aloe.Tests.Unit.Runtime && ctest --preset debug -R Runtime
cmake --build --preset gcc-debug --target Aloe.Tests.Unit.Runtime && ctest --preset gcc-debug -R Runtime
cmake --build --preset tsan --target Aloe.Tests.Unit.Runtime && ctest --preset tsan -R Runtime
```

Expected: pass, no TSan report. The only cross-thread paths are the inbox, the promises in `start`, the atomics in the tests, and quill's queue. A TSan report inside quill means its frontend queue is not annotated for the sanitizer; if so, add a suppression file for quill only, under `tests/` with a comment, and wire it through `TSAN_OPTIONS` in the `tsan` test preset's environment. Record either outcome in the spec's "Verified facts".

- [ ] **Step 5: Format, commit**

```bash
clang-format -i component/runtime/runtime/runtime.hpp tests/unit_tests/component/runtime/test_runtime.cpp
./scripts/check-format.sh
git add component/runtime tests/unit_tests/component/runtime
git commit -m "feat(runtime): the runtime that launches one shard per queue and drains them"
```

---

### Task 12: Cross-shard and steering tests under ThreadSanitizer

Spec: "Testing: Threads". Done-when 1 and 2 (fabric half). Threading contract rule 4.

**Files:**
- Create: `tests/unit_tests/component/runtime/test_cross_shard.cpp`, `tests/unit_tests/component/runtime/test_steering.cpp`
- Modify: `tests/unit_tests/component/runtime/CMakeLists.txt`

**Interfaces:**
- Consumes: `Runtime`, `EchoStack`, `stamp_of`, `queue_for`, `flow_of`, `ipv4_frame`.
- Produces: nothing new; both files join the `Runtime.Threads` target.

- [ ] **Step 1: The cross-shard test**

A shard task never awaits another shard's scheduler (spec, threading contract rule 4), so the hop is an operation state whose receiver starts the next hop from the shard it landed on. That is the shape the runtime itself uses for `Runtime::spawn` and `Runtime::stop`, exercised thousands of times from both sides.

`tests/unit_tests/component/runtime/test_cross_shard.cpp`:

```cpp
#include <aloe/fabric>
#include <aloe/runtime>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <thread>
#include <utility>

#include <echo_stack.hpp>
#include <gtest/gtest.h>
#include <logging_environment.hpp>

namespace {

    using namespace std::chrono_literals;

    constexpr int rounds    = 5000;
    constexpr aloe::device::MacAddress server{0x02, 0, 0, 0, 0, 0x01};
    constexpr auto patience = 20s;

    // One logging environment per test binary; this file owns it for the Runtime.Threads target.
    const auto* const logging = ::testing::AddGlobalTestEnvironment(new aloe::testing::LoggingEnvironment{});

    [[nodiscard]] bool eventually(const std::function<bool()>& condition) {
        const auto deadline = std::chrono::steady_clock::now() + patience;
        while (std::chrono::steady_clock::now() < deadline) {
            if (condition()) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return condition();
    }

    /**
     * Hops between two shards through their inboxes: `other.schedule()` from wherever it is, then
     * `home.schedule()` from the other shard, `rounds` times. An operation state, not a task: the
     * receiver starts the next hop on the shard it landed on, and two slots alternate so a hop never
     * destroys the operation that is completing it. Counts every landing on the wrong shard.
     */
    class Hopper {
    public:
        Hopper(const aloe::runtime::Scheduler home,
               const aloe::runtime::Scheduler other,
               std::atomic<int>* wrong,
               std::atomic<int>* done) noexcept
            : home_{home}
            , other_{other}
            , wrong_{wrong}
            , done_{done} {
        }

        Hopper(const Hopper&)            = delete;
        Hopper& operator=(const Hopper&) = delete;

        /// Any thread. Starts the first hop, towards `other`.
        void start() noexcept {
            hop();
        }

    private:
        struct Receiver {
            using receiver_concept = aloe::core::ex::receiver_t;
            Hopper* self;

            void set_value() noexcept {
                self->landed();
            }

            void set_stopped() noexcept {
                self->done_->store(-1);
            }
        };

        using HopSender = decltype(std::declval<const aloe::runtime::Scheduler&>().schedule());

        /// Builds the operation in place: operation states are immovable.
        struct Slot {
            Slot(const aloe::runtime::Scheduler target, Receiver receiver) noexcept
                : operation{aloe::core::ex::connect(target.schedule(), receiver)} {
            }

            aloe::core::ex::connect_result_t<HopSender, Receiver> operation;
        };

        /// Even hops go to `other`, odd hops come home.
        [[nodiscard]] aloe::runtime::Scheduler target() const noexcept {
            return hops_ % 2 == 0 ? other_ : home_;
        }

        void hop() noexcept {
            const auto slot = static_cast<std::size_t>(hops_ % 2);
            slots_[slot].emplace(target(), Receiver{this});
            aloe::core::ex::start(slots_[slot]->operation);
        }

        /// On the shard the hop targeted. The push into the next inbox orders every write here before the next landing.
        void landed() noexcept {
            if (aloe::runtime::ShardContext::current() != &target().context()) {
                ++*wrong_;
            }
            ++hops_;
            if (hops_ == 2 * rounds) {
                done_->store(1);
                return;
            }
            hop();
        }

        aloe::runtime::Scheduler home_;
        aloe::runtime::Scheduler other_;
        std::atomic<int>* wrong_;
        std::atomic<int>* done_;
        int hops_ = 0;
        std::array<std::optional<Slot>, 2> slots_;
    };

}  // namespace

// Two hoppers cross between two shards through their inboxes thousands of times, one started from each
// side. Under tsan this is the proof that the inbox and the scheduler's cross-shard path are race-free.
// The hoppers are declared before the runtime so that stop and join run before they are destroyed, on
// the failure path too; that is why the checks are EXPECTs and not ASSERTs.
TEST(CrossShard, HopsBetweenTwoShardsThroughTheirInboxes) {
    aloe::fabric::Fabric fabric;
    auto& port = fabric.add_port({.mac = server, .queues = 2, .pool_size = 16});
    std::atomic<int> wrong{0};
    std::atomic<int> done_a{0};
    std::atomic<int> done_b{0};
    std::optional<Hopper> a;
    std::optional<Hopper> b;

    aloe::runtime::Runtime<aloe::fabric::Port, aloe::testing::EchoStack<aloe::fabric::Port>> runtime{
        {.shard = {.idle = aloe::runtime::IdlePolicy::Yield, .yield_after = 10}, .threads = {}, .thread_hook = {}}, port};
    runtime.start();
    a.emplace(runtime.scheduler(0), runtime.scheduler(1), &wrong, &done_a);
    b.emplace(runtime.scheduler(1), runtime.scheduler(0), &wrong, &done_b);
    a->start();
    b->start();
    EXPECT_TRUE(eventually([&] { return done_a.load() == 1 && done_b.load() == 1; }));
    runtime.stop();
    runtime.join();

    EXPECT_EQ(wrong.load(), 0);
    for (const std::uint16_t shard : {std::uint16_t{0}, std::uint16_t{1}}) {
        // Through each inbox: the stop, the home hopper's returns and the other hopper's visits.
        EXPECT_EQ(runtime.counters(shard).inbox_received, 1 + 2 * rounds) << "shard " << shard;
        EXPECT_EQ(runtime.counters(shard).work_run, 1 + 2 * rounds) << "shard " << shard;
    }
}
```

- [ ] **Step 2: The steering test**

`tests/unit_tests/component/runtime/test_steering.cpp`:

```cpp
#include <aloe/fabric>
#include <aloe/runtime>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <echo_stack.hpp>
#include <frames.hpp>
#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;

    constexpr std::uint16_t queues = 4;
    constexpr std::size_t flows    = 256;
    constexpr auto patience        = 20s;
    constexpr aloe::device::MacAddress server{0x02, 0, 0, 0, 0, 0x01};
    constexpr aloe::device::MacAddress client{0x02, 0, 0, 0, 0, 0x02};
    constexpr aloe::device::Ipv4Address server_ip{10, 0, 0, 2};
    constexpr aloe::device::Ipv4Address client_ip{10, 0, 0, 1};
    constexpr std::size_t payload_offset = 14 + 20 + 8;  ///< Ethernet, IPv4 without options, UDP.

    [[nodiscard]] aloe::testing::Ipv4Spec spec_of(const std::size_t flow) {
        return {.destination_mac  = server,
                .source_mac       = client,
                .source           = client_ip,
                .destination      = server_ip,
                .source_port      = static_cast<std::uint16_t>(40000 + flow),
                .destination_port = 80,
                .protocol         = aloe::device::Ipv4Protocol::Udp};
    }

    /// Payload: the flow number, then four spare bytes the echo stamps the last two of.
    [[nodiscard]] std::vector<std::byte> frame_of(const std::size_t flow) {
        std::array<std::byte, 8> payload{};
        aloe::device::store_be32(std::span<std::byte>{payload}.first(4), static_cast<std::uint32_t>(flow));
        return aloe::testing::ipv4_frame(spec_of(flow), payload);
    }

}  // namespace

// Done-when 2, fabric half: every frame lands on the shard `queue_for` predicts, and comes back once.
TEST(Steering, EveryFrameIsAnsweredOnceByTheShardItsHashSelects) {
    aloe::fabric::Fabric fabric;
    auto& server_port = fabric.add_port({.mac = server, .queues = queues, .pool_size = 128});
    auto& client_port = fabric.add_port({.mac = client, .queues = 1, .pool_size = 64, .queue_depth = 4096});
    ASSERT_TRUE(server_port.steering().enabled);

    aloe::runtime::Runtime<aloe::fabric::Port, aloe::testing::EchoStack<aloe::fabric::Port>> runtime{
        {.shard = {.idle = aloe::runtime::IdlePolicy::Yield, .yield_after = 10}, .threads = {}, .thread_hook = {}},
        server_port};
    runtime.start();

    std::vector<std::uint16_t> expected(flows);
    std::array<std::size_t, queues> expected_per_shard{};
    for (std::size_t flow = 0; flow < flows; ++flow) {
        expected[flow] = aloe::device::queue_for(server_port.steering(), aloe::testing::flow_of(spec_of(flow)));
        ++expected_per_shard[expected[flow]];
        std::optional<aloe::fabric::Packet> packet;
        while (!(packet = client_port.allocate(0))) {
            std::this_thread::yield();
        }
        ASSERT_TRUE(aloe::testing::fill(*packet, frame_of(flow)));
        std::array<aloe::fabric::Packet, 1> burst{std::move(*packet)};
        ASSERT_EQ(client_port.transmit(0, burst), 1);
    }

    std::vector<int> seen(flows, 0);
    std::set<std::uint16_t> answering_shards;
    std::size_t received    = 0;
    const auto deadline     = std::chrono::steady_clock::now() + patience;
    while (received < flows && std::chrono::steady_clock::now() < deadline) {
        std::array<aloe::fabric::Packet, 16> burst;
        const std::size_t count = client_port.receive(0, burst);
        if (count == 0) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        for (std::size_t index = 0; index < count; ++index) {
            const std::span<const std::byte> data = burst[index].data();
            const std::uint32_t flow             = aloe::device::load_be32(data.subspan(payload_offset, 4));
            ASSERT_LT(flow, flows);
            ++seen[flow];
            const std::uint16_t stamp = aloe::testing::stamp_of(data);
            EXPECT_EQ(stamp, expected[flow]) << "flow " << flow << " was answered by the wrong shard";
            answering_shards.insert(stamp);
            burst[index] = aloe::fabric::Packet{};
        }
        received += count;
    }
    runtime.stop();
    runtime.join();

    EXPECT_EQ(received, flows);
    for (std::size_t flow = 0; flow < flows; ++flow) {
        EXPECT_EQ(seen[flow], 1) << "flow " << flow;
    }
    EXPECT_EQ(answering_shards.size(), queues) << "the 256 flows spread over every shard; widen the port range if not";
    for (std::uint16_t shard = 0; shard < queues; ++shard) {
        EXPECT_EQ(runtime.counters(shard).frames_received, expected_per_shard[shard]) << "shard " << shard;
        EXPECT_EQ(runtime.counters(shard).frames_transmitted, expected_per_shard[shard]) << "shard " << shard;
    }
}
```

Add both files to `Runtime.Threads` and link `Aloe::Component::Fabric`, `${SHARED_TESTING_TARGET}.Device`, `${SHARED_TESTING_TARGET}.Runtime` and `${SHARED_TESTING_TARGET}.Log` to that target; `test_cross_shard.cpp` registers the logging environment for the binary.

- [ ] **Step 3: Run under debug, then tsan, then asan**

```bash
cmake --preset debug > /dev/null && cmake --build --preset debug --target Aloe.Tests.Unit.Runtime.Threads && ctest --preset debug -R Runtime.Threads
cmake --build --preset tsan --target Aloe.Tests.Unit.Runtime.Threads && ctest --preset tsan -R Runtime.Threads
cmake --build --preset asan --target Aloe.Tests.Unit.Runtime.Threads && ctest --preset asan -R Runtime.Threads
```

Expected: pass everywhere. The cross-shard test takes a few seconds under TSan. If `answering_shards.size()` is below four, the fixed port range happens to miss a queue on this key and table; raise `flows` to 512 and note it.

- [ ] **Step 4: Format, commit**

```bash
clang-format -i tests/unit_tests/component/runtime/test_cross_shard.cpp tests/unit_tests/component/runtime/test_steering.cpp
./scripts/check-format.sh
git add tests/unit_tests/component/runtime
git commit -m "test(runtime): cross-shard hops and RSS steering on the fabric under ThreadSanitizer"
```

---

### Task 13: `ethdev::register_thread` and the ring integration test

Spec: "Ethdev addition", "Testing: Ethdev". Done-when 2 (ring half).

**Files:**
- Modify: `component/ethdev/eal/eal.hpp`, `component/ethdev/eal/eal.cpp`
- Modify: `docs/architecture/device.md` (ethdev key types)
- Create: `tests/integration_tests/runtime/CMakeLists.txt`, `tests/integration_tests/runtime/test_runtime_ring.cpp`
- Modify: `tests/integration_tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Runtime`, `EchoStack`, `EalEnvironment`, `probe_vdev`.
- Produces: `void aloe::ethdev::register_thread()`: registers the calling thread with DPDK, throws `EthdevError` on failure. Test target `Aloe.Tests.Integration.Runtime.Ring`.

- [ ] **Step 1: The failing test**

`tests/integration_tests/runtime/test_runtime_ring.cpp`:

```cpp
#include <aloe/ethdev>
#include <aloe/runtime>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <set>
#include <thread>
#include <utility>

#include <eal_environment.hpp>
#include <echo_stack.hpp>
#include <frames.hpp>
#include <gtest/gtest.h>
#include <logging_environment.hpp>
#include <rte_lcore.h>

namespace {

    using namespace std::chrono_literals;

    constexpr std::uint16_t queues = 4;
    constexpr auto patience        = 20s;
    constexpr aloe::device::MacAddress peer{0x02, 0, 0, 0, 0xfe, 0xed};

    const auto* const environment = ::testing::AddGlobalTestEnvironment(new aloe::testing::EalEnvironment{"net_ring0"});
    const auto* const logging     = ::testing::AddGlobalTestEnvironment(new aloe::testing::LoggingEnvironment{});

    using Queue = aloe::runtime::ShardQueue<aloe::ethdev::Port>;
    using Echo  = aloe::testing::EchoStack<aloe::ethdev::Port>;

    [[nodiscard]] bool eventually(const std::function<bool()>& condition) {
        const auto deadline = std::chrono::steady_clock::now() + patience;
        while (std::chrono::steady_clock::now() < deadline) {
            if (condition()) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return condition();
    }

    /// On its own shard: records the lcore id the hook gave the thread, injects one frame addressed
    /// to the port, and waits until the echo answered it and dropped the answer that came back.
    aloe::runtime::task<void> inject(Queue* queue,
                                     Echo* stack,
                                     aloe::runtime::Scheduler scheduler,
                                     std::atomic<unsigned>* lcore,
                                     std::atomic<int>* outcome) {
        lcore->store(rte_lcore_id());
        const auto frame =
            aloe::testing::ethernet_frame(queue->mac(), peer, aloe::testing::ethertype_experimental, aloe::testing::pattern(40));
        auto packet = queue->allocate();
        if (!packet || !aloe::testing::fill(*packet, frame) || !queue->transmit(std::move(*packet))) {
            outcome->store(-1);
            co_return;
        }
        for (int tick = 0; tick < 5000; ++tick) {
            if (stack->echoed() >= 1 && stack->dropped() >= 1) {
                outcome->store(1);
                co_return;
            }
            co_await scheduler.schedule_after(1ms);
        }
        outcome->store(-2);
    }

}  // namespace

// Done-when 2, ring half: each shard's transmit loops back onto its own queue, with real mbufs, on a
// thread the hook registered with DPDK, and nothing crosses between queues.
TEST(RuntimeRing, EachShardEchoesOnItsOwnQueueWithARegisteredThread) {
    aloe::ethdev::Port port{
        {.name = aloe::testing::probe_vdev("net_ring"), .queues = queues, .pool_size = 512}
    };
    aloe::runtime::Runtime<aloe::ethdev::Port, Echo> runtime{
        {.shard       = {.idle = aloe::runtime::IdlePolicy::Yield, .yield_after = 10},
         .threads     = {},
         .thread_hook = aloe::ethdev::register_thread},
        port};
    runtime.start();

    std::array<std::atomic<unsigned>, queues> lcores{};
    std::array<std::atomic<int>, queues> outcomes{};
    for (std::uint16_t index = 0; index < queues; ++index) {
        runtime.spawn(index,
                      inject(&runtime.shard(index).queue(),
                             &runtime.shard(index).stack(),
                             runtime.scheduler(index),
                             &lcores[index],
                             &outcomes[index]));
    }
    ASSERT_TRUE(eventually([&] {
        for (const auto& outcome : outcomes) {
            if (outcome.load() == 0) {
                return false;
            }
        }
        return true;
    }));
    runtime.stop();
    runtime.join();

    std::set<unsigned> distinct;
    for (std::uint16_t index = 0; index < queues; ++index) {
        EXPECT_EQ(outcomes[index].load(), 1) << "shard " << index << ": -1 could not inject, -2 never saw the echo";
        EXPECT_NE(lcores[index].load(), LCORE_ID_ANY) << "the hook registered the thread";
        distinct.insert(lcores[index].load());
        EXPECT_EQ(runtime.counters(index).frames_received, 2) << "shard " << index << ": the injected frame and its own answer";
        EXPECT_EQ(runtime.counters(index).frames_transmitted, 2) << "shard " << index;
        EXPECT_EQ(runtime.shard(index).stack().echoed(), 1) << "shard " << index;
        EXPECT_EQ(runtime.shard(index).stack().dropped(), 1) << "shard " << index;
    }
    EXPECT_EQ(distinct.size(), queues) << "every shard thread got its own lcore id";
}
```

`tests/integration_tests/runtime/CMakeLists.txt`:

```cmake
##############################################################################
# The runtime over the ring driver: the echo per queue with real mbufs and registered threads
##############################################################################
add_integration_test(${INTEGRATION_TESTING_TARGET}.Runtime.Ring
        test_runtime_ring.cpp
)
target_link_libraries(${INTEGRATION_TESTING_TARGET}.Runtime.Ring
        PRIVATE
        Aloe::Component::Runtime
        Aloe::Component::Ethdev
        ${SHARED_TESTING_TARGET}.Device
        ${SHARED_TESTING_TARGET}.Ethdev
        ${SHARED_TESTING_TARGET}.Runtime
        ${SHARED_TESTING_TARGET}.Log
        ${TEST_LIBS}
)
##############################################################################
```

Add `add_subdirectory(runtime)` to `tests/integration_tests/CMakeLists.txt`.

- [ ] **Step 2: See it fail**

```bash
cmake --preset debug > /dev/null && cmake --build --preset debug --target Aloe.Tests.Integration.Runtime.Ring 2>&1 | tail -5
```

Expected: `register_thread` is not a member of `aloe::ethdev`.

- [ ] **Step 3: `register_thread`**

In `component/ethdev/eal/eal.hpp`, after the `Eal` class:

```cpp
    /**
     * @brief Registers the calling thread with DPDK, so it gets an lcore id and mempool caches work.
     *
     * The runtime's thread hook for DPDK ports: `RuntimeConfig::thread_hook = aloe::ethdev::register_thread`.
     * Throws EthdevError when DPDK has no lcore slot left. Threads are not unregistered: shards live
     * as long as the process.
     */
    void register_thread();
```

In `component/ethdev/eal/eal.cpp`, add `#include <rte_lcore.h>` and:

```cpp
    void register_thread() {
        if (rte_thread_register() < 0) {
            throw EthdevError{detail::describe("rte_thread_register", rte_errno)};
        }
    }
```

- [ ] **Step 4: Build, run, document**

```bash
cmake --build --preset debug --target Aloe.Tests.Integration.Runtime.Ring && ctest --preset debug -R Runtime.Ring
cmake --build --preset asan --target Aloe.Tests.Integration.Runtime.Ring && ctest --preset asan -R Runtime.Ring
```

Expected: pass. If `lcores[index]` equals `LCORE_ID_ANY`, `rte_thread_register` succeeded but the id is read before registration took effect on this thread; it is read on the shard thread after the hook, so that would be a DPDK surprise worth recording. If the EAL rejects registration (`EINVAL`/`ENOMEM`), the test EAL arguments reserve too few lcores; `-l 0` leaves `RTE_MAX_LCORE - 1` slots for registered threads, so this should not happen.

`docs/architecture/device.md`, in the ethdev part of "Key types", add:

```markdown
- **`aloe::ethdev::register_thread()`** -- gives the calling thread a DPDK lcore id so per-lcore mempool
  caches work. The runtime's thread hook for DPDK ports.
```

- [ ] **Step 5: Format, commit**

```bash
clang-format -i component/ethdev/eal/eal.hpp component/ethdev/eal/eal.cpp tests/integration_tests/runtime/test_runtime_ring.cpp
./scripts/check-format.sh
git add component/ethdev docs/architecture/device.md tests/integration_tests
git commit -m "feat(ethdev): register shard threads with DPDK, and the runtime echo over the ring driver"
```

---

### Task 14: The tap manual test and the `ethernet_echo` example

Spec: "Testing: Manual", "Testing: Example". Done-when 2 (real card by hand).

**Files:**
- Create: `tests/shared/dpdk/packet_socket.hpp` (moved out of `test_ethdev_tap.cpp`)
- Modify: `tests/manual_tests/ethdev/test_ethdev_tap.cpp`, `tests/shared/CMakeLists.txt`
- Create: `tests/manual_tests/runtime/CMakeLists.txt`, `tests/manual_tests/runtime/test_runtime_tap.cpp`
- Modify: `tests/manual_tests/CMakeLists.txt`
- Create: `examples/ethernet_echo/CMakeLists.txt`, `examples/ethernet_echo/ethernet_echo.cpp`
- Modify: `examples/CMakeLists.txt`

**Interfaces:**
- Consumes: `Runtime`, `EchoStack`, `register_thread`, `EalEnvironment`.
- Produces: `aloe::testing::PacketSocket(std::string_view interface)`: `bool send(std::span<const std::byte>) const noexcept`, `std::optional<std::vector<std::byte>> receive(std::chrono::milliseconds) const`. Test target `Aloe.Tests.Manual.Runtime.Tap`. Example target `Aloe.Examples.EthernetEcho`.

- [ ] **Step 1: Share the packet socket**

Create `tests/shared/dpdk/packet_socket.hpp` with the `PacketSocket` class currently inside the anonymous namespace of `tests/manual_tests/ethdev/test_ethdev_tap.cpp`, moved verbatim into `namespace aloe::testing`, with two changes: `send` returns `bool` (`sent == static_cast<ssize_t>(frame.size())`) instead of using `ASSERT_EQ`, and the class gets a doc comment saying it is the kernel's end of a tap. Keep its includes (`<arpa/inet.h>`, `<linux/if_ether.h>`, `<net/if.h>`, `<netpacket/packet.h>`, `<poll.h>`, `<sys/socket.h>`, `<unistd.h>`, `<cerrno>`, `<cstring>`, `<chrono>`, `<optional>`, `<span>`, `<stdexcept>`, `<string>`, `<string_view>`, `<vector>`, and `<aloe/device>` for `MacAddress::size`). In `test_ethdev_tap.cpp` delete the class, include `<packet_socket.hpp>`, and change `ASSERT_NO_FATAL_FAILURE(kernel.send(inbound))` to `ASSERT_TRUE(kernel.send(inbound))`, with `aloe::testing::PacketSocket` as the type. Add the header to `${SHARED_TESTING_TARGET}.Ethdev` in `tests/shared/CMakeLists.txt`, and `Aloe::Component::Device` to its link libraries.

Build the moved test to make sure it still compiles:

```bash
cmake --preset debug > /dev/null && cmake --build --preset debug --target Aloe.Tests.Manual.Ethdev.Tap
```

- [ ] **Step 2: The runtime tap test**

`tests/manual_tests/runtime/test_runtime_tap.cpp`:

```cpp
#include <aloe/ethdev>
#include <aloe/runtime>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <eal_environment.hpp>
#include <echo_stack.hpp>
#include <frames.hpp>
#include <gtest/gtest.h>
#include <logging_environment.hpp>
#include <packet_socket.hpp>
#include <unistd.h>

// Needs CAP_NET_ADMIN: DPDK's tap driver creates a kernel interface. Run it as root, by hand:
//
//     sudo ./build/debug/tests/manual_tests/runtime/Aloe.Tests.Manual.Runtime.Tap
//
namespace {

    using namespace std::chrono_literals;

    constexpr std::string_view interface = "aloe-echo";
    constexpr aloe::device::MacAddress peer{0x02, 0, 0, 0, 0xfe, 0xed};
    constexpr auto patience = 2000ms;

    const auto* const environment =
        ::testing::AddGlobalTestEnvironment(new aloe::testing::EalEnvironment{"net_tap0,iface=aloe-echo"});
    const auto* const logging = ::testing::AddGlobalTestEnvironment(new aloe::testing::LoggingEnvironment{});

    /// What the echo makes of `frame`: addresses swapped, shard 0 stamped into the last two bytes.
    [[nodiscard]] std::vector<std::byte> echo_of(std::vector<std::byte> frame) {
        std::swap_ranges(frame.begin(), frame.begin() + 6, frame.begin() + 6);
        aloe::device::store_be16(std::span<std::byte>{frame}.last(2), 0);
        return frame;
    }

}  // namespace

TEST(RuntimeTap, TheKernelGetsItsFramesBackWithTheAddressesSwapped) {
    if (geteuid() != 0) {
        FAIL() << "this test creates a tap interface and needs CAP_NET_ADMIN; run it as root";
    }
    aloe::ethdev::Port port{
        {.name = "net_tap0", .queues = 1, .pool_size = 512}
    };
    aloe::runtime::Runtime<aloe::ethdev::Port, aloe::testing::EchoStack<aloe::ethdev::Port>> runtime{
        {.shard       = {.idle = aloe::runtime::IdlePolicy::Yield, .yield_after = 100},
         .threads     = {},
         .thread_hook = aloe::ethdev::register_thread},
        port};
    runtime.start();
    const aloe::testing::PacketSocket kernel{interface};

    for (std::uint8_t round = 0; round < 3; ++round) {
        const auto frame =
            aloe::testing::ethernet_frame(port.mac(), peer, aloe::testing::ethertype_experimental, aloe::testing::pattern(60, round));
        ASSERT_TRUE(kernel.send(frame));
        const auto expected = echo_of(frame);
        bool answered       = false;
        const auto deadline = std::chrono::steady_clock::now() + patience;
        while (!answered && std::chrono::steady_clock::now() < deadline) {
            const auto reply = kernel.receive(patience);  // the kernel also sends its own traffic; skip it
            answered         = reply.has_value() && *reply == expected;
        }
        EXPECT_TRUE(answered) << "round " << round;
    }

    runtime.stop();
    runtime.join();
    EXPECT_GE(runtime.counters(0).frames_transmitted, 3U);
}
```

`tests/manual_tests/runtime/CMakeLists.txt`:

```cmake
##############################################################################
# The runtime echo over a tap device against the kernel: needs CAP_NET_ADMIN, run by hand
##############################################################################
add_manual_test(${MANUAL_TESTING_TARGET}.Runtime.Tap
        test_runtime_tap.cpp
)
target_link_libraries(${MANUAL_TESTING_TARGET}.Runtime.Tap
        PRIVATE
        Aloe::Component::Runtime
        Aloe::Component::Ethdev
        ${SHARED_TESTING_TARGET}.Device
        ${SHARED_TESTING_TARGET}.Ethdev
        ${SHARED_TESTING_TARGET}.Runtime
        ${SHARED_TESTING_TARGET}.Log
        ${TEST_LIBS}
)
##############################################################################
```

Add `add_subdirectory(runtime)` to `tests/manual_tests/CMakeLists.txt`.

- [ ] **Step 3: The example**

`examples/ethernet_echo/ethernet_echo.cpp`. It carries its own stack, because a program showing how to write one is the point, and because examples do not depend on test helpers:

```cpp
// Runs the Ethernet echo on N shards of one DPDK port until interrupted, then prints the counters.
//
//     ethernet_echo <port> <queues> [-- <EAL arguments...>]
//
//     ethernet_echo net_tap0 1 -- --vdev=net_tap0,iface=aloe0      # a tap, as root
//     ethernet_echo 0000:03:00.0 4 -- -l 0 --file-prefix echo      # a real card
//
// Frames addressed to the port's MAC come back with the addresses swapped.

#include <aloe/core>
#include <aloe/device>
#include <aloe/ethdev>
#include <aloe/runtime>
#include <algorithm>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <pthread.h>
#include <signal.h>

namespace {

    /// The whole stack: answer frames addressed to me, swapping the addresses.
    class EchoStack {
    public:
        using Packet = aloe::ethdev::Packet;

        EchoStack(aloe::runtime::ShardContext& context, aloe::runtime::ShardQueue<aloe::ethdev::Port>& queue) noexcept
            : context_{&context}
            , queue_{&queue} {
        }

        void on_receive(std::span<Packet> burst) noexcept {
            for (Packet& packet : burst) {
                const std::span<std::byte> data = packet.data();
                if (data.size() < aloe::device::ethernet_header_size || !addressed_to_me(data)) {
                    continue;  // the shard frees what we leave
                }
                std::swap_ranges(data.begin(), data.begin() + 6, data.begin() + 6);
                if (!queue_->transmit(std::move(packet))) {
                    ++refused_;
                }
            }
        }

        [[nodiscard]] std::uint64_t refused() const noexcept {
            return refused_;
        }

    private:
        [[nodiscard]] bool addressed_to_me(const std::span<const std::byte> frame) const noexcept {
            aloe::device::MacAddress::Bytes destination{};
            std::ranges::copy(frame.first(aloe::device::MacAddress::size), destination.begin());
            return aloe::device::MacAddress{destination} == queue_->mac();
        }

        aloe::runtime::ShardContext* context_;
        aloe::runtime::ShardQueue<aloe::ethdev::Port>* queue_;
        std::uint64_t refused_ = 0;
    };

    struct Arguments {
        std::string port;
        std::uint16_t queues = 1;
        std::vector<std::string> eal;
    };

    [[nodiscard]] std::optional<Arguments> parse(const std::span<char*> argv) {
        if (argv.size() < 3) {
            return std::nullopt;
        }
        Arguments arguments;
        arguments.port = argv[1];
        const std::string_view queues{argv[2]};
        if (std::from_chars(queues.data(), queues.data() + queues.size(), arguments.queues).ec != std::errc{} ||
            arguments.queues == 0) {
            return std::nullopt;
        }
        arguments.eal.emplace_back("ethernet_echo");
        bool after_separator = false;
        for (char* argument : argv.subspan(3)) {
            if (!after_separator) {
                if (std::string_view{argument} != "--") {
                    return std::nullopt;
                }
                after_separator = true;
                continue;
            }
            arguments.eal.emplace_back(argument);
        }
        if (arguments.eal.size() == 1) {
            arguments.eal = {"ethernet_echo", "--in-memory", "--no-telemetry"};
        }
        return arguments;
    }

    /// Blocks SIGINT and SIGTERM for the calling thread and every thread it starts afterwards.
    [[nodiscard]] sigset_t block_termination_signals() {
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &signals, nullptr);
        return signals;
    }

}  // namespace

int main(int argc, char** argv) {
    const auto arguments = parse(std::span<char*>{argv, static_cast<std::size_t>(argc)});
    if (!arguments) {
        std::println(stderr, "usage: ethernet_echo <port> <queues> [-- <EAL arguments...>]");
        return EXIT_FAILURE;
    }
    const sigset_t signals = block_termination_signals();

    aloe::core::Logging logging{{.level = aloe::core::LogLevel::Info}};
    aloe::ethdev::Eal eal{arguments->eal};
    aloe::ethdev::Port port{{.name = arguments->port, .queues = arguments->queues}};
    std::println("{} ({}) with {} queues, MAC {}", arguments->port, port.driver_name(), port.queue_count(), port.mac().to_string());

    aloe::runtime::Runtime<aloe::ethdev::Port, EchoStack> runtime{
        {.shard = {}, .threads = {}, .thread_hook = aloe::ethdev::register_thread}, port};
    runtime.start();
    std::println("echoing on {} shards; interrupt to stop", runtime.shard_count());

    int signal = 0;
    sigwait(&signals, &signal);
    runtime.stop();
    runtime.join();

    for (std::uint16_t index = 0; index < runtime.shard_count(); ++index) {
        const auto& counters = runtime.counters(index);
        std::println("shard {}: {} ticks, {} frames in, {} out, {} refused, {} idle ticks",
                     index, counters.ticks, counters.frames_received, counters.frames_transmitted,
                     counters.transmit_refused + runtime.shard(index).stack().refused(), counters.idle_ticks);
    }
    return EXIT_SUCCESS;
}
```

`examples/ethernet_echo/CMakeLists.txt`:

```cmake
##############################################################################
# The Ethernet echo on N shards of one DPDK port: the runtime's living example
##############################################################################
add_executable(${EXAMPLES}.EthernetEcho
        ethernet_echo.cpp
)
target_link_libraries(${EXAMPLES}.EthernetEcho
        PRIVATE
        Aloe::Common::Core
        Aloe::Component::Runtime
        Aloe::Component::Ethdev
)
##############################################################################
```

Add `add_subdirectory(ethernet_echo)` to `examples/CMakeLists.txt`.

- [ ] **Step 4: Build everything, run the tap test if you have root**

```bash
cmake --build --preset debug
./build/debug/examples/ethernet_echo/Aloe.Examples.EthernetEcho 2>&1 | head -2   # prints usage, exit 1
sudo ./build/debug/tests/manual_tests/runtime/Aloe.Tests.Manual.Runtime.Tap          # only where sudo is available
```

Expected: the whole tree builds; the example prints its usage; the tap test passes when run as root. Without root, note in the PR that it compiled but did not run.

`aloe::ethdev::Eal` takes `std::span<const std::string>`; a `std::vector<std::string>` converts. If `Port` needs `pool_size` tuned for a real card, leave the default; the example is a demo.

- [ ] **Step 5: Format, commit**

```bash
clang-format -i tests/shared/dpdk/packet_socket.hpp tests/manual_tests/ethdev/test_ethdev_tap.cpp tests/manual_tests/runtime/test_runtime_tap.cpp examples/ethernet_echo/ethernet_echo.cpp
./scripts/check-format.sh
git add tests/shared tests/manual_tests examples
git commit -m "test(runtime): the echo over a tap by hand, and the ethernet_echo example"
```

---

### Task 15: Documentation

Spec: "Documentation". Done-when 5.

**Files:**
- Create: `docs/architecture/runtime.md`
- Modify: `docs/architecture/overview.md`, `docs/roadmap.md`, `docs/guides/getting-started.md`, `README.md`, `AGENTS.md`

**Interfaces:** none; prose only. Every name below must match the code as built; where a task changed a name during execution, the doc follows the code.

- [ ] **Step 1: `docs/architecture/runtime.md`**

```markdown
# Runtime Module

The runtime module (`component/runtime/`) is the shards: the unit everything above the device layer
runs in. A shard owns one queue of a device, a run queue, a cross-shard inbox, a timer wheel, a counting
scope and its counters, and runs a loop that polls the queue, fires due timers and runs ready work, all
on one thread. The module also holds the scheduler that gives senders a home on a shard, the shard-bound
coroutine task, and the runtime that launches one shard per device queue. Everything is reached through
the umbrella `#include <aloe/runtime>` (`export/aloe/runtime`), and targets link `Aloe::Component::Runtime`.
It depends on [`core`](core.md) for the execution facilities and logging, and on [`device`](device.md)
for the device concept. It never names DPDK.

## Key types

### The context and its primitives

- **`aloe::runtime::ShardContext`** -- everything of a shard that does not touch the device: the run queue,
  the inbox, the timer wheel, the task scope, the tick stamp and the counters. `run_once(now)` is its one
  verb: move the inbox onto the run queue, advance the wheel to `now`, run the work that was queued when
  the step began. `request_stop()` sets the stop flag and stops the scope; `drained()` is the loop's exit
  condition: stop requested, scope empty, both queues empty. `ShardContext::current()` names the context
  the calling thread is running.
- **`aloe::runtime::Work`** -- the intrusive node every unit of ready work is: a next pointer and a function.
  Operation states derive from it. **`RunQueue`** is the same-shard FIFO of them, with no synchronisation.
  **`Inbox`** is the multi-producer, single-consumer queue other threads push into: one atomic exchange per
  push, one load per pop, no allocation.
- **`aloe::runtime::Timer`, `TimerWheel`** -- an intrusive timer node and the hierarchical wheel it lives
  in: four levels of 256 slots, arm and cancel in constant time, swept by `advance(now)`. Deadlines are
  rounded up to the resolution, one millisecond by default, so a timer never fires early and is at most
  one resolution plus one tick late.
- **`aloe::runtime::TaskScope`** -- the per-shard counting scope, shaped like the standard's
  `counting_scope`: `spawn`, `request_stop`, `join`, with no atomics because one thread uses it. One task
  per connection will run in it. A spawned task's exception is logged and counted; it does not take the
  shard down.
- **`aloe::runtime::ShardCounters`** -- ticks, frames, work, timers and tasks, all monotonic, read on the
  shard's thread or after it has stopped.

### Scheduler and task

- **`aloe::runtime::Scheduler`** -- one pointer to a context, a value type, never type-erased. A stdexec
  scheduler with the timed shape added: `schedule()`, `schedule_after(duration)`, `schedule_at(time_point)`,
  `now()`. Same-shard `schedule()` pushes onto the run queue; from another thread it goes through the inbox.
  `spawn(sender)` starts work in the shard's scope. Not default-constructible, so a task started without a
  shard fails to compile.
- **`aloe::runtime::task<T>`** -- the C++26 coroutine task bound to a shard: `core::task<T, ShardEnvironment>`,
  where the environment names `Scheduler` as the scheduler type. Awaiting a shard sender costs nothing beyond
  the sender itself: no reschedule, no allocation. A child task awaited by a parent inherits scheduler and
  stop token.

### Shard and runtime

- **`aloe::runtime::ShardQueue<Device>`** -- one device queue as the stack sees it: `allocate()`,
  `transmit(packet)`, the device's facts. `transmit` appends to a bounded ring flushed at the end of every
  tick; when the ring is full and the device refuses, it returns false and the caller keeps the packet.
- **`aloe::runtime::IsStack`** -- what sits above a shard: a type constructed from the context and the queue
  with one function, `on_receive(span<Packet>)`, called on the shard thread with each burst. It moves out
  what it keeps; the shard frees the rest.
- **`aloe::runtime::Shard<Device, Stack>`** -- a context, a queue and the stack, with the tick: `step(now)`
  receives a burst, hands it to the stack, runs `run_once`, flushes the ring. `run()` ticks until drained.
  `ShardConfig` sets the wheel resolution, burst and ring sizes, and the idle policy, `Spin` or `Yield`.
- **`aloe::runtime::Runtime<Device, Stack>`** -- one shard per device queue, each on its own thread with an
  optional CPU to pin to and a hook that runs first (`ethdev::register_thread` for DPDK). `start`, `stop`
  from any thread, `join`; `scheduler(i)` and `spawn(i, sender)` from any thread; `counters(i)` after `join`.

## Usage

The whole of a stack that answers Ethernet frames, as `examples/ethernet_echo/ethernet_echo.cpp` has it:

```cpp
class EchoStack {
public:
    using Packet = aloe::ethdev::Packet;

    EchoStack(aloe::runtime::ShardContext& context, aloe::runtime::ShardQueue<aloe::ethdev::Port>& queue) noexcept
        : queue_{&queue} {}

    void on_receive(std::span<Packet> burst) noexcept {
        for (Packet& packet : burst) {
            auto data = packet.data();
            std::swap_ranges(data.begin(), data.begin() + 6, data.begin() + 6);  // destination <-> source
            std::ignore = queue_->transmit(std::move(packet));
        }
    }

private:
    aloe::runtime::ShardQueue<aloe::ethdev::Port>* queue_;
};

aloe::ethdev::Port port{{.name = "net_tap0", .queues = 4}};
aloe::runtime::Runtime<aloe::ethdev::Port, EchoStack> runtime{
    {.shard = {}, .threads = {}, .thread_hook = aloe::ethdev::register_thread}, port};
runtime.start();
```

Work on a shard is a task spawned through its scheduler. Timers are senders of that scheduler:

```cpp
aloe::runtime::task<void> heartbeat(aloe::runtime::Scheduler scheduler) {
    for (int beat = 0; beat < 10; ++beat) {
        co_await scheduler.schedule_after(std::chrono::milliseconds{100});
    }
}

runtime.spawn(0, heartbeat(runtime.scheduler(0)));  // from any thread: hops to shard 0 and spawns there
runtime.stop();                                     // every task parked on a timer completes stopped
runtime.join();
```

## Threading contract

Three rules make the data path lock-free; debug builds assert them.

1. Every member of a context, its scheduler's senders, its scope and its wheel runs on the owning thread,
   except `Inbox::push`, and therefore `schedule()` from another thread, `Runtime::stop` and `Runtime::spawn`.
2. A stop token handed to an operation on a shard is requested on that shard. The scope's stop source
   satisfies this because `Runtime::stop` arrives through the inbox.
3. Operations complete on the shard that owns them, so the receiver runs there too.
4. A shard task is affine to its shard. The task awaits every sender directly, with no reschedule, so it
   awaits only its own shard's senders and child tasks; another shard's `schedule()` is for operation
   states and `Runtime::spawn`. The scope asserts in debug builds that a task completes on the thread
   that spawned it.

## Design notes

**A direct call, not a receive sender.** The layer above gets frames by one call from the loop, run to
completion, and transmits by one call into the queue. A receive sender completing with a burst would put an
operation state and stdexec on the per-frame path. Senders begin one layer up, at the connection, where a
parked operation is worth an operation state.

**A device-free context.** The scheduler points at a `ShardContext`, not at a `Shard<Device, Stack>`, so the
scheduler, the timer senders and the task are one concrete type for every backend, and tasks never carry a
device type in their signatures. The context is tested without a device, and time is a stamp the loop passes
in, so a test drives the wheel and the drain with any clock it likes.

**One wheel per shard, never a timer per operation.** Timers are intrusive nodes swept once per tick. TCP
arms and re-arms them on every segment, which is why arm and cancel are constant-time and allocation-free.

**The task environment.** stdexec's P3552 task takes its scheduler type from its environment. With the
default environment the scheduler is type-erased and every await may reschedule through it. `ShardEnvironment`
names the concrete `Scheduler`, whose senders say they complete where they start, so the task awaits them
directly. The switch applies to every awaited sender, which is why threading contract rule 4 exists. A test
pins the saving: a task awaiting a timer costs one wheel entry and no run-queue push.

**Logging behind an alias.** Shards log only on cold paths, through [`core`](core.md)'s quill alias: a shard
starting and draining, a task failing, a hook or pin failing. Nothing logs inside a tick.
```

- [ ] **Step 2: `docs/architecture/overview.md`**

Under "Shards", replace the last line `Status: design. Arrives in phase 0.` with:

```markdown
Status: exists, see [runtime](runtime.md). One shard per device queue, one thread each, with the run loop,
the inbox and the timer wheel as described. A connection owning shard arrives with TCP in phase 1.
```

Under "Senders and receivers", replace the `Status:` paragraph with:

```markdown
Status: the scheduler, the timer senders, the counting scope, the stop plumbing and the shard-bound task
exist in [runtime](runtime.md), on the C++26 task type from P3552. The connection leaf senders, readiness
on receive, the two send completions and deadline stamps arrive with TCP in phase 1.
```

Under "Layers", after the first paragraph, add:

```markdown
A layer sits on a shard through one contract: it is constructed from the shard's context and its device
queue, and the shard calls its `on_receive` with every burst, on the shard's thread, run to completion.
Transmit is a call into the queue. Nothing about this contract names the asynchronous model, which is how
stdexec stays out of the protocol headers.
```

Update the "Layers" status line to say the runtime exists too: `Status: the device layer and the shard runtime exist, see [device](device.md) and [runtime](runtime.md). IPv4 and TCP arrive in phases 1 and 2, TLS in phase 3, HTTP/1.1 and WebSocket in phase 4.`

In the second paragraph of the page, change "Today the repository holds the build skeleton and the [`core`](core.md) module" to "Today the repository holds the build skeleton, the [`core`](core.md) and [`utils`](utils.md) modules, the [device layer](device.md) and the [shard runtime](runtime.md)".

- [ ] **Step 3: `docs/roadmap.md`**

Replace the three paragraphs under "## 0. Runtime core" with:

```markdown
Done. The repository skeleton: toolchain, dependencies through vcpkg, the `Aloe::Dpdk` target, the
[`core`](architecture/core.md) module, smoke tests and CI. The [device layer](architecture/device.md): the
packet and device concepts, the in-memory fabric and the DPDK backend, with one conformance suite that runs
against both without root or hugepages. The [shard runtime](architecture/runtime.md): core launch, run loop,
scheduler, run queue, cross-shard inbox, timer wheel and timer sender, counting scope, task type, stop
plumbing, counters and logging. Tasks and timers run on shards, work is submitted across shards, and an
Ethernet echo runs on several shards on both backends with every frame landing on the shard the hash selects,
verified on the fabric under ThreadSanitizer.
```

- [ ] **Step 4: `docs/guides/getting-started.md`, `README.md`, `AGENTS.md`**

`getting-started.md`, line 43: "Dependencies (DPDK, stdexec, quill, GoogleTest, and Google Benchmark for the benchmark targets)".

`README.md`: in the second paragraph, "The repository contains the build skeleton, the device layer and the shard runtime: the toolchain, the dependency setup, the packet and device abstraction with an in-memory backend for tests and a DPDK backend, and the shards that run everything above them. The stack itself is built in phases: a minimal TCP, full TCP with retransmission and congestion control, TLS, and finally HTTP/1.1 with WebSocket." Add a module row after `aloe::ethdev`:

```markdown
| `aloe::runtime` | `component/runtime/` | The shards: run loop, scheduler, timers, scope and task, and the runtime that launches one per queue. | [docs/architecture/runtime.md](docs/architecture/runtime.md) |
```

Re-align the table's columns with the wider namespace cell; `check-format.sh` does not check Markdown, so align by hand.

`AGENTS.md`:
- "Build and test", the done-when bullet: "A change is done when `debug`, `gcc-debug`, `asan` and `tsan` all build and pass, and the format check passes." Add after it: "PR CI builds the `ci` preset only, so the sanitizer and GCC runs are local."
- First paragraph: "The repository holds the build skeleton, the device layer ... and the DPDK backend (`ethdev`), and the shard runtime (`runtime`)."
- Layout table: in the `component/<module>/` row add `runtime` (shards, scheduler, timers, scope, task, runtime; no DPDK).
- Rules, "Execution facilities": add "and `aloe::core::TaskEnvironment` for a task bound to a concrete scheduler".
- Rules, new bullet after "Execution facilities": "**Logging.** Only `common/core/log/log.hpp` may name `quill::`. Everything else takes a `core::Logger` from `core::logger(name)` and writes `log.info<"text {}">(value)`. Nothing logs on a hot path."
- "Dependencies": vcpkg.json now lists quill; no change to the rule's text.
- "Traps already found": add the entries from "Read This First" that execution confirmed, at least these two: "`stdexec::get_completion_behavior` is deprecated and fails under `-Werror`; alias the `exec::` spelling in core" and "A type with a `std::atomic` member is immovable; `Work` is one, so containers of operation states use `std::unique_ptr` or `std::deque`." Also the two clang-tidy traps from "Read This First" (`readability-make-member-function-const`, `readability-convert-member-functions-to-static`) and the task trap: "A shard task awaits every sender directly, with no `affine` wrap; it awaits only its own shard's senders and child tasks." Add any new trap the tasks hit.

- [ ] **Step 5: Commit**

```bash
git add docs README.md AGENTS.md
git commit -m "docs: the runtime module page, and the overview, roadmap and README for phase 0 done"
```

---

### Task 16: Whole-branch verification and the pull request

Spec: "Done when". The reviewer's checklist for the final review.

- [ ] **Step 1: Every preset, from scratch**

```bash
for preset in debug gcc-debug asan tsan; do
    cmake --preset $preset > /dev/null && cmake --build --preset $preset && ctest --preset $preset || { echo "FAILED: $preset"; break; }
done
./scripts/check-format.sh
```

Expected: four green runs and `formatted correctly`. Record the test counts per label in the PR. PR CI runs the `ci` preset only, so these four runs are the sanitizer and GCC evidence; say so in the PR.

- [ ] **Step 2: The grep gates**

```bash
grep -rn 'stdexec::\|exec::' common component tests examples --include=*.hpp --include=*.cpp | grep -v 'common/core/execution/execution.hpp' | grep -v 'aloe::core::ex::' | grep -v 'core::ex::'
grep -rln 'quill::' common component tests examples --include=*.hpp --include=*.cpp
grep -rn '#include <aloe/core>' component/device component/fabric component/ethdev common/utils
grep -rn 'using namespace' common component --include=*.hpp --include=*.cpp
```

Expected: the first prints nothing; the second prints only `common/core/log/log.hpp`; the third and fourth print nothing.

- [ ] **Step 3: The "done when" list, item by item**

| Done when | Evidence |
|---|---|
| 1. Tasks and timers run on shards; cross-shard submission passes under tsan | `Aloe.Tests.Unit.Runtime` and `Aloe.Tests.Unit.Runtime.Threads` green under `tsan`, locally: CI does not run `tsan` |
| 2. Echo on several shards on both backends; steering checked on the fabric; ring per queue in CI; real card by hand | `Steering.*` and `RuntimeRing.*` green; `Aloe.Examples.EthernetEcho` built, run on a card or a tap if one was at hand, with the outcome in the PR |
| 3. A task awaiting a timer: one wheel entry, no run-queue push | `ShardTaskTest.ATaskAwaitingATimerCostsOneWheelEntryAndNoRunQueuePush` green |
| 4. No root, no hugepages, no card; four presets and the format check | Step 1 |
| 5. Documentation | Task 15, plus the Doxygen target if `BUILD_DOCS` is on: `cmake --build --preset debug --target docs` |

- [ ] **Step 4: Amend the spec**

In `docs/superpowers/specs/2026-10-02-shard-runtime-design.md`, under "Claims the prototype must prove", mark each claim with what execution showed, and add anything the tasks had to change. The spec is private and uncommitted; this is for the author.

- [ ] **Step 5: Open the pull request**

Title: `[RUNTIME][CORE][ETHDEV] Shard runtime: context, scheduler, timers, scope, task and the runtime`

Body, in the repository's short style:

```markdown
The second of the four steps to minimal TCP and the last piece of phase 0 (#4).

A device-free `ShardContext` owns the run queue, the multi-producer inbox, the hierarchical timer wheel,
the single-threaded counting scope and the counters; `run_once(now)` is its one verb. `Shard<Device, Stack>`
adds one device queue and the tick, `Runtime<Device, Stack>` launches one per queue on its own thread.
`Scheduler` is a value type around the context; `runtime::task<T>` is the P3552 task with that scheduler in
its environment, so awaiting a shard sender never reschedules. Core's `task` moves to `stdexec::task`, and
quill arrives behind a macro-free alias in core. Ethdev gains `register_thread` for the shard threads.

Verified: `debug`, `gcc-debug`, `asan`, `tsan` and the format check; the inbox, cross-shard and steering
tests under ThreadSanitizer; the echo over `net_ring` with four queues; the tap echo as root: <ran | compiled only>.
```

Closes #4.
