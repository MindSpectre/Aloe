# 09 CMake 4.4: a synthesized BMI target takes the importer's compile options but not its include directories

**Project:** CMake 4.4.4 (Ninja, clang-scan-deps 22.1.8). **Status:** draft, reproduced; duplicates not searched yet. Aloe works
around it in `cmake/dpdk.cmake` by giving DPDK's forced include an absolute path.

## What Aloe saw

After moving to CMake 4.4.4 (ledger 08), the dependency scan of `Aloe.Common.Core@synth_0`, the synthesized BMI
target for the DPDK-linking consumers of `aloe.core`, failed for every interface unit of core:

```
<built-in>:510:10: fatal error: 'rte_config.h' file not found
```

The scan command carries the importer's options from the INTERFACE library `Aloe.Dpdk`
(`-include rte_config.h -march=corei7 -mrtm ...`) but only core's own include path, not `Aloe.Dpdk`'s
`SYSTEM INTERFACE` include directories. DPDK's pkg-config data names the forced include by its bare name.

## Expected

Either the synthesized target takes the importer's include directories together with its options, or it takes
neither. A forced include (`-include x.h`), which is a compile option resolved against the include path, is the
case where taking only one half breaks.

## Workaround

`cmake/dpdk.cmake` puts `-isystem <dir>` for each of DPDK's include directories into the same compile-option unit as
the `-include`, so the synthesized target gets them too. The same directories stay in
`target_include_directories(SYSTEM INTERFACE)`; the duplicate `-isystem` changes nothing for normal targets.

An absolute path for the forced include is not enough: `rte_config.h` itself includes `<rte_build_config.h>` from the
same directory, and a forced include given by absolute path is no longer a system header, so clang-tidy reports
`bugprone-reserved-identifier` inside it.

## Reproducer

Under `repro/synth-include-dirs/` (CMake 4.4.4, clang 22.1.8, Ninja): module library `a`; INTERFACE library `opts`
with `SYSTEM INTERFACE inc` and the options `-include cfg.h -march=corei7 -DX=1`; static library `b` linking `opts`
PUBLIC; `main` linking `a` and `b`, importing `a`. The scan of `a@synth_0` runs with `-include cfg.h -march=corei7
-DX=1` and no `-isystem .../inc`, and fails with `'cfg.h' file not found`. With `-include cfg.h` alone CMake builds
no synthesized target (that option alone does not make the flags BMI-incompatible), and the build passes.
