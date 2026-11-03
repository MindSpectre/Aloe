# 02 clang-tidy: readability-redundant-declaration fires on libstdc++'s operator new/delete in every importer

**Project:** LLVM (clang-tidy). **Status:** draft.

**Duplicates searched:** llvm/llvm-project issues for "clang-tidy modules redundant-declaration" (none);
llvm #63939 "[Clang Tidy][C++20 Modules] Clang Tidy too slow and buggy on modules" (open since 2023) lists this check
among the ones to disable for `import std;`, without a reproducer of its own:
https://github.com/llvm/llvm-project/issues/63939. The Extra Clang Tools 20.1.0 release notes mention fixed false
positives for "C++20 export declarations" and "global scoped overloaded operator new and operator delete", which
does not cover this case on 22.1.8.

## Title

[clang-tidy] readability-redundant-declaration reports libstdc++'s global operator new/delete in a file that
imports a named module

## Body

A translation unit that imports a named module whose global module fragment includes standard headers gets four
`readability-redundant-declaration` diagnostics, one each for `operator new`, `operator new[]`, `operator delete`
and `operator delete[]`, located in libstdc++'s `<new>` (a system header) with "previously declared here" pointing
at the same header. The importer declares none of them. `--header-filter` does not suppress them, and with
`--warnings-as-errors=*` every importing file fails.

Versions: clang-tidy 22.1.8 (Fedora 22.1.8-4.fc44), libstdc++ 16.1.1, CMake 4.3.0 with Ninja 1.13.2,
`CMAKE_CXX_SCAN_FOR_MODULES=ON`, `-std=c++26`.

Reproducer: `repro/redundant-declaration/` (`wire.hpp`, `wire.cppm`, `a_import.cpp`). `wire.cppm` is

```cpp
module;
#include "wire.hpp"          // includes <algorithm>, <array>, <cstdint>, <span>
export module probe.wire;
export namespace probe::wire {
    using probe::wire::MacAddress;
    using probe::wire::sum;
}
```

and `a_import.cpp` includes `<array>`, `<cstdint>`, `<span>`, then `import probe.wire;` and uses both names.
CMake: one library with the `.cppm` in a `CXX_MODULES` file set, one executable linking it, and
`CMAKE_CXX_CLANG_TIDY=clang-tidy;--checks=-*,readability-redundant-declaration;--warnings-as-errors=*`.

Expected: no diagnostic; the importer redeclares nothing. Actual (once per operator):

```
/usr/include/c++/16/new:107:6: note: previously declared here
error: redundant 'operator new' declaration [readability-redundant-declaration,-warnings-as-errors]
```

The module interface unit itself (`wire.cppm`) is clean; only importers are affected.
