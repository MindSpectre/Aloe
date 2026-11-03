# 04 ccache: an importer of a clang named module is served from the cache after the module changed

**Project:** ccache. **Status:** draft.

**Duplicates searched:** ccache #1252 "Add support for C++20 modules" (open since 2023):
https://github.com/ccache/ccache/issues/1252. ccache #1703 "Doesn't work with C++20 modules" (closed, not
planned): the maintainer explains that ccache marks GCC's `-fmodules-ts` as too hard and falls back to running the
compiler. The `modules` sloppiness in the manual is about Clang's header modules (`-fmodules`), not named modules.
Neither covers clang named modules, which ccache does not detect at all.

## Title

Clang C++20 named modules: an importer is cached without the imported BMI, so a changed module interface yields a
stale object

## Body

With CMake's C++20 module support, clang receives each imported module through a response file
(`@CMakeFiles/<target>.dir/<file>.o.modmap`, holding `-fmodule-file=<name>=<path>.pcm`). ccache 4.12.3 caches the
importer's compilation without hashing the `.pcm` contents, unlike GCC's `-fmodules-ts`, which it refuses to cache.
After a change to the module interface, Ninja rebuilds the importer, and ccache returns the object compiled against
the old interface.

Versions: ccache 4.12.3, clang 22.1.8, CMake 4.3.0, Ninja 1.13.2, Fedora 44 (ccache's `/usr/lib64/ccache/clang++`
masquerade on `PATH`).

Steps (`repro/ccache-stale/`, one library `hot` with `hot.cppm` in a `CXX_MODULES` file set, one executable over
`hot_user.cpp`, `-O3`):

1. Build with `mix_member` not `inline`. The importer calls `probe::hot::Mixer@probe.hot::mix_member`.
2. Add `inline` to `mix_member` in `hot.cppm` and rebuild. Ninja recompiles `hot_user.cpp`, ccache returns a hit,
   the object still calls the out-of-line symbol, and the link fails:

```
undefined reference to `probe::hot::Mixer@probe.hot::mix_member(unsigned int) const'
```

The same build without ccache (`CMAKE_CXX_COMPILER=/usr/bin/clang++`) inlines the member and links. A change that
keeps the old symbol alive would link and run stale code silently.

Expected: either hash the BMIs named by `-fmodule-file=` (also inside `@` response files), or treat
`-fmodule-file=` like `-fmodules-ts` and fall back to running the compiler.

Before filing: confirm with `ccache --show-log-stats` or the ccache log that the second compile was a hit, rather
than inferring it from the stale object.
