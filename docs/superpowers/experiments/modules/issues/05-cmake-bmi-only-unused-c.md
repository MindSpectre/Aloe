# 05 CMake: the BMI-only command for clang passes `-c`, which clang reports as unused

**Project:** CMake (possibly clang's driver). **Status:** draft; search Kitware's GitLab for duplicates before
filing.

**Related, not duplicates:** bemanproject/transform_view PR #12 shows the same warning on a clang 22 `.bmi` step:
https://github.com/bemanproject/transform_view/pull/12. A CMake Discourse thread reports it on clang 16:
https://discourse.cmake.org/t/how-to-control-the-location-of-the-c-20-binary-module-interface-bmi-output-directory/7968/13

## Title

C++20 modules with clang: BMI-only compile of a synthesized target trips `-Wunused-command-line-argument` on `-c`,
which fails under `-Werror`

## Body

When an executable imports a module from a library, CMake 4.3.0 generates a synthesized target
(`<lib>@synth_<hash>`) that builds a BMI only (`<hash>.bmi`). Its command line carries `-c` alongside the BMI-only
output, and clang 22.1.8 reports:

```
clang++: error: argument unused during compilation: '-c' [-Werror,-Wunused-command-line-argument]
FAILED: CMakeFiles/wire@synth_c7a3079a3c1d.dir/209d9834f3a7.bmi
```

Any project that builds with `-Werror` fails until it adds `-Wno-unused-command-line-argument`, which also hides real
unused-argument mistakes.

Versions: CMake 4.3.0, Ninja 1.13.2, clang 22.1.8, `CMAKE_CXX_SCAN_FOR_MODULES=ON`, `-std=c++26`.

Reproducer: the `repro/redundant-declaration/` project built with
`CMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -Werror"` and no clang-tidy. Capture the exact failing command with
`ninja -v` before filing, and attach it.

Expected: the BMI-only command line contains no argument clang reports as unused.
