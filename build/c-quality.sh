#!/usr/bin/env bash
#
# c-quality.sh
#
# Run C quality passes over a project:
#   - clang-format (in-place)
#   - clang-tidy
#   - cppcheck
#
# Usage:
#   ./c-quality.sh [SOURCE_ROOT]
#
# SOURCE_ROOT defaults to current directory.

set -euo pipefail

note()  { printf '[INFO] %s\n' "$*" >&2; }
warn()  { printf '[WARN] %s\n' "$*" >&2; }
fatal() { printf '[FAIL] %s\n' "$*" >&2; exit 1; }

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || fatal "Required tool not found: $1"
}

find_compile_commands_root() {
    # Walk up from SRC_ROOT to / looking for compile_commands.json
    local dir="$1"
    while [[ "$dir" != "/" ]]; do
        if [[ -f "$dir/compile_commands.json" ]]; then
            printf '%s\n' "$dir"
            return 0
        fi
        dir=$(dirname "$dir")
    done
    return 1
}

run_clang_format() {
    local root="$1"
    note "Running clang-format on .c/.h files under: $root"

    mapfile -t files < <(
        find "$root" -type f \( -name '*.c' -o -name '*.h' \)
    )

    if (( ${#files[@]} == 0 )); then
        warn "No .c/.h files found for clang-format."
        return 0
    fi

    clang-format -i "${files[@]}"
    note "clang-format: formatted ${#files[@]} files."
}

run_clang_tidy() {
    local root="$1"
    note "Running clang-tidy on .c files under: $root"

    mapfile -t c_files < <(find "$root" -type f -name '*.c')
    if (( ${#c_files[@]} == 0 )); then
        warn "No .c files found for clang-tidy."
        return 0
    fi

    # Arguments common to all invocations (e.g., -p for compile_commands.json)
    local -a tidy_common_args=()
    # Extra compiler flags for the fallback (no compile_commands.json) case
    local -a tidy_compile_args=()

    local cdb_root
    if cdb_root=$(find_compile_commands_root "$root"); then
        note "Using compile_commands.json from: $cdb_root"
        tidy_common_args=(-p "$cdb_root")
    else
        warn "No compile_commands.json found; using simple C11 fallback flags."
        # These will be passed after '--' *after* the filename.
        tidy_compile_args=(-std=c11 -Wall -Wextra -Wpedantic)
    fi

    # C-oriented check set: avoid C++-only noise.
    local checks='-*,clang-analyzer-*,bugprone-*,readability-*,portability-*'
    local header_filter='.*'

    for f in "${c_files[@]}"; do
        note "clang-tidy: $f"
        if (( ${#tidy_compile_args[@]} > 0 )); then
            clang-tidy \
                -checks="$checks" \
                -header-filter="$header_filter" \
                "${tidy_common_args[@]}" \
                "$f" \
                -- "${tidy_compile_args[@]}"
        else
            clang-tidy \
                -checks="$checks" \
                -header-filter="$header_filter" \
                "${tidy_common_args[@]}" \
                "$f"
        fi
    done

    note "clang-tidy: completed for ${#c_files[@]} files."
}

run_cppcheck() {
    local root="$1"
    note "Running cppcheck on: $root"

    # cppcheck can be noisy about system headers; suppress that.
    cppcheck \
        --enable=all \
        --inconclusive \
        --std=c11 \
        --platform=unix64 \
        --suppress=missingIncludeSystem \
        --quiet \
        "$root"

    note "cppcheck: completed."
}

main() {
    require_cmd clang-format
    require_cmd clang-tidy
    require_cmd cppcheck

    local src_root="${1:-.}"
    src_root=$(cd "$src_root" && pwd)

    note "Source root: $src_root"

    run_clang_format "$src_root"
    run_clang_tidy   "$src_root"
    run_cppcheck     "$src_root"

    note "All C quality passes completed successfully."
}

main "$@"