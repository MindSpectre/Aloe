# 10 CMake 4.4: a module reached through an INTERFACE library keeps its plain BMIs beside synthesized ones

**Project:** CMake 4.4.4 (Ninja, clang 22.1.8). **Status:** draft, reproduced; duplicates not searched yet. The
remainder of ledger 08 after CMake 4.4: 4.4 no longer crashes when every module arrives through the INTERFACE
library, but still hands out plain BMIs for it.

## What Aloe saw

`Aloe.Benchmarks.TickToTrade.Harness` links DPDK (whose `-march=corei7` makes its flags BMI-incompatible), links
`Aloe::Common::Core` directly and reaches `Aloe.Common.Log` through `Aloe::Common::Runtime`, an INTERFACE library.
Its module map named the synthesized BMI of `aloe.core` but the plain BMIs of `aloe.core`'s partitions, and clang
reported `module file '.../aloe.core-clock.pcm' not found` and `declaration of 'TscScale' must be imported from module
'aloe.core'` for every source of the harness.

## Reproducer

Under `repro/synth-partition-mix/`: module `a` with partition `a:p`; module library `l` imports `a`, links `a`
PUBLIC; INTERFACE library `c` links `l`; executable `main` with `-march=corei7` links `a` and `c`, imports `a` and
`l`. Module map of `main.cpp` with CMake 4.4.4:

```
-fmodule-file="a=CMakeFiles/a@synth_0.dir/8f2ad5352a5f.bmi"
-fmodule-file="l=CMakeFiles/l.dir/l.pcm"
-fmodule-file="a:p=CMakeFiles/a.dir/a-p.pcm"
```

`l` and `a:p` come from the plain BMIs (built without `-march=corei7`), `a` from the synthesized one. With `main`
linking `a`, `c` and `l` directly, all three come from `@synth_0` targets.

## Expected

A target gets one consistent BMI set for its flags, whether it reaches a module library directly or through an
INTERFACE library.

## Workaround

A target that links converted modules directly links each converted module it otherwise reaches only through an
INTERFACE library directly too (`benchmarks/tick_to_trade/CMakeLists.txt` links `Aloe::Common::Log`). It goes away
when every target shares one BMI-relevant flag set, so CMake synthesizes nothing.
