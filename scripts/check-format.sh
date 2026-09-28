#!/usr/bin/env bash
#
# Fails if a C++ source in the tree is not formatted the way .clang-format asks.
# Run from anywhere; it works on the checkout this script lives in.

set -euo pipefail

cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

files=()
for directory in common component tests examples benchmarks; do
    [[ -d $directory ]] || continue
    while IFS= read -r -d '' file; do
        files+=("$file")
    done < <(find "$directory" -type f \( -name '*.cpp' -o -name '*.hpp' \) -print0 | sort -z)
done

if [[ ${#files[@]} -eq 0 ]]; then
    echo "no C++ sources yet"
    exit 0
fi

clang-format --style=file --dry-run -Werror "${files[@]}"
echo "formatted correctly: ${#files[@]} files"
