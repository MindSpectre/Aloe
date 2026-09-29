#!/usr/bin/env bash
#
# Fails if a C++ source is not formatted the way .clang-format asks.
# Run from anywhere; it works on the checkout this script lives in.
#
#   check-format.sh           every C++ source in the working tree
#   check-format.sh --staged  the staged version of every staged C++ source, which is
#                             what a commit would record; this is what the pre-commit hook runs

set -euo pipefail

cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

source_directories=(common component tests examples benchmarks)

staged=false
case ${1:-} in
    '') ;;
    --staged) staged=true ;;
    *)
        echo "usage: ${0##*/} [--staged]" >&2
        exit 2
        ;;
esac

is_checked_source() {
    local file=$1 directory
    [[ $file == *.cpp || $file == *.hpp ]] || return 1
    for directory in "${source_directories[@]}"; do
        [[ $file == "$directory"/* ]] && return 0
    done
    return 1
}

files=()
if $staged; then
    # Added, copied, modified and renamed. A deleted file has no content left to check.
    while IFS= read -r -d '' file; do
        if is_checked_source "$file"; then
            files+=("$file")
        fi
    done < <(git diff --cached --name-only --diff-filter=ACMR -z)
else
    for directory in "${source_directories[@]}"; do
        [[ -d $directory ]] || continue
        while IFS= read -r -d '' file; do
            files+=("$file")
        done < <(find "$directory" -type f \( -name '*.cpp' -o -name '*.hpp' \) -print0 | sort -z)
    done
fi

if [[ ${#files[@]} -eq 0 ]]; then
    $staged || echo "no C++ sources yet"
    exit 0
fi

if ! command -v clang-format >/dev/null; then
    echo "clang-format not found; scripts/install-linux.sh installs it" >&2
    exit 1
fi

if ! $staged; then
    clang-format --style=file --dry-run -Werror "${files[@]}"
    echo "formatted correctly: ${#files[@]} files"
    exit 0
fi

# The staged blob, not the file in the working tree: the two differ when a file is
# only partly staged, and the blob is what ends up in the commit.
unformatted=()
for file in "${files[@]}"; do
    git show ":$file" | clang-format --assume-filename="$file" --style=file --dry-run -Werror || unformatted+=("$file")
done

if [[ ${#unformatted[@]} -gt 0 ]]; then
    echo "not formatted the way .clang-format asks; to fix, then stage the result:" >&2
    echo "  clang-format -i$(printf ' %q' "${unformatted[@]}")" >&2
    exit 1
fi
