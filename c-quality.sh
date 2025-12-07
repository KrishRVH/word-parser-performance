#!/usr/bin/env bash
#
# c-quality.sh
#
# Run C quality passes over a project:
#   - clang-format (in-place)
#   - clang-tidy (uses .clang-tidy if present)
#   - cppcheck
#
# Usage:
#   ./c-quality.sh [SOURCE_ROOT]
#
# SOURCE_ROOT defaults to current directory.
# Run from project root, not build directory.
# Parallelism defaults to nproc; override with JOBS=n.

set -euo pipefail

readonly JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

note()  { printf '\033[0;34m[INFO]\033[0m %s\n' "$*" >&2; }
warn()  { printf '\033[0;33m[WARN]\033[0m %s\n' "$*" >&2; }
fail()  { printf '\033[0;31m[FAIL]\033[0m %s\n' "$*" >&2; }
fatal() { fail "$@"; exit 1; }

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || fatal "Required tool not found: $1"
}

find_config_root() {
    # Walk up from $1 looking for $2 (e.g., compile_commands.json or .clang-tidy)
    local dir="$1" target="$2"
    while [[ "$dir" != "/" ]]; do
        if [[ -f "$dir/$target" ]]; then
            printf '%s\n' "$dir"
            return 0
        fi
        dir=$(dirname "$dir")
    done
    return 1
}

collect_files() {
    # $1 = root, $2... = patterns (e.g., '*.c' '*.h')
    # Excludes common build directories and CMake artifacts
    local root="$1"; shift

    # Build -name pattern arguments properly
    local -a name_args=()
    local first=1
    for pat in "$@"; do
        ((first)) && first=0 || name_args+=(-o)
        name_args+=(-name "$pat")
    done

    find "$root" \
        -type d \( \
            -name 'build' -o \
            -name 'cmake-build-*' -o \
            -name 'CMakeFiles' -o \
            -name '.git' \
        \) -prune -o \
        -type f \( "${name_args[@]}" \) -print0
}

run_clang_format() {
    local root="$1"
    note "Running clang-format (jobs=$JOBS)"

    local count
    count=$(collect_files "$root" '*.c' '*.h' | tr -cd '\0' | wc -c)

    if ((count == 0)); then
        warn "No .c/.h files found for clang-format."
        return 0
    fi

    collect_files "$root" '*.c' '*.h' \
        | xargs -0 -P "$JOBS" clang-format -i

    note "clang-format: formatted $count files."
}

run_clang_tidy() {
    local root="$1"
    note "Running clang-tidy (jobs=$JOBS)"

    local -a c_files
    mapfile -d '' c_files < <(collect_files "$root" '*.c')

    if ((${#c_files[@]} == 0)); then
        warn "No .c files found for clang-tidy."
        return 0
    fi

    local -a tidy_args=()

    # Use compile_commands.json if available
    local cdb_root
    if cdb_root=$(find_config_root "$root" compile_commands.json); then
        note "Using compile_commands.json from: $cdb_root"
        tidy_args+=(-p "$cdb_root")
    else
        warn "No compile_commands.json; using fallback flags."
        # Append fallback flags after '--' below
    fi

    # Let .clang-tidy define checks; only override if missing
    local tidy_root
    if ! tidy_root=$(find_config_root "$root" .clang-tidy); then
        warn "No .clang-tidy found; using default C-focused checks."
        tidy_args+=(
            -checks='-*,clang-analyzer-*,bugprone-*,portability-*'
        )
    fi

    tidy_args+=(-header-filter='.*')

    local tidy_errors=0

    run_tidy_on_file() {
    local f="$1"
    shift
    local -a args=("$@")

    if [[ ! " ${args[*]} " =~ " -p " ]]; then
        fatal "No compile_commands.json found; generate with cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
    fi
    clang-tidy "${args[@]}" "$f"
	}
    export -f run_tidy_on_file

    # Run in parallel; collect exit status
    printf '%s\0' "${c_files[@]}" \
        | xargs -0 -P "$JOBS" -I{} bash -c 'run_tidy_on_file "$@"' _ {} "${tidy_args[@]}" \
        || tidy_errors=1

    if ((tidy_errors)); then
        warn "clang-tidy reported issues."
    else
        note "clang-tidy: completed for ${#c_files[@]} files."
    fi

    return $tidy_errors
}

run_cppcheck() {
    local root="$1"
    note "Running cppcheck (jobs=$JOBS)"

    local cdb_root
    if cdb_root=$(find_config_root "$root" compile_commands.json); then
        note "Using compile_commands.json for cppcheck"
        cppcheck \
            --project="$cdb_root/compile_commands.json" \
            --enable=warning,style,performance,portability \
            --inconclusive \
			--inline-suppr \
            --suppress=missingIncludeSystem \
            --suppress=unmatchedSuppression \
            --quiet \
            --check-level=exhaustive \
            --error-exitcode=1 \
            -j "$JOBS" \
        || { warn "cppcheck reported issues."; return 1; }
    else
		# Collect files the same way as other tools
		local -a files
		mapfile -d '' files < <(collect_files "$root" '*.c' '*.h')

		if ((${#files[@]} == 0)); then
			warn "No .c/.h files found for cppcheck."
			return 0
		fi

		# cppcheck quirks:
		#   - unusedFunction incompatible with -j
		#   - toomanyconfigs/checkersReport are info noise
		printf '%s\0' "${files[@]}" | xargs -0 \
			cppcheck \
				--enable=warning,style,performance,portability \
				--inconclusive \
				--std=c11 \
				--platform=unix64 \
				--suppress=missingIncludeSystem \
				--suppress=unmatchedSuppression \
				--suppress=knownConditionTrueFalse \
				--quiet \
				--error-exitcode=1 \
				-j "$JOBS" \
			|| { warn "cppcheck reported issues."; return 1; }

		note "cppcheck: completed for ${#files[@]} files."
	fi
}

main() {
    require_cmd clang-format
    require_cmd clang-tidy
    require_cmd cppcheck

    local src_root="${1:-.}"
    src_root=$(cd "$src_root" && pwd)

    note "Source root: $src_root"
    note "Parallelism: $JOBS jobs"

    local exit_code=0

    run_clang_format "$src_root"
    run_clang_tidy "$src_root"   || exit_code=1
    run_cppcheck "$src_root"     || exit_code=1

    if ((exit_code == 0)); then
        note "All C quality passes completed successfully."
    else
        fail "Some quality checks reported issues (see above)."
    fi

    return $exit_code
}

main "$@"
