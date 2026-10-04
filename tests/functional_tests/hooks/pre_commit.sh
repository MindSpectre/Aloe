#!/usr/bin/env bash
#
# Usage: pre_commit.sh <scenario> <repository root> <scratch dir>
#
# Runs one scenario of .githooks/pre-commit against a throwaway git repository that holds
# copies of the hook, scripts/check-format.sh and .clang-format. Exits non-zero, with the
# reason on stderr, when the hook behaves differently from what the scenario expects.

set -euo pipefail

scenario=${1:?scenario}
mkdir -p -- "${3:?scratch dir}"
source_root=$(cd -- "${2:?repository root}" && pwd)
work_dir=$(cd -- "$3" && pwd)
git_bin=$(type -P git)

# Neither the developer's git configuration (a global core.hooksPath, commit signing) nor the
# system one may change what a scenario sees.
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME=probe GIT_AUTHOR_EMAIL=probe@example.invalid
export GIT_COMMITTER_NAME=probe GIT_COMMITTER_EMAIL=probe@example.invalid

unformatted='int   add( int a,int b ){return a+b;}'
formatted='int add(int a, int b) {
    return a + b;
}'

fail() {
    echo "FAIL [$scenario]: $*" >&2
    exit 1
}

setup_repository() {
    [[ -x $source_root/.githooks/pre-commit ]] || fail ".githooks/pre-commit is missing or not executable"
    rm -rf -- "$work_dir"
    mkdir -p -- "$work_dir/repo/scripts" "$work_dir/repo/.githooks"
    cd -- "$work_dir/repo"
    cp -- "$source_root/.clang-format" .
    cp -- "$source_root/scripts/check-format.sh" scripts/
    cp -- "$source_root/.githooks/pre-commit" .githooks/
    git init --quiet
    git config core.hooksPath .githooks
}

# write_file <path> <content>
write_file() {
    mkdir -p -- "$(dirname -- "$1")"
    printf '%s\n' "$2" >"$1"
}

# try_commit [PATH]: commits whatever is staged and records the outcome in commit_status and
# commit_output. The optional argument replaces PATH for the commit, and so for the hook.
try_commit() {
    local search_path=${1:-$PATH}
    if commit_output=$(env PATH="$search_path" "$git_bin" commit --quiet --message probe 2>&1); then
        commit_status=0
    else
        commit_status=$?
    fi
}

expect_committed() {
    [[ $commit_status -eq 0 ]] || fail "the commit was rejected:"$'\n'"$commit_output"
    git rev-parse --verify --quiet HEAD >/dev/null || fail "no commit was made"
}

expect_rejected() {
    [[ $commit_status -ne 0 ]] || fail "the commit went through"
    ! git rev-parse --verify --quiet HEAD >/dev/null || fail "a commit was made despite the rejection"
}

expect_output_contains() {
    [[ $commit_output == *"$1"* ]] || fail "the output does not contain [$1]:"$'\n'"$commit_output"
}

# A PATH that has what the hook and the format script need, and no clang-format.
path_without_clang_format() {
    mkdir -p -- "$work_dir/bin"
    local tool tool_path
    for tool in bash sh env dirname basename find sort cat tr sed grep mkdir rm cp mv mktemp; do
        tool_path=$(type -P "$tool") || continue
        ln -s -- "$tool_path" "$work_dir/bin/$tool"
    done
    printf '%s' "$work_dir/bin"
}

setup_repository

case $scenario in
    rejects_unformatted_staged_files)
        write_file common/core/bad.cpp "$unformatted"
        write_file common/tcp/bad.hpp "$unformatted"
        git add common/core/bad.cpp common/tcp/bad.hpp
        try_commit
        expect_rejected
        expect_output_contains "common/core/bad.cpp"
        expect_output_contains "common/tcp/bad.hpp"
        expect_output_contains "clang-format -i common/core/bad.cpp common/tcp/bad.hpp"
        ;;
    accepts_formatted_staged_files)
        write_file common/core/good.cpp "$formatted"
        git add common/core/good.cpp
        try_commit
        expect_committed
        ;;
    ignores_files_that_are_not_staged)
        write_file common/core/good.cpp "$formatted"
        write_file common/core/bad.cpp "$unformatted"
        git add common/core/good.cpp
        try_commit
        expect_committed
        ;;
    ignores_files_outside_the_checked_sources)
        write_file scratch/bad.cpp "$unformatted"
        write_file common/core/notes.txt "$unformatted"
        git add scratch/bad.cpp common/core/notes.txt
        try_commit
        expect_committed
        ;;
    judges_the_staged_content_not_the_working_tree)
        write_file common/core/half.cpp "$unformatted"
        git add common/core/half.cpp
        write_file common/core/half.cpp "$formatted"
        try_commit
        expect_rejected
        expect_output_contains "common/core/half.cpp"
        ;;
    ignores_working_tree_edits_made_after_staging)
        write_file common/core/half.cpp "$formatted"
        git add common/core/half.cpp
        write_file common/core/half.cpp "$unformatted"
        try_commit
        expect_committed
        ;;
    allows_deleting_a_source)
        write_file common/core/good.cpp "$formatted"
        git add common/core/good.cpp
        try_commit
        expect_committed
        git rm --quiet common/core/good.cpp
        try_commit
        expect_committed
        [[ $(git rev-list --count HEAD) -eq 2 ]] || fail "the deletion was not committed"
        ;;
    missing_clang_format_blocks_a_source_commit)
        write_file common/core/good.cpp "$formatted"
        git add common/core/good.cpp
        try_commit "$(path_without_clang_format)"
        expect_rejected
        expect_output_contains "clang-format"
        expect_output_contains "install-linux.sh"
        ;;
    missing_clang_format_does_not_block_a_docs_commit)
        write_file docs/notes.md "no C++ in here"
        git add docs/notes.md
        try_commit "$(path_without_clang_format)"
        expect_committed
        ;;
    whole_tree_mode_checks_files_that_are_not_staged)
        write_file common/core/bad.cpp "$unformatted"
        if scripts/check-format.sh >/dev/null 2>&1; then
            fail "the whole-tree check passed with an unformatted file in the tree"
        fi
        write_file common/core/bad.cpp "$formatted"
        scripts/check-format.sh >/dev/null 2>&1 || fail "the whole-tree check failed on a formatted tree"
        ;;
    *)
        fail "unknown scenario"
        ;;
esac
