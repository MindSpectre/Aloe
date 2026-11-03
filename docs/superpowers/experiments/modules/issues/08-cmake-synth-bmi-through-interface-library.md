# 08 CMake 4.3: a consumer reached through an INTERFACE library gets the plain BMI while its imported module used a synthesized one

**Project:** CMake (with clang's crash on mismatched BMIs, llvm #221405). **Status:** the crash in this reproducer is gone in CMake 4.4.4, but
the plain-BMI half remains (ledger 10); check whether 4.3.x is still maintained before filing anything. If it is, file against 4.3
with the reproducer below. Otherwise this entry only records why Aloe needs CMake ≥ 4.4.

**Related:** llvm/llvm-project #221405, "[clang][modules] Crash in ASTReader when a TU is given a different BMI for a
transitively imported module" (open since 2026-09-05), the clang side: clang segfaults instead of diagnosing
mismatched BMIs: https://github.com/llvm/llvm-project/issues/221405. CMake's manual (cmake-cxxmodules, "synthetic
target") and CMake issue 25916 describe synthetic targets and the one-set-of-BMIs assumption.

## What Aloe saw

`log` became the first Aloe module that imports another module (`aloe.core`). Fifteen consumers crashed clang
22.1.8 in `ASTDeclReader::attachPreviousDeclImpl<FunctionDecl>`: every runtime test, the ring integration test, the
two tap tests and `examples/tcp_echo_tasks`. All of them reach `Aloe.Common.Log` through `Aloe::Common::Runtime`, an
INTERFACE library.

## Reproducer

Under `repro/synth-bmi-interface/`. With clang 22.1.8, Ninja 1.13.2 and `-std=gnu++23`:

```cmake
cmake_minimum_required(VERSION 4.3)
project(r CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
add_library(a STATIC)
target_sources(a PUBLIC FILE_SET modules TYPE CXX_MODULES BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES a.cppm)
add_library(b STATIC)
target_sources(b PUBLIC FILE_SET modules TYPE CXX_MODULES BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES b.cppm)
target_link_libraries(b PUBLIC a)
add_library(c INTERFACE)
target_link_libraries(c INTERFACE b)
add_executable(main main.cpp)
target_link_libraries(main PRIVATE c)
```

```cpp
// a.cppm
module;
#include <string>
export module a;
export struct A { std::string s; int f() const { return 1; } };
// b.cppm
module;
#include <string>
export module b;
import a;
export struct B { A a; int g() const { return a.f(); } };
// main.cpp
import a;
import b;
int main() { B b{}; return b.g() - 1; }
```

| CMake | `main` links | `b.cppm`'s BMI of `a` | `main`'s BMIs | Result |
|---|---|---|---|---|
| 4.3.0 | `c` (INTERFACE → `b`) | `a@synth_*.bmi` | plain `a.pcm`, plain `b.pcm` | clang segfault (exit 139) |
| 4.3.0 | `b` directly | `a@synth_*.bmi` | `a@synth_*`, `b@synth_*` | builds |
| 4.3.0 | `c` and `b` | `a@synth_*.bmi` | `a@synth_*`, `b@synth_*` | builds |
| 4.4.4 | `c` (INTERFACE → `b`) | plain | plain `a.pcm`, plain `b.pcm` | builds |

The compile flags of `a`, `b` and `main` are identical in the first row (`-std=gnu++23`, plus one warning flag added
to every target with `add_compile_options`), so this is not a flag mismatch. The synthesized BMI is built with
`--precompile` in a separate step, while `a.pcm` comes from the object compile's `-fmodule-output`. The two files
differ, and `main` sees both: the plain one directly, and the synthesized one through `b.pcm`.

Expected: in CMake 4.3, a consumer that links module libraries only through an INTERFACE library gets the same BMI
set as the libraries it imports were built against.
