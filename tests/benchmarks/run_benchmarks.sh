#!/usr/bin/env bash
# Iron Benchmark Suite
# Compares Iron compiler output against C reference implementations.
# Exit 0 if all benchmarks pass their max_ratio threshold, exit 1 otherwise.

set -euo pipefail

# ── Resolve repo root ──────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PROBLEMS_DIR="$SCRIPT_DIR/problems"
IRONC="$REPO_ROOT/build/ironc"
TMPDIR="${TMPDIR:-/tmp}/iron_bench_$$"

# ── Parse arguments ────────────────────────────────────────────────────────
FILTER_PROBLEM=""
VERBOSE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --problem)
            FILTER_PROBLEM="$2"
            shift 2
            ;;
        --verbose|-v)
            VERBOSE=1
            shift
            ;;
        *)
            echo "Usage: $0 [--problem NAME] [--verbose]"
            exit 1
            ;;
    esac
done

# ── Preflight checks ──────────────────────────────────────────────────────
if [ ! -x "$IRONC" ]; then
    echo "ERROR: ironc not found at $IRONC"
    echo "Build the compiler first: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build"
    exit 1
fi

if ! command -v clang &>/dev/null; then
    echo "ERROR: clang not found in PATH"
    exit 1
fi

mkdir -p "$TMPDIR"
trap 'rm -rf "$TMPDIR"' EXIT

# ── Portable timeout wrapper (macOS lacks GNU timeout) ────────────────────
# Runs a command with a timeout, writing stdout+stderr to the given output file.
# Returns the command's exit code, or 124 on timeout.
run_with_timeout() {
    local secs="$1"
    local outfile="$2"
    shift 2
    if command -v timeout &>/dev/null; then
        timeout "${secs}s" "$@" >"$outfile" 2>&1
    elif command -v gtimeout &>/dev/null; then
        gtimeout "${secs}s" "$@" >"$outfile" 2>&1
    else
        # Fallback: background process with kill after timeout
        "$@" >"$outfile" 2>&1 &
        local pid=$!
        ( sleep "$secs" && kill -9 "$pid" 2>/dev/null ) &
        local watchdog=$!
        if wait "$pid" 2>/dev/null; then
            kill "$watchdog" 2>/dev/null
            wait "$watchdog" 2>/dev/null
            return 0
        else
            local rc=$?
            kill "$watchdog" 2>/dev/null
            wait "$watchdog" 2>/dev/null
            return $rc
        fi
    fi
}

# ── JSON config reader (jq with grep/sed fallback) ────────────────────────
read_config() {
    local config_file="$1"
    local key="$2"
    if command -v jq &>/dev/null; then
        jq -r ".$key" "$config_file"
    else
        # Fallback: simple grep/sed for flat JSON
        sed 's/[{},]/ /g' "$config_file" | tr ' ' '\n' | \
            grep -A1 "\"$key\"" | tail -1 | tr -d ' "'
    fi
}

# ── Extract "Total time: X ms" from output ────────────────────────────────
extract_time_ms() {
    local output="$1"
    # Match "Total time: <number> ms" — Iron prints integer, C prints float
    echo "$output" | grep -oE 'Total time: [0-9]+(\.[0-9]+)? ms' | \
        grep -oE '[0-9]+(\.[0-9]+)?' | head -1
}

# ── Verify correctness against expected_output.txt ─────────────────────────
check_correctness() {
    local actual_output="$1"
    local expected_file="$2"
    while IFS= read -r expected_line; do
        [ -z "$expected_line" ] && continue
        if ! echo "$actual_output" | grep -qF "$expected_line"; then
            echo "  CORRECTNESS FAILURE: expected line not found: $expected_line"
            return 1
        fi
    done < "$expected_file"
    return 0
}

# ── Main benchmark loop ──────────────────────────────────────────────────
echo "=== Iron Benchmark Suite ==="
echo ""

total=0
passed=0
failed=0
errors=0
results=()

for problem_dir in "$PROBLEMS_DIR"/*/; do
    problem_name="$(basename "$problem_dir")"

    # Apply filter if --problem was specified
    if [ -n "$FILTER_PROBLEM" ] && [ "$problem_name" != "$FILTER_PROBLEM" ]; then
        continue
    fi

    total=$((total + 1))
    config_file="$problem_dir/config.json"
    iron_src="$problem_dir/main.iron"
    c_src="$problem_dir/solution.c"
    expected_file="$problem_dir/expected_output.txt"

    timeout_sec=$(read_config "$config_file" "timeout_sec")
    max_ratio=$(read_config "$config_file" "max_ratio")

    c_bin="$TMPDIR/bench_c_${problem_name}"
    iron_build_dir="$TMPDIR/iron_build_${problem_name}"
    mkdir -p "$iron_build_dir"

    # ── Step 1: Compile C reference ────────────────────────────────────
    if ! clang -std=c17 -O3 -o "$c_bin" "$c_src" -lm 2>"$TMPDIR/c_compile_err"; then
        echo "[ERROR] $problem_name: C compilation failed"
        if [ $VERBOSE -eq 1 ]; then
            cat "$TMPDIR/c_compile_err"
        fi
        errors=$((errors + 1))
        results+=("[ERROR] $problem_name: C compilation failed")
        continue
    fi

    # ── Step 2: Build Iron ─────────────────────────────────────────────
    # ironc outputs binary to cwd with basename of source file, so we cd to a temp dir
    if ! (cd "$iron_build_dir" && "$IRONC" build "$iron_src" 2>"$TMPDIR/iron_compile_err"); then
        echo "[ERROR] $problem_name: Iron compilation failed"
        if [ $VERBOSE -eq 1 ]; then
            cat "$TMPDIR/iron_compile_err"
        fi
        errors=$((errors + 1))
        results+=("[ERROR] $problem_name: Iron compilation failed")
        continue
    fi
    iron_bin="$iron_build_dir/main"
    if [ ! -x "$iron_bin" ]; then
        echo "[ERROR] $problem_name: Iron binary not found after compilation"
        errors=$((errors + 1))
        results+=("[ERROR] $problem_name: Iron binary not produced")
        continue
    fi

    # ── Step 3: Run C reference ────────────────────────────────────────
    if ! run_with_timeout "$timeout_sec" "$TMPDIR/c_output" "$c_bin"; then
        echo "[ERROR] $problem_name: C execution failed or timed out"
        errors=$((errors + 1))
        results+=("[ERROR] $problem_name: C execution failed (timeout: ${timeout_sec}s)")
        continue
    fi
    c_output=$(cat "$TMPDIR/c_output")

    # ── Step 4: Run Iron binary ────────────────────────────────────────
    if ! run_with_timeout "$timeout_sec" "$TMPDIR/iron_output" "$iron_bin"; then
        echo "[ERROR] $problem_name: Iron execution failed or timed out"
        errors=$((errors + 1))
        results+=("[ERROR] $problem_name: Iron execution failed (timeout: ${timeout_sec}s)")
        continue
    fi
    iron_output=$(cat "$TMPDIR/iron_output")

    # ── Step 5: Verify correctness ─────────────────────────────────────
    if [ -f "$expected_file" ]; then
        if ! check_correctness "$iron_output" "$expected_file"; then
            echo "[ERROR] $problem_name: Iron output incorrect"
            if [ $VERBOSE -eq 1 ]; then
                echo "  Iron output:"
                echo "$iron_output" | head -5
            fi
            errors=$((errors + 1))
            results+=("[ERROR] $problem_name: incorrect output")
            continue
        fi
    fi

    # ── Step 6: Extract timing ─────────────────────────────────────────
    c_ms=$(extract_time_ms "$c_output")
    iron_ms=$(extract_time_ms "$iron_output")

    if [ -z "$c_ms" ] || [ -z "$iron_ms" ]; then
        echo "[ERROR] $problem_name: Could not extract timing from output"
        if [ $VERBOSE -eq 1 ]; then
            echo "  C output:    $(echo "$c_output" | grep -i 'total time' || echo '(none)')"
            echo "  Iron output: $(echo "$iron_output" | grep -i 'total time' || echo '(none)')"
        fi
        errors=$((errors + 1))
        results+=("[ERROR] $problem_name: timing extraction failed")
        continue
    fi

    # ── Step 7: Compute ratio and compare ──────────────────────────────
    # Use awk for floating-point arithmetic
    ratio=$(awk "BEGIN { printf \"%.1f\", $iron_ms / $c_ms }")
    pass=$(awk "BEGIN { print ($iron_ms / $c_ms <= $max_ratio) ? 1 : 0 }")

    # Round times for display
    c_display=$(awk "BEGIN { printf \"%d\", $c_ms + 0.5 }")
    iron_display=$(awk "BEGIN { printf \"%d\", $iron_ms + 0.5 }")

    if [ "$pass" -eq 1 ]; then
        passed=$((passed + 1))
        results+=("[PASS] $problem_name: ${ratio}x (threshold: ${max_ratio}x) - C: ${c_display}ms, Iron: ${iron_display}ms")
    else
        failed=$((failed + 1))
        results+=("[FAIL] $problem_name: ${ratio}x (threshold: ${max_ratio}x) - C: ${c_display}ms, Iron: ${iron_display}ms")
    fi
done

# ── Print results ──────────────────────────────────────────────────────────
for r in "${results[@]}"; do
    echo "$r"
done

echo ""
echo "Results: $passed/$total passed ($failed failed, $errors errors)"

if [ "$failed" -gt 0 ] || [ "$errors" -gt 0 ]; then
    exit 1
fi
exit 0
