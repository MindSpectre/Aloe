# C++ Modules Experiment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> Tasks 3 and 5 run several agents at once; see "Running a wave".

**Goal:** Find out whether Aloe's modules can become C++20 named modules under clang 22, with no change in
behaviour or generated hot-path code, and report what it costs and what breaks.

**Architecture:** Each Aloe module becomes one named module `aloe.<module>`. Each of its headers becomes a partition
`aloe.<module>:<file stem>`, and a primary interface re-exports every partition, exactly as the umbrella header
includes every header today. The umbrella header `<aloe/<module>>` shrinks to `import aloe.<module>;`. Code that
still includes umbrellas therefore keeps compiling, and modules convert one at a time, in dependency order, with
modules of the same layer converted in parallel. The runtime and `execution` stay headers unless the optional Task 9
runs.

**Tech Stack:** clang 22.1.8, clang-tidy 22.1.8, CMake 4.3.0 (`CXX_MODULES` file sets, `CMAKE_CXX_SCAN_FOR_MODULES`),
Ninja 1.13.2, mold, vcpkg (stdexec, quill, DPDK, gtest).

**Spec:** none written; this plan stands on the stage 0 findings, recorded in
`docs/superpowers/experiments/modules/issues/README.md` and summarised under "Findings this plan rests on".

## Findings this plan rests on

Each was reproduced on this machine on 2026-11-02/03:

1. **GCC is out.** GCC 16.1.1 crashes in modules mode on any file that contains stdexec's text, whether or not the
   file uses modules (ledger 01, GCC PR 128318). The experiment builds with clang only.
2. **The umbrella can be one line.** A header holding only `import aloe.x;`, included before or after standard
   headers, builds and links under clang with Aloe's warning flags and `.clang-tidy`, and so does an exported
   `detail` namespace inside partitions. So does a file that `#include`s stdexec and imports modules.
3. **Module code isn't inlined by default.** Inside a named module, a member function defined in its class is not
   implicitly inline. At `-O3` the importer calls it, and inlines it only once it is declared `inline`.
4. **ccache serves stale objects** for files that import a clang named module (ledger 04), so no compiler launcher
   is used.
5. **Three things need silencing:**
   - `-Wunused-command-line-argument`, tripped by CMake's command that builds only a module's compiled interface
     (ledger 05);
   - clang-tidy's `readability-redundant-declaration`, which fires in every importer (ledger 02);
   - `-WTU-local-entity-exposure`, only where stdexec sits inside a module (ledger 06, Task 9).

## Global Constraints

- No functionality change. Only these may change:
  - include lines, module declarations, `import`, `export`;
  - `inline` added under the inline rule below;
  - file renames from `.hpp` to `.cppm`;
  - CMake target structure, presets and build scripts;
  - test include lines.

  No name, signature, behaviour, default or comment changes, except doc comments that name a file that no longer
  exists.
- Compiler: `/usr/bin/clang++` 22.1.8. A GCC configure stops with `FATAL_ERROR` naming GCC PR 128318.
- No compiler launcher: `CMAKE_CXX_COMPILER_LAUNCHER` and `CMAKE_C_COMPILER_LAUNCHER` are empty in every preset.
- `CMAKE_CXX_SCAN_FOR_MODULES` is `ON` globally.
- vcpkg comes from the main checkout: every configure runs with `VCPKG_ROOT=/home/eclipse/Workspace/Aloe/vcpkg` in
  its environment.
- CMake 4.4.4, not the system's 4.3.0, from Task 3 on: `cmake` and `ctest` are
  `/tmp/claude-1000/-home-eclipse-Workspace-Aloe/402db02e-fdf1-41ad-97d2-bfaf5f4d4c84/scratchpad/cmake-venv/bin/cmake`
  and `.../cmake-venv/bin/ctest`. CMake 4.3.0 hands a consumer that reaches a module library through an INTERFACE
  library a different BMI than the one its imported module was built against, and clang crashes on the mismatch
  (ledger 08). A build directory configured with one CMake is never reused with the other.
- A task is done when `debug`, `asan` and `tsan` build and `ctest` passes in each, and `./scripts/check-format.sh`
  passes. `gcc-debug` is not part of the gate on this branch.
- Module names are `aloe.<module>`; partition names are `aloe.<module>:<file stem>` (`aloe.wire:tcp_sequence`).
  The primary interface is `<module dir>/<module>.cppm` (`common/wire/wire.cppm`). Each partition file stays in the
  folder its header was in, renamed from `x.hpp` to `x.cppm`.
- Target names and aliases do not change (`Aloe::Common::Wire`), so consumers' `target_link_libraries` stay as
  they are.
- Every problem met that belongs to a tool or library goes into `docs/superpowers/experiments/modules/issues/` as a
  new numbered draft with a ledger row, before any workaround for it is committed.
- Commits: `refactor(<module>): ...` for conversions, `build: ...` for CMake and presets, `docs: ...` for the
  ledger and report. The branch is `experiment/modules`, worktree
  `/home/eclipse/Workspace/Aloe/.claude/worktrees/modules`.

## The conversion recipe

Every module conversion (Tasks 2 to 9) follows this recipe. Task 2 amended it (rules 3 to 7, 9 and 10, after its review); later tasks follow the text as it stands.

1. **Partitions.** `x.hpp` becomes `x.cppm`:

   ```cpp
   module;
   #include <cstdint>              // every standard and third-party include of the old header, and nothing else
   export module aloe.wire:tcp_sequence;
   import :bytes;                  // was #include <bytes.hpp>, same module
   import aloe.core;               // was #include <aloe/core> or a bare include of a core header

   export namespace aloe::wire {
       // the old namespace body, unchanged, nested `detail` included
   }  // namespace aloe::wire
   ```

   `#pragma once` goes. Nothing follows `export module` except imports and the exported namespace.
   - **Implementation partitions:** a partition that only other partitions use, and no consumer, is still exported
     from the primary interface; visibility doesn't change.
   - **Imports between modules:** imports of other modules are not re-exported (`import`, not `export import`). A
     consumer that needs both modules already includes both umbrellas.
2. **Primary interface.** `<module dir>/<module>.cppm` holds `export module aloe.<module>;` and one
   `export import :<stem>;` per partition, in the order the umbrella listed the headers.
3. **Implementation files.** `x.cpp` stays `x.cpp`: a global module fragment with its includes, then
   `module aloe.<module>;`. It drops its include of its own header, since the primary interface is imported
   implicitly. Before dropping it, check that the `.cpp`'s own fragment includes every header for the names the
   `.cpp` uses directly. Declarations in the interface's fragment are reachable from the `.cpp`, but not visible to
   it. Task 2 example: `tsc_clock.cpp` gets `timespec` and `clock_gettime` only transitively, through `<chrono>`.
   An implementation unit that used another module's umbrella imports that module after its `module aloe.<m>;`
   line; the umbrella cannot go in its fragment, since the umbrella ends in an `import` (amended after Task 5).
4. **The umbrella.** `<module>/export/aloe/<module>` keeps its doc comment. Its body is, in order:
   1. `#pragma once`;
   2. the umbrella of every module its interface units import, and no other (`<aloe/core>` only when they import
      `aloe.core`; amended after Task 4, where `device` imports only `aloe.wire`);
   3. one comment line citing ledger 07;
   4. the sorted union of every standard, system and third-party `#include` in the module's interface units' global
      module fragments, each under the same `#if` it has there. Count `.cppm` files only, not `.cpp` implementation
      units, but follow the includes transitively through any header-module header a fragment includes (rule 8);
   5. `import aloe.<module>;`.

   Why: clang 22.1.8 rejects libstdc++ 16's locale templates in a non-module file that includes `<ios>`,
   `<iostream>` or `<format>` after importing a module with partitions (ledger 07). This hit 21 files in the pilot.
   A module imported only indirectly triggers it too, so the umbrellas of a module's dependencies come first.

   The list must stay a superset of the fragments: a partition that gains an include adds it to the umbrella too.
   A missing header fails silently until some consumer includes `<ios>`, so `include-census.py --umbrellas` checks
   each umbrella's list against its fragments (Task 2b).

   Third-party headers in this list (quill for `log`, stdexec for `runtime`) are parsed textually again in every
   consumer. That cost is measured in Task 10, not avoided.
5. **Inline rule.** Every non-template function defined in a `.cppm` gets an explicit `inline`. This covers free
   functions, and member functions, constructors and operators defined inside a class body. It exempts:
   - `constexpr` and `consteval` functions, which are implicitly inline;
   - `= default` and `= delete`;
   - members of class templates, and function templates.

   Out-of-class definitions in a `.cpp` stay as they are. The rule keeps today's inlining: without it, every small
   member becomes a call.

   Put `inline` immediately before the return type, and keep the other specifiers where they are:
   - `[[nodiscard]] static inline T f()`;
   - `[[nodiscard]] inline T f() const`;
   - `inline X(...) noexcept` for constructors, private ones included.

   `.clang-format` has `QualifierAlignment: Leave` and does not reorder them. Never enable clang-tidy's
   `readability-redundant-inline-specifier`: it would flag exactly these `inline`s, which a named module requires
   (ledger 03).

   `[[gnu::always_inline]]` is allowed, but only together with `inline`. Stage 0 measured both at `-O3`:
   - without `inline`, the attribute does nothing across the module boundary: a free function still compiles to a
     `jmp` and a member to a `call`, because the importer never sees the body;
   - with `inline`, both are inlined.

   Add the attribute only where the call comparison or the assembly shows a hot function still called out of line
   after `inline`. When in doubt about what the compiler does, read the assembly rather than guess: run
   `objdump -d -C --no-show-raw-insn` on the release object of the importer.
6. **CMake.** A module's sub-libraries (`Aloe.Common.Wire.Bytes`, `.Ethernet`, ...) fold into its one library.
   - Make it `STATIC` even when the module has no `.cpp`: its module units are compiled, and a library with no
     sources of its own cannot hold a `CXX_MODULES` file set.
   - Attach every `.cppm`: `target_sources(<lib> PUBLIC FILE_SET modules TYPE CXX_MODULES BASE_DIRS <module dir>
     FILES ...)`.
   - Keep the `export/` include directory for the umbrella, and drop the per-folder include directories.
   - Keep the `Aloe::<Group>::<Module>` alias on the library.
   - Turn each former sub-library's `target_link_libraries` into `PUBLIC` links of the module library.
   - **Generated units.** A generated module unit is configured from `<folder>/<stem>.cppm.in` to
     `${CMAKE_CURRENT_BINARY_DIR}/generated/<stem>.cppm`. List it in `FILES` by that absolute path, and add
     `${CMAKE_CURRENT_BINARY_DIR}/generated` to `BASE_DIRS`. Its old include directory goes.
   - **The umbrella.** List `export/aloe/<module>` as a plain source of the library, as `add_combined_library` did,
     so IDEs still show it.
   - **Checking clang-tidy.** `CODE_CHECK` lines in `build.ninja` confirm that clang-tidy runs on the `.cppm` units.
7. **Tests and other consumers.** A test or helper that included a header of the converted module by its bare name
   now includes the umbrella. Nothing else in tests changes.

   The umbrella re-includes the module's standard headers (rule 4), so consumers keep the headers they used to get
   transitively, and no consumer include changes for that reason. Without that list, `<chrono>` no longer reached
   consumers: in Task 2, `fixtures/scenario/script/script.hpp`'s defaulted `operator==` over `core::Duration` became
   implicitly deleted.
8. **When a module cannot convert.** If a module cannot convert without breaking Global Constraint 1, stop: leave
   it a header module, keep its umbrella textual, and record why. One example is `ethdev`, whose inline code calls
   DPDK's `static inline` functions, which a module may not expose. Recording the reason is part of the
   experiment's result, not a failure of the task. Measured in Task 5: clang 22.1.8 reports
   `-WTU-local-entity-exposure` (on by default, fatal under `-Werror`) for nine DPDK `static inline` functions
   called from exported inline members of `ethdev::Packet` (headroom, tailroom, prepend, append, adj, trim,
   refcnt_update, refcnt_read, pktmbuf_free), so `ethdev` stays a header module.

   A fallback anywhere in the graph is safe, verified in stage 0 (probe `p4`, `upper`). A converted module may
   include a header module's headers in its global module fragment even though those headers include umbrellas
   that are `import` shims. clang builds it, and the importer links and runs. So a fallback of `log`, `loop`, `net`
   or `tcp` does not block the waves after it.
9. **Documentation.** Doc comments that name a file the conversion removed are updated in the same commit (Global
   Constraint 1 allows it). `docs/architecture/*.md` and `codestyle.md` text that describes the old layout is left
   alone. Task 10's report lists it, since it changes only if the experiment lands.
10. **Verification builds.** Every gate run that closes a task starts from an empty build directory:
    `rm -rf build/<preset>`, then configure, build and `ctest`. `--clean-first` keeps the configure cache and
    proves less (Review Focus, stale incremental builds). Commits follow the Global Constraints' subjects, and a
    change to `scripts/experiment/` goes in a `build:` commit of its own, not inside a `refactor(<module>)` commit.

## Running a wave

Tasks 3 and 5 list several modules that do not depend on each other. Each module of a wave gets one agent, in a
worktree of its own.

Agents cannot share one worktree. Every build directory's `build.ninja` depends on every `CMakeLists.txt` in the
tree. When one agent renames `x.hpp` to `x.cppm` before updating its file list, every other agent's next build
reconfigures and fails with "Cannot find source file".

- **Worktrees.** Before dispatching, the lead creates one worktree per module from the current head of
  `experiment/modules`:

  ```bash
  git worktree add /home/eclipse/Workspace/Aloe/.claude/worktrees/wave-<module> -b experiment/modules-<module> experiment/modules
  ```

  The lead also configures each one:

  ```bash
  VCPKG_ROOT=/home/eclipse/Workspace/Aloe/vcpkg cmake --preset debug
  ```

  This happens before dispatch because vcpkg takes a lock on `VCPKG_ROOT`, so concurrent first configures would run
  one after another anyway. If the harness refuses to create the worktrees, the wave runs its modules one after
  another in the experiment worktree.
- **Scope.** An agent edits only its own module's folder and its own tests (`tests/*/<group>/<module>/`,
  `tests/*/<module>/`). It builds and tests its whole worktree in `debug`. Its dependencies are modules converted in
  earlier waves.
- **Shared files.** An agent never edits the root `CMakeLists.txt`, `cmake/`, `.clang-tidy`, `tests/shared/`,
  another module or the ledger. It reports the change it needs, and a tool problem as a ledger draft in its final
  message.
- **Commits.** Each agent commits its module once, on its own branch: `refactor(<module>): <module> is the module
  aloe.<module>`. After the wave, the lead:
  1. cherry-picks each commit onto `experiment/modules`; the paths are disjoint, so no conflict is expected;
  2. builds and tests the whole tree in `debug`, `asan` and `tsan` from fresh build directories;
  3. runs the call comparison;
  4. records new ledger drafts;
  5. removes the wave's worktrees and branches.

The agent brief for a wave task is the recipe, the Global Constraints, the task's own section, and this section.

## Review Focus

- **Hot path loses inlining.** A missed `inline` turns a small helper into a call on the receive or send path. No
  test catches it. The call comparison (Task 1 Step 6) runs after every wave, and a new call in a release binary is
  a failed task.
- **Stale incremental builds.** A wrong module dependency in CMake can leave an importer compiled against an old
  interface, the same symptom ccache showed. Each wave is verified from fresh build directories, and Task 10 checks
  an incremental rebuild after touching one partition.
- **Exposure through third-party inline code.** DPDK's `static inline` functions (`ethdev`) and quill's internals
  (`log`) may be TU-local entities that an exported inline function exposes. The ethdev and log tasks stop at the
  first such diagnostic and report it, rather than moving code out of line.
- **The clock's compile definition.** `ALOE_USE_TSC_CLOCK` selects `core::Clock` inside a module unit. A build with
  `-DUSE_TSC_CLOCK=OFF` must still configure, build and pass, as the `ci` preset does on `main`. Task 2 runs it.
- **Tests that reach into `detail`.** Tests use `tcp::detail`, `fabric::detail` and others. Exporting the whole
  namespace keeps them compiling; the build of each module's tests is the check.

---

### Task 0: Every cross-module include goes through the umbrella, under `main`'s full gate

The rule from the author: same module `#include <file.hpp>`, same file `#include "file.hpp"`, another module
`#include <aloe/module>`. It is broken in about 120 places today. This task runs before any experiment change,
under `main`'s own gate with `gcc-debug` included. It therefore goes to `main` as one PR whatever the experiment
concludes.

**Files:**
- Create: `scripts/experiment/include-census.py`
- Modify: every `.hpp` and `.cpp` under `common/` and `fixtures/` that the census lists with a bare cross-module
  include, and the tests that include `clock.hpp`, `tsc_clock.hpp` or `tsc_scale.hpp` directly. Also each module's
  `CMakeLists.txt` where a sub-library now includes an umbrella whose library it does not link.

**Interfaces:**
- Produces:
  - `scripts/experiment/include-census.py` prints, per module: bare same-module includes, bare cross-module
    includes, umbrella includes, imports, and the lines of `.hpp` plus `.cppm`. A bare include belongs to the
    module whose folder holds a file of that name.
  - No bare include names another module's file. Task 2 onward relies on this, because a converted module's file
    names stop existing.

- [ ] **Step 1: Write the census** and save its output as
  `docs/superpowers/experiments/modules/baseline/census-main.txt`. Expected: about 120 bare cross-module includes,
  most of them in `tcp`, `runtime`, `net` and `fabric`.
- [ ] **Step 2: Replace the includes.** In each file, replace every bare include of another module's header with
  that module's umbrella, one umbrella per module. Place it among the other `<aloe/...>` includes in the order
  `codestyle.md` gives for includes.
- [ ] **Step 3: Link the umbrellas.** Add a `target_link_libraries` entry wherever a sub-library now includes an
  umbrella of a module it does not link directly. Link the alias, `Aloe::Common::<Module>`.
- [ ] **Step 4: Verify under `main`'s gate.** Expected: the census shows zero bare cross-module includes. Then, for
  each of `debug`, `gcc-debug`, `asan`, `tsan`:
  `VCPKG_ROOT=/home/eclipse/Workspace/Aloe/vcpkg cmake --preset <p> && cmake --build --preset <p> && ctest --preset <p>`,
  followed by `./scripts/check-format.sh`. Expected: every build succeeds and every test passes: 58 in `debug`,
  `gcc-debug` and `asan`, 45 in `tsan`.
- [ ] **Step 5: Commit** the include changes alone, without the census script:
  `refactor: other modules' code comes in through their umbrella headers only`. Commit the script separately as
  `build: the include census of the modules experiment`, so the first commit cherry-picks onto `main` cleanly.

### Task 1: The branch builds with module scanning on, clang only, and the baselines are recorded

**Files:**
- Modify:
  - `CMakeLists.txt:11-14` (scan `ON`, the GCC refusal) and `CMakeLists.txt:80-89` (the warning flag);
  - `CMakePresets.json` (launchers empty in `linux-base`);
  - `.clang-tidy` (drop `readability-redundant-declaration`);
  - `scripts/check-format.sh:28,48` (accept `.cppm`);
  - `AGENTS.md` (the "No C++ modules" rule and the gate, on this branch only).
- Create: `scripts/experiment/aloe-calls.sh`, and `docs/superpowers/experiments/modules/baseline/` (calls, census,
  build times).

**Interfaces:**
- Produces: `scripts/experiment/aloe-calls.sh <binary>...` prints one line per distinct call target, `<count>
  <demangled name>`, sorted by name, summed over all the binaries given. It:
  - takes the target of every `call` and `jmp` instruction that names a symbol;
  - drops targets with an offset (`<name+0x40>`), which are jumps within a function, not calls;
  - drops `initializer for module ...`;
  - strips the module attachment `@aloe\.[a-z_]+`. Stage 0 showed it is the primary module's name, never the
    partition's: `advance@probe.wire`, `TcpSequence@probe.wire::next`;
  - keeps names that start with `aloe::`.

- [ ] **Step 1: Turn scanning on and refuse GCC.** In `CMakeLists.txt`:
  - replace the `CMAKE_CXX_SCAN_FOR_MODULES OFF` block with `set(CMAKE_CXX_SCAN_FOR_MODULES ON)`;
  - give it a comment naming the experiment;
  - add `if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU") message(FATAL_ERROR "...") endif ()`, with text that names GCC PR
    128318 and ledger 01.
- [ ] **Step 2: Silence the CMake warning.** Append `-Wno-unused-command-line-argument` to
  `general_compile_options` inside the existing `Clang` branch, with a comment naming ledger 05.
- [ ] **Step 3: Adjust the tools.**
  - In `.clang-tidy`, delete the `readability-redundant-declaration,` line and add a YAML comment above `Checks`
    naming ledger 02.
  - In `CMakePresets.json` `linux-base`, set `CMAKE_CXX_COMPILER_LAUNCHER` and `CMAKE_C_COMPILER_LAUNCHER` to `""`.
  - In `scripts/check-format.sh`, accept `*.cppm` at line 28 and in the `find` at line 48.
  - In `AGENTS.md`, replace the "No C++ modules" rule with one line saying this branch builds modules with clang
    only (ledger 01), and drop `gcc-debug` from the gate.
- [ ] **Step 4: Confirm clang-format reads `.cppm`.** Write a throwaway `.cppm` with `export module x;` and a
  misformatted namespace into the scratchpad. Run `clang-format --dry-run --Werror` on it. Expected: it reports the
  namespace. If it treats the file as something other than C++, add `--assume-filename=x.cpp` to the script for
  `.cppm` files.
- [ ] **Step 5: Build and test with scanning on.** For each of `debug`, `asan`, `tsan`:
  `VCPKG_ROOT=/home/eclipse/Workspace/Aloe/vcpkg cmake --preset <p> && cmake --build --preset <p> && ctest --preset <p>`.
  Expected: every build succeeds and every test passes: 58 in `debug` and `asan`, 45 in `tsan`. Record the wall
  time of a clean `debug` build in `baseline/build-times.md`. Also confirm that `cmake --preset gcc-debug` stops
  with the `FATAL_ERROR`.
- [ ] **Step 6: Record the baselines.**
  - Build `release` with the Step 5 command. Run `aloe-calls.sh` on every executable under
    `build/release/tests/unit_tests/` and on the tick-to-trade binary (`find build/release -name
    'Aloe.Benchmarks.TickToTrade' -type f`), writing to `baseline/calls.txt`.
  - Run the census into `baseline/census.txt`.

  Expected: `calls.txt` is non-empty, and lists no target with `+0x` or `initializer for module`.
- [ ] **Step 7: Commit** `build: module scanning on, clang only, no compiler launcher; the call-comparison script`.
  `docs/superpowers/` is ignored by version control, so the baselines stay local.

**The call comparison**, which every later task runs:
1. Rebuild `release`, configured with `-DDO_BENCHMARKS=ON` (the preset leaves benchmarks off), and run `aloe-calls.sh` over the binary set `baseline/calls.txt`'s header names: every executable under `build/release/tests/unit_tests/` plus `build/release/benchmarks/tick_to_trade/tick_to_trade`. Use `aloe-calls.sh` as of 634af6e or later, which strips every `@aloe.<module>` attachment in a name. Pass the binaries by name: a glob over `build/release/tests/unit_tests/*/` also matches `CMakeFiles` directories, and objdump stops on them.
2. Join the result with `baseline/calls.txt` by name.
3. It passes when no target's count is higher than its baseline count and no target is new. A lower count is
   fine: it means more inlining.
4. For each target that fails, add `inline` under recipe rule 5 until it passes. If the target is a function that
   was already out of line in the baseline, record it and its new callers in the task's commit message instead.

### Task 2: `core` becomes `aloe.core` (pilot, run by the lead)

The pilot settles the recipe on the smallest module that has a `.cpp`, a generated header and a compile-time
switch. Amend "The conversion recipe" with anything learned before starting Task 3.

**Files:**
- Rename: `common/core/clock/{clock,tsc_scale,tsc_clock}.hpp`, `values/values.hpp`, `guards/guards.hpp` and
  `fixed_string/fixed_string.hpp` to `.cppm`.
- Rename: `common/core/version/version.hpp.in` to `version.cppm.in`, which generates
  `${CMAKE_CURRENT_BINARY_DIR}/generated/version.cppm` as partition `aloe.core:version`.
- Create: `common/core/core.cppm`.
- Modify: `common/core/clock/tsc_clock.cpp` (implementation unit), `common/core/CMakeLists.txt`,
  `common/core/export/aloe/core`.
- Modify: the tests that included core headers directly, if Task 0 left any.

**Interfaces:**
- Produces: module `aloe.core` exporting everything `<aloe/core>` exposed: `core::Clock`, `core::TimePoint`,
  `core::Duration`, `core::calibrate_clock`, `core::TscClock`, `core::TscScale`, `core::force_non_const`,
  `core::FixedString`, and the version constants under their current names.

- [ ] **Step 1: Convert core.** Convert under the recipe. List the generated `version.cppm` in the file set with its
  binary directory among `BASE_DIRS`.
- [ ] **Step 2: Build core.** Run `cmake --build --preset debug --target Aloe.Common.Core`. Expected: it builds.
  Then build the whole tree. Expected: it builds; every other module still includes `<aloe/core>`, which now imports.
- [ ] **Step 3: Run the gate** (Task 1 Step 5). Expected: all pass, with the same test counts.
- [ ] **Step 4: Check the clock switch.** Configure `build/tsc-off` with `cmake --preset debug -B build/tsc-off
  -DUSE_TSC_CLOCK=OFF`, then build and run `ctest --test-dir build/tsc-off -LE manual`. Expected: all pass, with the
  TSC tests skipped as on CI.
- [ ] **Step 5: Run the call comparison** (Task 1). Expected: it passes. No target's count rises and no target is new;
  `inline` is added until that holds.
- [ ] **Step 6: Amend the recipe** with what the pilot needed and the plan did not say. Then commit
  `refactor(core): core is the module aloe.core`.

### Task 2b: The census checks each umbrella's standard-header list against its module's fragments

Added after Task 2's review. Rule 4's list fails silently when it misses a header, so it gets a mechanical check
before any other module copies the pattern.

**Files:**
- Modify: `scripts/experiment/include-census.py`

**Interfaces:**
- Produces: `include-census.py --umbrellas`. For every module with a `<module dir>/<module>.cppm`, it compares:
  - **the expected list:** the sorted union of the `#include` lines between `module;` and `export module` in that
    module's `.cppm` files, with any `#if`/`#endif` they sit under. It follows quoted or bare includes of header-module
    headers transitively and skips `<aloe/...>` umbrellas;
  - **the actual list:** the `#include` lines of standard, system and third-party headers in
    `export/aloe/<module>`, before its `import`.

  It prints `ok <module>`, or `MISSING <module> <header>` and `EXTRA <module> <header>`. It also checks that the
  umbrella includes `<aloe/<dep>>` for every `import aloe.<dep>;` in the module's `.cppm` files, printing
  `MISSING-UMBRELLA <module> <dep>` when one is absent. It exits 1 if anything is missing; extras are reported but
  do not fail.

- [ ] **Step 1: Implement `--umbrellas`** in the existing script, without changing its default output.
- [ ] **Step 2: Verify.** Run `python3 -I scripts/experiment/include-census.py --umbrellas`. Expected: `ok core`
  and exit 0. Then delete one header (say `<ratio>`) from a scratch copy of `common/core/export/aloe/core`, point the
  check at it, and confirm it prints `MISSING core <ratio>` and exits 1. A temporary edit of the real file, reverted
  before committing, is fine too.
- [ ] **Step 3: Commit** `build: the include census checks each umbrella's header list against its module`.

### Task 3: Wave 1, `wire`, `stream`, `log` (three agents)

Dependencies: `wire` has none; `stream` and `log` need only `core`. `log` keeps quill's includes in each partition's
global module fragment.

**Files:** per module, everything under `common/<module>/`, plus that module's tests.

**Interfaces:**
- Produces: `aloe.wire`, `aloe.stream` and `aloe.log`, exporting what their umbrellas exposed, under unchanged
  names.

- [ ] **Step 1: Dispatch** one agent per module, as "Running a wave" describes.
- [ ] **Step 2: Agent work.** Each agent converts its module under the recipe in its own worktree, then builds and
  tests that worktree in `debug`. Expected: it builds, every test passes, and clang-tidy reports nothing.
  - The `log` agent stops and reports if quill's code produces `-WTU-local-entity-exposure` or a missing
    declaration in an importer (Review Focus 3). The fallback is recipe 8.
- [ ] **Step 3: Lead gate.** The lead runs the Task 1 Step 5 gate from fresh build directories. Expected: all pass.
- [ ] **Step 4: Call comparison** (Task 1). Expected: it passes.
- [ ] **Step 5: Commit** one `refactor(<module>)` commit per module, plus any new ledger drafts.

### Task 4: `device` becomes `aloe.device`

Alone in its layer: it needs `wire`, and everything after it needs it.

**Files:** `common/device/` (`device.hpp`, `packet.hpp`, `rss.hpp`), its tests, and
`tests/shared/device_conformance.hpp` only if it included a device header by name.

**Interfaces:**
- Produces: `aloe.device`, exporting `device::IsPacket`, `device::IsDevice`, `device::packet_headroom`, the RSS
  types and everything else `<aloe/device>` exposed.

- [ ] **Step 1: Convert.** Convert under the recipe. The concepts and `static_assert`s move unchanged into the
  exported namespace.
- [ ] **Step 2: Gate and compare.** Run the gate and the call comparison (Task 1). Expected: both pass.
- [ ] **Step 3: Commit** `refactor(device): device is the module aloe.device`.

### Task 5: Wave 2, `loop`, `fabric`, `frames`, `ethdev` (four agents)

Dependencies:

| Module | Needs |
|---|---|
| `loop` | `core`, `device` |
| `fabric` | `core`, `device` |
| `frames` | `device`, `wire` |
| `ethdev` | `core`, `device`, DPDK |

None of them needs another member of the wave.

**Files:** per module, `common/loop/`, `fixtures/fabric/`, `fixtures/frames/` or `common/ethdev/`, plus its tests.

**Interfaces:**
- Produces: `aloe.loop`, `aloe.fabric`, `aloe.frames`, `aloe.ethdev` (or `ethdev` recorded as a header module under
  recipe 8).

- [ ] **Step 1: Dispatch** four agents as in Task 3.
  - **`ethdev`:** keeps every DPDK include in the global module fragment of the partition or implementation unit
    that uses it. It stops at the first exposure diagnostic involving a DPDK `static inline` function and reports it
    (Review Focus 3).
  - **`fabric`:** its `detail/frame.hpp` becomes partition `aloe.fabric:frame`.
- [ ] **Step 2: Gate and compare.** The lead runs the gate and the call comparison. Expected: both pass. Inspect the
  counts for `ethdev::Packet`'s receive and transmit paths in particular.
- [ ] **Step 3: Commit** one commit per module, and the ledger drafts.

### Task 6: `net` becomes `aloe.net`

**Files:** `common/net/` (`arp_cache.hpp`/`.cpp`, `datagram.hpp`, `ipv4.hpp`, `prebuilt.hpp`) and its tests.

**Interfaces:**
- Produces: `aloe.net`, exporting `net::Ipv4<Device>`, `net::Datagram`, `net::Prebuilt`, `net::ArpCache`,
  `net::SendError` and everything else `<aloe/net>` exposed.

- [ ] **Step 1: Convert** under the recipe. `arp_cache.cpp` becomes an implementation unit.
- [ ] **Step 2: Gate and compare.** Expected: both pass; check the tick-to-trade binary's `aloe::net::` counts first.
- [ ] **Step 3: Commit** `refactor(net): net is the module aloe.net`.

### Task 7: `tcp` becomes `aloe.tcp`

The largest module: 26 files, among them the 1,653-line `tcp_stack.hpp`, and `flow_table.cpp`.

**Files:** `common/tcp/` and `tests/unit_tests/common/tcp/`, plus `tests/shared/` TCP fixtures if they named a tcp
header.

**Interfaces:**
- Produces: `aloe.tcp`, exporting `tcp::Stack`, `tcp::Connection`, `tcp::TcpConfig`, `tcp::TcpCounters`,
  `tcp::NewReno`, `tcp::NoCongestionControl`, the concepts in `tcp_fwd`, and the whole `tcp::detail` namespace the
  tests use.

- [ ] **Step 1: Convert** under the recipe. `Table` and `Stack` fold into the one library.
  - Partitions follow the folders' files one to one (`aloe.tcp:tcp_stack`, `aloe.tcp:rtt_estimator`, ...).
  - The `PrivateTag` passkeys and the `friend class Stack` declarations stay as they are.
- [ ] **Step 2: Gate and compare.** Expected: all pass, including the scenario scripts (scenario still includes
  `<aloe/tcp>`). The call comparison passes; check the tick-to-trade binary's `aloe::tcp::` counts first.
- [ ] **Step 3: Commit** `refactor(tcp): tcp is the module aloe.tcp`.

### Task 8: `scenario` becomes `aloe.scenario` (one agent), beside the lead's review of `tests/shared`

**Files:** `fixtures/scenario/` (13 files; its six quoted includes become partition imports or implementation-unit
includes) and its tests.

**Interfaces:**
- Produces: `aloe.scenario`, exporting the parser, runner, harness and script types under unchanged names.

- [ ] **Step 1: Convert** under the recipe, in one agent.
  - `parse_script.cpp`, `runner.cpp`, `harness.cpp`, `peer.cpp`, `match.cpp` and `notation.cpp` become
    implementation units.
  - Meanwhile the lead confirms that every header under `tests/shared/` reaches the converted modules only through
    umbrellas.
- [ ] **Step 2: Gate and compare.** Expected: all pass, with the scenario scripts run in both checksum modes as
  before.
- [ ] **Step 3: Commit** `refactor(scenario): scenario is the module aloe.scenario`.

### Task 9 (optional): `runtime` becomes `aloe.runtime`, `execution` stays a header

Run only if Tasks 0 to 8 landed and the author asks for it. `execution` stays the header `<aloe/execution>`, which
the runtime's partitions include in their global module fragments. That is the case ledger 06 describes.

**Files:** `common/runtime/` (15 files), `common/runtime/CMakeLists.txt` (`-Wno-TU-local-entity-exposure` as a
`PRIVATE` and `INTERFACE` compile option of the runtime library only), and the runtime's tests.

- [ ] **Step 1: Convert** under the recipe. Add the warning option first, with a comment naming ledger 06, and only
  if the exposure appears.
- [ ] **Step 2: Gate.** Expected: all pass, including `examples/tcp_echo_tasks` and the runtime tests under `tsan`.
- [ ] **Step 3: Commit** `refactor(runtime): runtime is the module aloe.runtime; execution stays a header`.

### Task 10: The measurements and the report

**Files:**
- Create: `docs/superpowers/experiments/modules/report.md`
- Modify: the ledger README, for statuses and any final drafts

- [ ] **Step 1: Final numbers.** Run `include-census.py` and `aloe-calls.sh` on the final tree and diff them against
  the baselines. Time a clean `debug` build as `rm -rf build/debug; cmake --preset debug; cmake --build --preset debug`, three runs, reporting each time and the Ninja step count. `--clean-first` skips the configure step, so its time does not compare with the 129 s baseline, and one sample cannot tell a difference from noise.
- [ ] **Step 2: Incremental rebuild.** Touch one leaf partition (`common/wire/tcp/tcp_sequence.cppm`) and time
  `cmake --build --preset debug`. Do the same on a checkout of Task 1's commit, touching
  `common/wire/tcp/tcp_sequence.hpp`. Then confirm the rebuilt tests still pass. Any importer left stale shows up
  here as a link error.
- [ ] **Step 3: Write the report.** It records:
  - per module: converted, or kept as a header module with the reason;
  - the census before and after: include lines replaced by imports, and lines per module;
  - clean and incremental build times;
  - the call comparison;
  - every workaround in force and its ledger entry;
  - the ledger's state;
  - a recommendation: adopt on clang now, wait for GCC PR 128318, or drop. Also whether Task 1 goes to `main`
    regardless.
- [ ] **Step 4: Commit** the AGENTS.md wording that describes the branch's final state, if it changed.
