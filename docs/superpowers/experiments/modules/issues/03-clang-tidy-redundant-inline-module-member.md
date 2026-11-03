# 03 clang-tidy: readability-redundant-inline-specifier calls a member of a named module implicitly inline

**Project:** LLVM (clang-tidy). **Status:** draft. It does not block Aloe, whose `.clang-tidy` leaves the check
off, but anyone who follows its advice in a module loses inlining on the hot path.

**Duplicates searched:** llvm #122291 "Clang-tidy: False positive readability-redundant-inline-specifier" (closed)
is about out-of-class definitions in headers, a different case.

## Title

[clang-tidy] readability-redundant-inline-specifier: a member function defined in its class inside a named module is
not implicitly inline

## Body

Since C++20 ([class.mfct]/1, [dcl.inline]), a member function defined in its class definition is implicitly inline
only if it is attached to the global module. Inside a named module it is not, and clang behaves that way: at `-O3`
an importer calls such a member out of line, and inlines it once it is declared `inline`. clang-tidy 22.1.8 still
reports the `inline` as redundant:

```
hot.cppm:13:23: warning: function 'mix_member' has inline specifier but is implicitly inlined [readability-redundant-inline-specifier]
```

Removing it, as the check suggests, changes the generated code of every importer from an inlined `imul` to a `call`.

Reproducer: `repro/ccache-stale/hot.cppm` with `mix_member` declared `inline`, run through
`clang-tidy --checks=-*,readability-redundant-inline-specifier` with a compile database from CMake 4.3
(`CMAKE_EXPORT_COMPILE_COMMANDS=ON`). Disassembly evidence, release build, no ccache:

| function in `probe.hot`                | importer at `-O3`  |
|----------------------------------------|--------------------|
| free function, not `inline`            | `jmp` to the module's copy |
| member defined in class, not `inline`  | `call`             |
| member defined in class, `inline`      | inlined            |

Expected: no diagnostic for a member attached to a named module.

Note for the report: rerun the check with a real compile database before filing. Aloe's quick run passed no
database, so it also printed parse errors on `export module`; only the redundant-inline warning is the finding.
