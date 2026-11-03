#!/usr/bin/env bash
#
# Fails if a C++ source breaks the include rules of codestyle.md:
#
#   1. #include "name.hpp"   only in name.cpp, for the header beside it, and that header is
#                            always included this way;
#   2. #include <file.hpp>   a header of the same module, by its basename (<detail/file.hpp>
#                            for one in a detail/ folder);
#   3. #include <aloe/name>  anything from another module: its umbrella, never one of its
#                            headers. A module does not include its own umbrella.
#
# A module is a directory with an export/aloe/<name> umbrella. Tests, examples and the
# benchmark are not modules: they reach every module through its umbrella, and their own
# helpers by basename. Run from anywhere; it works on the checkout this script lives in.
#
#   check-includes.sh           every C++ source in the working tree
#   check-includes.sh --staged  the staged version of every staged C++ source, which is
#                               what a commit would record; the pre-commit hook runs this

set -euo pipefail

cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

source_directories=(common component fixtures tests examples benchmarks)
module_directories=(common component fixtures)

staged=false
case ${1:-} in
    '') ;;
    --staged) staged=true ;;
    *)
        echo "usage: ${0##*/} [--staged]" >&2
        exit 2
        ;;
esac

# Every module: its root directory and its name, from the umbrella's path.
declare -A module_name_of_root=()
for directory in "${module_directories[@]}"; do
    [[ -d $directory ]] || continue
    while IFS= read -r -d '' umbrella; do
        root=${umbrella%/export/aloe/*}
        module_name_of_root[$root]=${umbrella##*/}
    done < <(find "$directory" -path '*/export/aloe/*' -type f -print0)
done

module_root_of() {
    local file=$1 root
    for root in "${!module_name_of_root[@]}"; do
        if [[ $file == "$root"/* ]]; then
            printf '%s' "$root"
            return
        fi
    done
}

# Every header a module owns, by path. Basenames are unique across modules, so a flat
# include names exactly one of them.
module_headers=()
declare -A module_header_by_basename=()
for root in "${!module_name_of_root[@]}"; do
    while IFS= read -r -d '' header; do
        module_headers+=("$header")
        module_header_by_basename[${header##*/}]=$header
    done < <(find "$root" -name '*.hpp' -not -path '*/export/*' -print0)
done

# The module header an include names, if any: by basename for <file.hpp>, by path suffix
# for <dir/file.hpp>. <stdexec/execution.hpp> names none, though execution.hpp is a basename.
header_named() {
    local spelled=$1 header
    if [[ $spelled != */* ]]; then
        printf '%s' "${module_header_by_basename[$spelled]:-}"
        return
    fi
    for header in "${module_headers[@]}"; do
        if [[ $header == */"$spelled" ]]; then
            printf '%s' "$header"
            return
        fi
    done
}

is_checked_source() {
    local file=$1 directory
    [[ $file == *.cpp || $file == *.hpp || $file == */export/aloe/* ]] || return 1
    for directory in "${source_directories[@]}"; do
        [[ $file == "$directory"/* ]] && return 0
    done
    return 1
}

files=()
if $staged; then
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
        done < <(find "$directory" -type f \( -name '*.cpp' -o -name '*.hpp' -o -path '*/export/aloe/*' \) -print0 |
            sort -z)
    done
fi

contents_of() {
    if $staged; then
        git show ":$1"
    else
        cat -- "$1"
    fi
}

violations=0
report() {
    echo "$1:$2: $3" >&2
    violations=$((violations + 1))
}

include_pattern='^[[:space:]]*#[[:space:]]*include[[:space:]]*([<"])([^>"]+)[>"]'

for file in "${files[@]}"; do
    owner=$(module_root_of "$file")
    owner_name=${owner:+${module_name_of_root[$owner]}}
    own_header=
    if [[ $file == *.cpp ]]; then
        own_header=${file##*/}
        own_header=${own_header%.cpp}.hpp
        [[ -f ${file%/*}/$own_header ]] || own_header=
    fi

    line_number=0
    while IFS= read -r line; do
        line_number=$((line_number + 1))
        [[ $line =~ $include_pattern ]] || continue
        delimiter=${BASH_REMATCH[1]}
        spelled=${BASH_REMATCH[2]}

        if [[ $delimiter == '"' ]]; then
            if [[ -z $own_header || $spelled != "$own_header" ]]; then
                report "$file" "$line_number" "\"$spelled\": quotes are only for a .cpp's own header; write <${spelled##*/}> or <aloe/module>"
            fi
            continue
        fi

        if [[ $spelled == aloe/* ]]; then
            if [[ -n $owner && $spelled == "aloe/$owner_name" ]]; then
                report "$file" "$line_number" "<$spelled>: a module includes its own headers, not its umbrella"
            fi
            continue
        fi

        if [[ -n $own_header && $spelled == "$own_header" ]]; then
            report "$file" "$line_number" "<$spelled>: a .cpp includes its own header as \"$own_header\""
            continue
        fi

        header=$(header_named "$spelled")
        [[ -n $header ]] || continue
        header_root=$(module_root_of "$header")
        if [[ $header_root != "$owner" ]]; then
            report "$file" "$line_number" "<$spelled>: belongs to another module; write <aloe/${module_name_of_root[$header_root]}>"
        fi
    done < <(contents_of "$file")
done

if [[ $violations -gt 0 ]]; then
    echo "$violations include(s) break the rules in codestyle.md, \"Include Organization\"" >&2
    exit 1
fi

$staged || echo "includes follow the rules: ${#files[@]} files"
