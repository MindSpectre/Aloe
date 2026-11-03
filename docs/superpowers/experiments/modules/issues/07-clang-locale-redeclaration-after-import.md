# 07 clang: libstdc++'s locale functions included after an import are "inline declaration follows non-inline definition"

**Project:** LLVM (clang). **Status:** draft.

**Duplicates searched:** llvm/llvm-project issues for "follows non-inline definition" and "isspace follows non-inline
definition". #70173 "[C++20] [Modules] Is it a real ODR violation to have non-inline template forward decl followed
with inline template decl?" (closed 2023-10-25) has the same shape, a forward declaration in one module's global
module fragment, the inline definition in another's and the header again after the import, with a synthetic header:
https://github.com/llvm/llvm-project/issues/70173. #59063 (closed) is the same message under Clang modules
(`-fmodules`) with a module map for libstdc++ 11, not named modules. Neither has a reproducer made only of standard
headers under clang 22.

## Title

[C++20] [Modules] Textual `<ios>` after importing a module with two partitions fails: "inline declaration of
'has_facet' follows non-inline definition" (libstdc++ 16)

## Body

A named module has two partitions. Each one includes ordinary standard headers in its global module fragment, and the
second imports the first. A translation unit that imports the module and then includes `<ios>` fails with 16 errors,
one for each of libstdc++'s locale function templates: `has_facet`, `use_facet`, `isspace`, `isprint` and the others
through `tolower`. Each error's "previous definition" is the same line as the error. libstdc++ first declares these
templates without `inline`, `has_facet` and `use_facet` as friends of `locale` (`bits/locale_classes.h:84`) and the
`is*` functions in `bits/localefwd.h`. It then defines them `inline` (`bits/locale_classes.tcc:190`,
`bits/locale_facets.h:2615`). That pattern is valid in a textual build.

The reproducer is `repro/locale-after-import/`:

```cpp
// a.cppm
module;
#include <cmath>
#include <cstdint>
#include <stdexcept>
export module probe:a;
export namespace probe { struct A { int x; }; }

// b.cppm
module;
#include <chrono>
#include <cstdint>
#include <limits>
#include <ratio>
#include <string>
#include <x86intrin.h>
export module probe:b;
import :a;
export namespace probe { struct B { int y; }; }

// probe.cppm
export module probe;
export import :a;
export import :b;

// use.cpp
import probe;
#include <ios>
int main() { return probe::A{0}.x + probe::B{0}.y; }
```

```
clang++ -std=c++26 --precompile a.cppm -o probe-a.pcm
clang++ -std=c++26 --precompile b.cppm -o probe-b.pcm -fmodule-file=probe:a=probe-a.pcm
clang++ -std=c++26 --precompile probe.cppm -o probe.pcm -fmodule-file=probe:a=probe-a.pcm -fmodule-file=probe:b=probe-b.pcm
clang++ -std=c++26 -fmodule-file=probe=probe.pcm -fmodule-file=probe:a=probe-a.pcm -fmodule-file=probe:b=probe-b.pcm -c use.cpp
```

```
.../c++/16/bits/locale_classes.tcc:190:5: error: inline declaration of 'has_facet' follows non-inline definition
.../c++/16/bits/locale_classes.tcc:190:5: note: previous definition is here
.../c++/16/bits/locale_classes.tcc:220:5: error: inline declaration of 'use_facet' follows non-inline definition
...
16 errors generated.
```

Versions: clang 22.1.8 (Fedora 22.1.8-4.fc44), libstdc++ 16.1.1, x86-64 Linux, `-std=c++26`. CMake 4.3.0 with Ninja
1.13.2 reaches the same error through its own BMI commands.

What we observed:
- The error depends on exactly which headers are included. Removing any one of the nine includes from either
  partition makes it go away. Each partition alone, and one module whose fragment includes all nine headers, compile
  cleanly.
- `#include <iostream>` or `#include <format>` after the import fails the same way. `#include <cmath>` does not fail,
  and nor does `#include <memory>` in this reduction. It does fail in a real build that also includes gtest.
- Including `<ios>` before `import probe;` compiles. So does including the partitions' own headers before the import
  and `<ios>` after it.
- A module unit that includes `<ios>` in its global module fragment and then imports `probe` compiles. Its includes
  always precede its imports.

Clang's documentation already warns that including headers after an import is not well supported ("Standard C++
Modules", the section on including headers after import declarations). But the C++ standard allows this order, and a
consumer that is not itself a module cannot always choose it. An umbrella header that imports a module is followed by
whatever the consumer includes next.

Expected: `use.cpp` compiles. Clang should merge the imported `has_facet` declarations with the textual ones without
losing the `inline` that the definition carries.

## Aloe workaround

In each converted module's umbrella header, every standard and system header that the module's interface units
include is included textually before `import aloe.<module>;`. Every later textual copy of those headers is then
stopped by its include guard. See `common/core/export/aloe/core`.

**Narrowing (Task 2 review):** in the reduction, `#include <string>` alone before `import probe;` makes the 16
errors disappear. The trigger is libstdc++'s locale forward declarations, which `<string>` brings in through
`bits/localefwd.h`, being seen textually only after the import. Mention it in the upstream report. Aloe's workaround
still includes the module's whole fragment union, because the reduced fix did not carry over to the full tree.
