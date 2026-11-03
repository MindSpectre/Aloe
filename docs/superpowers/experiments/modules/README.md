# C++ modules experiment: result

**Status: failed for now, not closed.** Stopped on 2026-11-03 after two waves. The branch `experiment/modules` is
kept as a reference; it is not to be merged. Retry when the conditions at the end hold.

## Question

Can Aloe's modules become C++20 named modules, so a module's code imports other modules (`import aloe.<m>;`)
instead of maintaining include lists, with no change in behaviour or generated code?

## What was converted

| Module | Result |
|---|---|
| `core`, `wire`, `stream`, `log`, `device`, `fabric`, `frames`, `loop` | named modules `aloe.<m>`; the umbrella `<aloe/<m>>` is a shim that includes the standard headers and imports the module |
| `ethdev` | stays a header module: exported inline members of `ethdev::Packet` call nine DPDK `static inline` functions, which a module may not expose (`-WTU-local-entity-exposure`, fatal under `-Werror`). This is the standard's rule, not a bug |
| `execution` | stays a header by plan (stdexec exposes TU-local entities, ledger 06) |
| `net`, `tcp`, `scenario`, `runtime` | not attempted |

Through `device` (commit `66fe60f`) every gate ran green from empty build directories with CMake 4.4.4: `debug` and
`asan` 58/58, `tsan` 45/45, the format check, and the include census. The release call comparison (call and jump
targets in `aloe::` code over every unit test binary and `tick_to_trade`) matched the pre-experiment baseline exactly:
717 targets, none higher, none new.

Wave 2 (`fabric`, `frames`, `loop`) passed `debug` 58/58 module by module, but the merged tip does not build two
targets, `Aloe.Tests.Unit.Benchmarks` and `Aloe.Tests.Integration.Benchmarks.Levels` (ledger 10: their module map
takes the synthesized `aloe.core` with its plain partitions, and direct links only moved the mismatch). `debug` at the
tip: 56/58 pass, those two not run; `tsan` 45/45. The experiment stopped here, so the call comparison for `loop` was
never run on the merged tree. Performance was not the problem: with an explicit `inline` on in-class members (a module does not
make them implicitly inline), the generated code did not change, and DPDK code compiles as before since `ethdev`
stayed textual.

## Why it stopped

The tooling, not the language:

- **GCC cannot build it** (ledger 01, GCC PR 128318): clang only, which loses the second compiler.
- **CMake**: 4.3, the version Fedora ships, crashes clang once a module imports a module through an INTERFACE library
  (08). 4.4.4 fixes the crash but synthesized BMI targets drop the importer's include directories (09) and mix plain
  and synthesized BMIs for modules reached through an INTERFACE library (10). Every not-yet-converted Aloe module is an
  INTERFACE combined library, so 10 recurs wave after wave and needs per-target direct links: after wave 2 it hit the benchmark harness,
  `examples/ethernet_echo` and three test targets. Each module of wave 2 built alone; merged, clang crashed nine
  times on mixed module maps until those targets linked the converted modules directly.
- **clang-tidy**: false positives in importers (02, 03) and a crash on a mixed module map.
- **ccache** serves stale importer objects (04); **clangd** reports errors in every `.cppm`.
- **Includes did not go away inside a module**: standard headers move into each unit's global module fragment, and
  clang's locale bug (07) makes every umbrella repeat its module's standard-include list before the import.
- Clean `debug` build: 129 s before, 144 s after `core` alone.

## What is worth keeping

- `f423d02` (other modules' code comes in through their umbrella headers only) applies to `main` on its own.
- The issue drafts with minimal reproducers in `issues/`, ready to file upstream.
- The conversion recipe and wave procedure in `docs/superpowers/plans/2026-11-03-modules-experiment.md`, amended
  with everything learned.

## Retry when

- GCC fixes PR 128318, or a single-compiler build is acceptable;
- CMake handles modules reached through INTERFACE libraries consistently (ledgers 08 and 10), or Aloe's combined
  libraries stop being INTERFACE libraries;
- clangd and clang-tidy handle importers without false positives.

The branch builds only with clang and CMake ≥ 4.4.4 (`pip install cmake==4.4.4` into a virtual environment works).
