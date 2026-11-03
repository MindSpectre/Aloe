# Modules experiment: issue ledger

Problems met while moving Aloe to C++ modules, written up so each one can be reported upstream. Nothing here has
been filed yet unless its status says so. Each draft carries the versions, a minimal reproducer under `repro/`, and
the duplicates searched before writing it.

Toolchain at the time of writing (2026-11-03): clang 22.1.8 (Fedora 22.1.8-4.fc44), GCC 16.1.1 20260515
(Red Hat 16.1.1-2), CMake 4.3.0, Ninja 1.13.2, ccache 4.12.3, stdexec 2026-05-25 (vcpkg).

| #  | Draft                                                            | Project | Status                                         | Aloe workaround                                      |
|----|------------------------------------------------------------------|---------|------------------------------------------------|------------------------------------------------------|
| 01 | [GCC ICE on extern const redeclared inline constexpr](01-gcc-extern-inline-constexpr-ice.md) | GCC     | already filed: GCC PR 128318, stdexec #2148    | build with clang only                                |
| 02 | [redundant-declaration fires in every importer](02-clang-tidy-redundant-declaration-import.md) | LLVM    | draft, no exact duplicate (llvm #63939 related) | `-readability-redundant-declaration` in `.clang-tidy` |
| 03 | [redundant-inline-specifier calls a module member implicitly inline](03-clang-tidy-redundant-inline-module-member.md) | LLVM    | draft                                          | check not enabled in `.clang-tidy`                   |
| 04 | [ccache returns stale importer objects for clang named modules](04-ccache-stale-clang-named-module.md) | ccache  | draft, related to ccache #1252                 | no compiler launcher in the modules presets          |
| 05 | [`-c` unused on CMake's BMI-only command](05-cmake-bmi-only-unused-c.md) | CMake   | draft, needs a duplicate search on Kitware's GitLab | `-Wno-unused-command-line-argument`                  |
| 06 | [stdexec exposes TU-local entities from a module](06-stdexec-tu-local-exposure.md) | stdexec | draft                                          | `-Wno-TU-local-entity-exposure`, only if the runtime becomes a module |
| 07 | [libstdc++'s locale functions included after an import](07-clang-locale-redeclaration-after-import.md) | LLVM    | draft, related to llvm #70173 (closed)         | the umbrella includes its module's standard headers before the import |
| 08 | [a consumer through an INTERFACE library gets the plain BMI](08-cmake-synth-bmi-through-interface-library.md) | CMake | crash gone in 4.4.4, the plain-BMI half remains (10); clang side is llvm #221405 (open) | build with CMake 4.4.4 |
| 09 | [a synthesized BMI target drops the importer's include directories](09-cmake-synth-target-drops-include-dirs.md) | CMake | draft, not searched | DPDK's include dirs as `-isystem` inside its compile options (`cmake/dpdk.cmake`) |
| 10 | [a module reached through an INTERFACE library keeps its plain BMIs beside synthesized ones](10-cmake-synth-mixed-through-interface-library.md) | CMake | draft, reproduced, not searched; the rest of 08 under 4.4 | link such modules directly (the benchmark harness links `Log`) |

New entries take the next number, the same sections, and a row here.
