#!/usr/bin/env python3
"""Two tick-to-trade result files side by side, with the change of every percentile.

    scripts/compare-benchmark.py baseline.json candidate.json [--threshold PERCENT]

Rows are matched by round, level, topology, payload and metric. The exit status is 1 when any p50, p90, p99,
p99.9 or max moved by more than the threshold (default 5 percent) in either direction, so a rerun after a phase 2
step can gate on it; the table is printed either way. Machines and builds are printed, never compared: a baseline
from one machine is not read against another.
"""

import argparse
import json
import sys

PERCENTILES = ("p50", "p90", "p99", "p999", "max")
KEYS = ("round", "level", "topology", "payload", "metric")


def key(row):
    return (row["round"], row["level"], row["topology"], row["payload"], row["metric"])


def load(path):
    with open(path, encoding="utf-8") as handle:
        report = json.load(handle)
    if report.get("schema") != 1:
        sys.exit(f"{path}: unknown schema {report.get('schema')!r}")
    for section in ("machine", "run", "rows"):
        if section not in report:
            sys.exit(f"{path}: no {section!r} section")
    for index, row in enumerate(report["rows"]):
        for field in KEYS + PERCENTILES:
            if field not in row:
                sys.exit(f"{path}: row {index} has no {field!r}")
        for field in PERCENTILES:
            if not isinstance(row[field], int) or isinstance(row[field], bool):
                sys.exit(f"{path}: row {index} has a non-integer {field!r}: {row[field]!r}")
    return report


def describe(report):
    machine, run = report["machine"], report["run"]
    return (f"{machine['cpu']}, {machine['cpus']} cpus, smt {machine['smt']}, {machine['governor']}, "
            f"clocksource {machine['clocksource']}, kernel {machine['kernel']}; revision {run['revision']}, "
            f"{run['build']}, {run['rounds']} rounds, gap {run['gap_ns']} ns, cores {run['stack_core']}/{run['peer_core']}, "
            f"{'no hugepages' if run['no_huge'] else 'hugepages'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("baseline")
    parser.add_argument("candidate")
    parser.add_argument("--threshold", type=float, default=5.0, help="percent of change that flags a row (5)")
    arguments = parser.parse_args()
    baseline, candidate = load(arguments.baseline), load(arguments.candidate)
    print(f"baseline:  {describe(baseline)}")
    print(f"candidate: {describe(candidate)}")
    before = {key(row): row for row in baseline["rows"]}
    flagged = missing = 0
    header = f"{'round':8} {'level':7} {'topology':10} {'payload':>7} {'metric':12}" + "".join(
        f" {p + ' base':>10} {p + ' new':>10} {'%':>7}" for p in PERCENTILES)
    print(header)
    for row in candidate["rows"]:
        base = before.get(key(row))
        line = f"{row['round']:8} {row['level']:7} {row['topology']:10} {row['payload']:>7} {row['metric']:12}"
        if base is None:
            print(line + "  (new row)")
            continue
        for p in PERCENTILES:
            old, new = base[p], row[p]
            if old == 0 and new != 0:
                shown, moved = "n/a", True
            else:
                change = (new - old) * 100.0 / old if old else 0.0
                shown, moved = f"{change:>+6.1f}", abs(change) > arguments.threshold
            flagged += moved
            line += f" {old:>10} {new:>10} {shown:>6}{'!' if moved else ' '}"
        print(line)
    after = {key(row) for row in candidate["rows"]}
    for row in baseline["rows"]:
        if key(row) not in after:
            print(f"{row['round']:8} {row['level']:7} {row['topology']:10} {row['payload']:>7} {row['metric']:12}"
                  "  (missing)")
            missing += 1
    if flagged or missing:
        parts = []
        if flagged:
            parts.append(f"{flagged} percentile(s) moved by more than {arguments.threshold}%")
        if missing:
            parts.append(f"{missing} baseline row(s) missing from the candidate")
        print("; ".join(parts))
        return 1
    print(f"no percentile moved by more than {arguments.threshold}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
