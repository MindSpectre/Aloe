#!/usr/bin/env python3
"""Include census of the modules experiment.

Per module: bare same-module includes, bare cross-module includes, umbrella includes, imports, and the
lines of .hpp plus .cppm. A bare include `<x.hpp>` belongs to the module whose folder holds a file x.hpp.
Also scans tests/, benchmarks/ and examples/, where every bare include of a module's header counts as
cross-module. Exit status is 1 when any bare cross-module include exists.

With --umbrellas, instead checks each converted module (one with <dir>/<module>.cppm): the umbrella
export/aloe/<module> must list every standard, system and third-party header that the module's .cppm
global module fragments include, under the same #if line, and include <aloe/<dep>> for every
`import aloe.<dep>;`. Prints `ok`, MISSING, EXTRA and MISSING-UMBRELLA lines; exit 1 on a MISSING of
either kind. `--umbrella-dir DIR` reads DIR/<module> instead of export/aloe/<module> (for tests).
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


COND = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b\s*(.*)$")
AIMPORT = re.compile(r"^\s*(?:export\s+)?import\s+aloe\.(\w+)\s*;")
MODULE_DECL = re.compile(r"^\s*(?:export\s+)?module\s+[\w.:]+\s*;")


def scan_includes(lines, stop):
    """(guard, header, quote) triples of the non-umbrella includes in lines, until stop(line) is true.

    The guard is the text of the innermost enclosing #if line, or None. One level only: a nested #if
    is reported by its innermost line alone, and an #else or #elif is keyed as such, so it never
    matches the #if branch. Nested guards are not needed today.
    """
    out, stack = [], []
    for line in lines:
        if stop(line):
            break
        c = COND.match(line)
        if c:
            kind, rest = c.group(1), " ".join(c.group(2).split())
            text = f"#{kind} {rest}".strip()
            if kind in {"if", "ifdef", "ifndef"}:
                stack.append(text)
            elif kind in {"elif", "else"} and stack:
                stack[-1] = f"{stack[-1]} / {text}"
            elif kind == "endif" and stack:
                stack.pop()
            continue
        m = INCLUDE.match(line)
        if m and not m.group(2).startswith("aloe/"):
            out.append((stack[-1] if stack else None, m.group(2), m.group(1)))
    return out


def header_index():
    idx = {}
    for group in GROUPS:
        for f in (ROOT / group).rglob("*.hpp"):
            idx[f.name] = f
    return idx


def expand(entries, index, seen):
    """Follow bare or quoted includes of header-module headers (.hpp under common/ or fixtures/)."""
    out = set()
    for guard, target, _ in entries:
        f = index.get(target) if "/" not in target else None
        if f is None:
            out.add((guard, target))
        elif f not in seen:
            seen.add(f)
            inner = scan_includes(f.read_text(errors="replace").splitlines(), lambda _l: False)
            # An include inside the header keeps its own guard, else the guard of the include of the header.
            out |= expand([(g or guard, t, q) for g, t, q in inner], index, seen)
    return out


def fmt(guard, header):
    header = f"<{header}>"
    return f"{header} (under {guard})" if guard else header


def check_umbrellas(mods, umbrella_dir) -> int:
    index = header_index()
    failed = False
    for name, d in mods.items():
        if not (d / f"{name}.cppm").is_file():
            continue
        cppms = sorted(list(d.rglob("*.cppm")) + list(d.rglob("*.cppm.in")))
        expected, deps = set(), set()
        for f in cppms:
            lines = f.read_text(errors="replace").splitlines()
            deps |= {m.group(1) for m in map(AIMPORT.match, lines) if m}
            starts = [i for i, l in enumerate(lines) if l.strip() == "module;"]
            if starts:
                inc = scan_includes(lines[starts[0] + 1 :], lambda l: bool(MODULE_DECL.match(l)))
                expected |= expand(inc, index, set())
        path = (umbrella_dir / name) if umbrella_dir else d / "export" / "aloe" / name
        ulines = path.read_text(errors="replace").splitlines()
        actual = expand(scan_includes(ulines, lambda l: bool(AIMPORT.match(l) or IMPORT.match(l))), index, set())
        ulist = {m.group(2) for m in map(INCLUDE.match, ulines) if m}
        missing, extra = sorted(expected - actual, key=str), sorted(actual - expected, key=str)
        no_umbrella = sorted(dep for dep in deps if f"aloe/{dep}" not in ulist)
        for g, h in missing:
            print(f"MISSING {name} {fmt(g, h)}")
        for g, h in extra:
            print(f"EXTRA {name} {fmt(g, h)}")
        for dep in no_umbrella:
            print(f"MISSING-UMBRELLA {name} {dep}")
        failed |= bool(missing or no_umbrella)
        if not (missing or extra or no_umbrella):
            print(f"ok {name}")
    return 1 if failed else 0


def main() -> int:
    mods = modules()
    if "--umbrellas" in sys.argv:
        dir_arg = sys.argv[sys.argv.index("--umbrella-dir") + 1] if "--umbrella-dir" in sys.argv else None
        return check_umbrellas(mods, Path(dir_arg) if dir_arg else None)
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
