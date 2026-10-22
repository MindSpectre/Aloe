#!/usr/bin/env bash
#
# Usage: compare_scenario.sh <python3> <script> <expected exit code> <expected message> <baseline> <candidate>
#                            [script options...]
#
# Runs scripts/compare-benchmark.py on two result files, with any further arguments as its options, and exits
# non-zero, with the reason on stderr, unless the script exited with the expected code and printed the expected
# message (a fixed string).

set -uo pipefail

python=${1:?python3}
script=${2:?script}
expected_code=${3:?expected exit code}
expected_message=${4:?expected message}
baseline=${5:?baseline}
candidate=${6:?candidate}
shift 6

output=$("$python" -I "$script" "$baseline" "$candidate" "$@")
code=$?

if [[ $code -ne $expected_code ]]; then
    printf '%s\n' "$output" >&2
    echo "FAIL: exit code $code, expected $expected_code" >&2
    exit 1
fi
if ! grep -qF -- "$expected_message" <<<"$output"; then
    printf '%s\n' "$output" >&2
    echo "FAIL: output lacks: $expected_message" >&2
    exit 1
fi
printf '%s\n' "$output"
