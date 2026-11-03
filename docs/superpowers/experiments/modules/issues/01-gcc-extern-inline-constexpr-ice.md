# 01 GCC: ICE in transfer_defining_module when an extern const is redeclared inline constexpr

**Project:** GCC. **Status:** already reported, nothing to file. Add Aloe's data point to the GCC PR only if it is
still UNCONFIRMED when the experiment ends.

- GCC PR 128318, "ICE: Segmentation fault in transfer_defining_module with -fmodules-ts when an extern const
  variable is redeclared inline constexpr", UNCONFIRMED, filed 2026-10-30:
  https://gcc.gnu.org/bugzilla/show_bug.cgi?id=128318
- stdexec #2148, "Internal compiler error on GCC with modules", open since 2026-07-27:
  https://github.com/NVIDIA/stdexec/issues/2148
- iains/gcc-darwin-arm64 #155, same backtrace on Darwin: https://github.com/iains/gcc-darwin-arm64/issues/155

## What Aloe saw

With `-fmodules` (or `-fmodules-ts`), GCC 16.1.1 segfaults on any translation unit that contains stdexec's text,
whether or not it declares or imports a module: a file holding only `#include <stdexec/execution.hpp>` crashes at
`stdexec/__detail/__query.hpp:177`, `inline constexpr forwarding_query_t forwarding_query{};`. C++20, 23 and 26 all
crash, and `-fno-module-lazy` does not help. stdexec declares about 80 of its customisation-point objects as
`extern T const name;` before the `inline constexpr` definition, and has no configuration switch that avoids it.

## Reproducer

`repro/gcc-extern-inline/tiny.cpp`:

```cpp
namespace stdexec {
  struct forwarding_query_t;
  extern forwarding_query_t const forwarding_query;
}
namespace stdexec {
  struct forwarding_query_t {};
  inline constexpr forwarding_query_t forwarding_query{};
}
```

```
$ g++ -std=c++26 -fmodules -c tiny.cpp
tiny.cpp:7:39: internal compiler error: Segmentation fault
$ g++ -std=c++26 -c tiny.cpp      # exit 0
```
