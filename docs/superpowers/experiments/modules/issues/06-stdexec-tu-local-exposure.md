# 06 stdexec: a module that returns a stdexec sender exposes TU-local entities

**Project:** stdexec. **Status:** draft. It matters to Aloe only if the runtime becomes a module, the last and
optional task of the experiment.

**Duplicates searched:** stdexec issues for "TU-local" (only #2071, unrelated) and "module" (#2148 GCC ICE, #2139
"Modularizing stdexec", open, which plans to replace the `__sexpr` machinery):
https://github.com/NVIDIA/stdexec/issues/2139

## Title

Sender built in a module interface exposes TU-local entities (`-WTU-local-entity-exposure`) from `__sexpr` and
`operator|`

## Body

A module interface that includes `<stdexec/execution.hpp>` in its global module fragment and exports an inline
function returning `stdexec::just(v) | stdexec::then(f)` gets two `-WTU-local-entity-exposure` warnings from clang
22.1.8. Both name entities in stdexec's anonymous namespace: `stdexec::(anonymous namespace)::__sexpr<...>` from
`__basic_sender.hpp:59`, and the `operator|` that combines it with the `then` closure. Under `-Werror` the module
fails to build. With `-Wno-TU-local-entity-exposure` it builds, and an importer that `sync_wait`s the sender runs
correctly.

```cpp
module;
#include <stdexec/execution.hpp>
export module probe.exec;

export namespace probe::exec {
    [[nodiscard]] inline auto plus_one(const int value) {
        return stdexec::just(value) | stdexec::then([](const int v) { return v + 1; });
    }
}
```

Versions: stdexec 2026-05-25 (vcpkg port), clang 22.1.8, CMake 4.3.0.

Expected: stdexec's sender types have linkage, so a module may expose them.
