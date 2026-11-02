#!/usr/bin/env python3
"""Include census of the modules experiment.

Per module: bare same-module includes, bare cross-module includes, umbrella includes, imports, and the
lines of .hpp plus .cppm. A bare include `<x.hpp>` belongs to the module whose folder holds a file x.hpp.
Also scans tests/, benchmarks/ and examples/, where every bare include of a module's header counts as
cross-module. Exit status is 1 when any bare cross-module include exists.
"""
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GROUPS = ("common", "fixtures")
OUTSIDE = ("tests", "benchmarks", "examples")
SRC = {".hpp", ".cpp", ".cppm", ".h", ".inl"}
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')
IMPORT = re.compile(r"^\s*(?:export\s+)?import\s+[\w.:]+\s*;")


def modules():
    mods = {}
    for group in GROUPS:
        for d in sorted((ROOT / group).iterdir()):
            if d.is_dir():
                mods[d.name] = d
    return mods


def main() -> int:
    mods = modules()
    owner = {}
    for name, d in mods.items():
        for f in d.rglob("*"):
            if f.is_file() and f.suffix in {".hpp", ".h", ".inl"}:
                owner[f.name] = name
    stats = defaultdict(lambda: defaultdict(int))
    violations = []
    scopes = list(mods.items())
    scopes += [(o, ROOT / o) for o in OUTSIDE if (ROOT / o).is_dir()]
    for name, d in scopes:
        for f in sorted(d.rglob("*")):
            if not f.is_file() or f.suffix not in SRC:
                continue
            lines = f.read_text(errors="replace").splitlines()
            if f.suffix in {".hpp", ".cppm"}:
                stats[name]["lines"] += len(lines)
            for no, line in enumerate(lines, 1):
                m = INCLUDE.match(line)
                if m:
                    target = m.group(2)
                    if target.startswith("aloe/"):
                        stats[name]["umbrella"] += 1
                    elif "/" not in target and target in owner:
                        if owner[target] == name:
                            stats[name]["bare_same"] += 1
                        else:
                            stats[name]["bare_cross"] += 1
                            violations.append(f"{f.relative_to(ROOT)}:{no}: {line.strip()} (module {owner[target]})")
                elif IMPORT.match(line):
                    stats[name]["imports"] += 1
    print(f"{'module':<12}{'bare_same':>10}{'bare_cross':>11}{'umbrella':>9}{'imports':>8}{'lines':>8}")
    total = 0
    for name, _ in scopes:
        s = stats[name]
        total += s["bare_cross"]
        print(f"{name:<12}{s['bare_same']:>10}{s['bare_cross']:>11}{s['umbrella']:>9}{s['imports']:>8}{s['lines']:>8}")
    print(f"\ntotal bare cross-module includes: {total}")
    if "-v" in sys.argv or "--verbose" in sys.argv:
        print("\n".join(violations))
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
