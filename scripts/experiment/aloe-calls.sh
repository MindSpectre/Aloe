#!/usr/bin/env bash
# Usage: aloe-calls.sh <binary>...
# Prints "<count> <demangled name>" per distinct call target in aloe:: code, sorted by name,
# summed over all the binaries. The count is the number of call/jmp instructions naming it.
set -euo pipefail

[[ $# -gt 0 ]] || { echo "usage: ${0##*/} <binary>..." >&2; exit 2; }

for binary in "$@"; do
    objdump -d -C --no-show-raw-insn "$binary"
done |
    # A call or jmp whose operand is "<symbol>". The name may itself contain < and >.
    grep -E '^\s+[0-9a-f]+:\s+(call|jmp)[a-z]*\s+[0-9a-f]+ <.*>$' |
    # Strip through the first " <" and the trailing ">".
    sed -E 's/^[^<]*<//; s/>$//' |
    # A jump within a function names an offset: <name+0x40>.
    { grep -v -E '\+0x[0-9a-f]+$' || true; } |
    { grep -v '^initializer for module ' || true; } |
    # The module attachment is the primary module's name: advance@probe.wire, TcpSequence@probe.wire::next.
    # A signature can carry several (TscClock@aloe.core::trust(TscEvidence@aloe.core const&)), so strip each one.
    sed -E 's/@aloe\.[a-z_]+(:[a-z_]+)?//g' |
    { grep '^aloe::' || true; } |
    LC_ALL=C sort |
    uniq -c |
    awk '{ count = $1; sub(/^ *[0-9]+ /, ""); print count, $0 }' |
    LC_ALL=C sort -k2
