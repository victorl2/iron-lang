#!/usr/bin/env bash
# Iron Benchmark Suite
# Compares Iron compiler output against C reference implementations.
# Supports memory tracking, baseline saving, and regression detection.
# Exit 0 if all benchmarks pass their max_ratio threshold, exit 1 otherwise.

set -euo pipefail

# ── Resolve repo root ──────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PROBLEMS_DIR="$SCRIPT_DIR/problems"
BASELINES_DIR="$SCRIPT_DIR/baselines"
IRONC="$REPO_ROOT/build/ironc"
TMPDIR="${TMPDIR:-/tmp}/iron_bench_$$"

# Number of timing samples per binary. Runs are interleaved [C, Iron, C, Iron, ...]
# and the median of each side is used to compute the speed ratio. This design
# absorbs CI scheduler noise that plagues single-sample benchmarks on
# shared-tenant runners (GitHub-hosted ubuntu/macos).
SAMPLES=30

# ── Parse arguments ────────────────────────────────────────────────────────
FILTER_PROBLEM=""
VERBOSE=0
SAVE_BASELINE=0
CHECK_REGRESSION=0
WRITE_JSON=0
JSON_FILE=""
COMPARE_MODE=0
# --only-parallel / --skip-parallel split the suite into two groups so the CI
# workflow can run them as separate jobs concurrently. A benchmark is considered
# "parallel" if its directory name starts with parallel_, concurrency_, or spawn_.
GROUP_FILTER="all"

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
        --save-baseline)
            SAVE_BASELINE=1
            shift
            ;;
        --check-regression)
            CHECK_REGRESSION=1
            shift
            ;;
        --json)
            WRITE_JSON=1
            JSON_FILE="${2:-results.json}"
            shift 2
            ;;
        --compare)
            COMPARE_MODE=1
            shift
            ;;
        --only-parallel)
            GROUP_FILTER="parallel"
            shift
            ;;
        --skip-parallel)
            GROUP_FILTER="sequential"
            shift
            ;;
        *)
            echo "Usage: $0 [--problem NAME] [--verbose] [--save-baseline] [--check-regression] [--json FILE] [--compare] [--only-parallel | --skip-parallel]"
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
CC="clang"

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

# ── Run with memory tracking via /usr/bin/time ─────────────────────────────
# Runs a command, captures stdout to outfile, and extracts peak RSS in KB.
# Writes memory (KB) to memfile.
run_with_memory() {
    local secs="$1"
    local outfile="$2"
    local memfile="$3"
    shift 3

    local timefile="$TMPDIR/time_output_$$_$RANDOM"

    if [ -x /usr/bin/time ]; then
        # macOS: /usr/bin/time -l outputs "maximum resident set size" in bytes
        # Linux: /usr/bin/time -v outputs "Maximum resident set size (kbytes)"
        if /usr/bin/time -l true 2>/dev/null; then
            # macOS style
            /usr/bin/time -l "$@" >"$outfile" 2>"$timefile"
            local rc=$?
            # macOS reports bytes; extract and convert to KB
            local mem_bytes
            mem_bytes=$(grep "maximum resident set size" "$timefile" | grep -oE '[0-9]+' | head -1 || echo "0")
            if [ -n "$mem_bytes" ] && [ "$mem_bytes" -gt 0 ]; then
                echo $(( mem_bytes / 1024 )) > "$memfile"
            else
                echo "0" > "$memfile"
            fi
            rm -f "$timefile"
            return $rc
        else
            # Linux style
            /usr/bin/time -v "$@" >"$outfile" 2>"$timefile"
            local rc=$?
            local mem_kb
            mem_kb=$(grep "Maximum resident set size" "$timefile" | grep -oE '[0-9]+' | head -1 || echo "0")
            echo "${mem_kb:-0}" > "$memfile"
            rm -f "$timefile"
            return $rc
        fi
    else
        # No /usr/bin/time available, fall back to timeout only
        run_with_timeout "$secs" "$outfile" "$@"
        local rc=$?
        echo "0" > "$memfile"
        return $rc
    fi
}

# ── JSON config reader (jq with grep/sed fallback) ────────────────────────
read_config() {
    local config_file="$1"
    local key="$2"
    if command -v jq &>/dev/null; then
        jq -r ".$key // empty" "$config_file"
    else
        # Fallback: simple grep/sed for flat JSON
        sed 's/[{},]/ /g' "$config_file" | tr ' ' '\n' | \
            grep -A1 "\"$key\"" | tail -1 | tr -d ' "'
    fi
}

# ── Extract "Total time: X" (ns preferred, ms fallback) ──────────────────
# Phase 58: Iron benchmarks print BOTH a ns line and an ms line; C benchmarks
# print only a decimal ms line. This helper prefers the ns line when present
# (sub-ms precision, eliminates integer-ms quantization) and normalizes the
# result to float ms internally so the rest of the runner compares apples
# to apples against C's "%.3f ms" output.
extract_time_ms() {
    local output="$1"
    # 1) First preference: "Total time: <integer> ns" line (Iron Phase 58+)
    local ns_val
    ns_val=$(echo "$output" | grep -oE 'Total time: [0-9]+ ns' | \
        grep -oE '[0-9]+' | head -1)
    if [ -n "$ns_val" ]; then
        # Normalize ns -> ms with 6-decimal precision (microsecond granularity)
        awk "BEGIN { printf \"%.6f\", $ns_val / 1000000.0 }"
        return 0
    fi
    # 2) Fallback: "Total time: <number> ms" line (C reference, pre-Phase-58 Iron)
    echo "$output" | grep -oE 'Total time: [0-9]+(\.[0-9]+)? ms' | \
        grep -oE '[0-9]+(\.[0-9]+)?' | head -1
}

# ── Median of a list of floats (passed as positional args) ────────────────
# Sorts numerically and returns the middle element, or average of the two
# middle elements for even-length lists. Used to collapse SAMPLES timing
# samples into a single robust value before computing the Iron/C ratio.
median() {
    printf '%s\n' "$@" | sort -n | awk '
        { v[NR] = $1 }
        END {
            n = NR
            if (n == 0) { print "0"; exit }
            if (n % 2 == 1) { printf "%.6f\n", v[(n+1)/2] }
            else { printf "%.6f\n", (v[n/2] + v[n/2+1]) / 2 }
        }
    '
}

# ── Parallel-group membership ──────────────────────────────────────────────
# A benchmark belongs to the "parallel" group if its directory name starts with
# parallel_, concurrency_, or spawn_. The CI workflow runs parallel and
# sequential groups as two concurrent jobs per OS so the thread-heavy
# benchmarks do not serialize the suite wall-time — and so the sequential
# benchmarks are not timed on a CPU being hammered by the parallel ones.
is_parallel_benchmark() {
    case "$1" in
        parallel_*|concurrency_*|spawn_*) return 0 ;;
        *) return 1 ;;
    esac
}

# Returns 0 if the benchmark should be included under the current GROUP_FILTER.
benchmark_matches_group() {
    local name="$1"
    case "$GROUP_FILTER" in
        all) return 0 ;;
        parallel) is_parallel_benchmark "$name" ;;
        sequential) if is_parallel_benchmark "$name"; then return 1; else return 0; fi ;;
    esac
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
case "$GROUP_FILTER" in
    parallel)   echo "Group: parallel (parallel_*, concurrency_*, spawn_*)" ;;
    sequential) echo "Group: sequential (excluding parallel_*, concurrency_*, spawn_*)" ;;
    *)          echo "Group: all" ;;
esac
echo ""

total=0
passed=0
failed=0
errors=0
skipped=0
results=()

# JSON accumulator for baseline
json_problems=""

# JSON accumulator for --json output
json_results=""

for problem_dir in "$PROBLEMS_DIR"/*/; do
    problem_name="$(basename "$problem_dir")"

    # Apply filter if --problem was specified
    if [ -n "$FILTER_PROBLEM" ] && [ "$problem_name" != "$FILTER_PROBLEM" ]; then
        continue
    fi

    # Apply group filter (--only-parallel / --skip-parallel)
    if ! benchmark_matches_group "$problem_name"; then
        continue
    fi

    total=$((total + 1))
    config_file="$problem_dir/config.json"
    iron_src="$problem_dir/main.iron"
    c_src="$problem_dir/solution.c"
    expected_file="$problem_dir/expected_output.txt"

    timeout_sec=$(read_config "$config_file" "timeout_sec")
    max_ratio=$(read_config "$config_file" "max_ratio")

    # Check for skip field
    skip_val=$(read_config "$config_file" "skip" 2>/dev/null || echo "")
    if [ "$skip_val" = "true" ]; then
        skipped=$((skipped + 1))
        results+=("[SKIP] $problem_name: marked as skip in config")
        continue
    fi

    c_bin="$TMPDIR/bench_c_${problem_name}"
    iron_build_dir="$TMPDIR/iron_build_${problem_name}"
    mkdir -p "$iron_build_dir"

    # ── Step 1: Compile C reference ────────────────────────────────────
    if ! $CC -std=gnu17 -O3 -o "$c_bin" "$c_src" -lm -lpthread 2>"$TMPDIR/c_compile_err"; then
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

    # ── Step 2b: Build Iron without optimization (compare mode) ─────────
    iron_noopt_bin=""
    if [ $COMPARE_MODE -eq 1 ]; then
        iron_noopt_dir="$TMPDIR/iron_noopt_${problem_name}"
        mkdir -p "$iron_noopt_dir"
        if (cd "$iron_noopt_dir" && "$IRONC" build --no-optimize "$iron_src" 2>/dev/null); then
            iron_noopt_bin="$iron_noopt_dir/main"
        fi
    fi

    # ── Steps 3-5: Interleaved sampling + correctness ──────────────────
    # Run each binary SAMPLES times, alternating [C, Iron, C, Iron, ...] so
    # both sides experience the same scheduler/cache conditions at each pair.
    # Sample 0 also measures peak RSS (via /usr/bin/time) and verifies Iron's
    # output against expected_output.txt. Subsequent samples are timing-only.
    c_times=()
    iron_times=()
    c_mem_kb=0
    iron_mem_kb=0
    sample_status="ok"

    for ((s = 0; s < SAMPLES; s++)); do
        # ── C sample ──
        if [ "$s" -eq 0 ]; then
            c_mem_file="$TMPDIR/c_mem_${problem_name}"
            if ! run_with_memory "$timeout_sec" "$TMPDIR/c_output" "$c_mem_file" "$c_bin"; then
                sample_status="c_exec_fail"
                break
            fi
            c_mem_kb=$(cat "$c_mem_file" 2>/dev/null || echo "0")
        else
            if ! run_with_timeout "$timeout_sec" "$TMPDIR/c_output" "$c_bin"; then
                sample_status="c_exec_fail"
                break
            fi
        fi
        c_output=$(cat "$TMPDIR/c_output")
        c_sample_ms=$(extract_time_ms "$c_output")
        if [ -z "$c_sample_ms" ]; then
            sample_status="c_timing_fail"
            break
        fi
        c_times+=("$c_sample_ms")

        # ── Iron sample ──
        if [ "$s" -eq 0 ]; then
            iron_mem_file="$TMPDIR/iron_mem_${problem_name}"
            if ! run_with_memory "$timeout_sec" "$TMPDIR/iron_output" "$iron_mem_file" "$iron_bin"; then
                sample_status="iron_exec_fail"
                break
            fi
            iron_mem_kb=$(cat "$iron_mem_file" 2>/dev/null || echo "0")
        else
            if ! run_with_timeout "$timeout_sec" "$TMPDIR/iron_output" "$iron_bin"; then
                sample_status="iron_exec_fail"
                break
            fi
        fi
        iron_output=$(cat "$TMPDIR/iron_output")

        # Correctness check on sample 0 only (output is deterministic).
        if [ "$s" -eq 0 ] && [ -f "$expected_file" ]; then
            if ! check_correctness "$iron_output" "$expected_file"; then
                echo "[ERROR] $problem_name: Iron output incorrect"
                if [ $VERBOSE -eq 1 ]; then
                    echo "  Iron output:"
                    echo "$iron_output" | head -5
                fi
                sample_status="correctness_fail"
                break
            fi
        fi

        iron_sample_ms=$(extract_time_ms "$iron_output")
        if [ -z "$iron_sample_ms" ]; then
            sample_status="iron_timing_fail"
            break
        fi
        iron_times+=("$iron_sample_ms")
    done

    case "$sample_status" in
        c_exec_fail)
            echo "[ERROR] $problem_name: C execution failed or timed out"
            errors=$((errors + 1))
            results+=("[ERROR] $problem_name: C execution failed (timeout: ${timeout_sec}s)")
            continue
            ;;
        iron_exec_fail)
            echo "[ERROR] $problem_name: Iron execution failed or timed out"
            errors=$((errors + 1))
            results+=("[ERROR] $problem_name: Iron execution failed (timeout: ${timeout_sec}s)")
            continue
            ;;
        c_timing_fail|iron_timing_fail)
            echo "[ERROR] $problem_name: Could not extract timing from output"
            if [ $VERBOSE -eq 1 ]; then
                echo "  C output:    $(echo "$c_output" | grep -i 'total time' || echo '(none)')"
                echo "  Iron output: $(echo "$iron_output" | grep -i 'total time' || echo '(none)')"
            fi
            errors=$((errors + 1))
            results+=("[ERROR] $problem_name: timing extraction failed")
            continue
            ;;
        correctness_fail)
            errors=$((errors + 1))
            results+=("[ERROR] $problem_name: incorrect output")
            continue
            ;;
    esac

    # ── Step 6: Collapse samples to per-side medians ───────────────────
    c_ms=$(median "${c_times[@]}")
    iron_ms=$(median "${iron_times[@]}")

    # ── Step 6b: Run and time unoptimized version (compare mode) ────────
    iron_noopt_ms=""
    if [ $COMPARE_MODE -eq 1 ] && [ -n "$iron_noopt_bin" ] && [ -x "$iron_noopt_bin" ]; then
        if run_with_timeout "$timeout_sec" "$TMPDIR/iron_noopt_output" "$iron_noopt_bin"; then
            iron_noopt_output=$(cat "$TMPDIR/iron_noopt_output")
            iron_noopt_ms=$(extract_time_ms "$iron_noopt_output")
        fi
    fi

    # ── Step 7: Compute ratio and compare ──────────────────────────────
    # Use awk for floating-point arithmetic.
    # Treat C times < 0.001ms as 0.001ms to avoid division-by-zero artifacts.
    ratio=$(awk "BEGIN { c = ($c_ms < 0.001) ? 0.001 : $c_ms; printf \"%.1f\", $iron_ms / c }")
    pass=$(awk "BEGIN { c = ($c_ms < 0.001) ? 0.001 : $c_ms; print ($iron_ms / c <= $max_ratio) ? 1 : 0 }")

    # Memory ratio
    mem_ratio="n/a"
    if [ "$c_mem_kb" -gt 0 ] && [ "$iron_mem_kb" -gt 0 ] 2>/dev/null; then
        mem_ratio=$(awk "BEGIN { printf \"%.1f\", $iron_mem_kb / $c_mem_kb }")
    fi

    # Format times for display — show sub-ms precision for fast benchmarks
    c_display=$(awk "BEGIN { v = $c_ms; if (v < 1) printf \"%.2f\", v; else printf \"%d\", v + 0.5 }")
    iron_display=$(awk "BEGIN { v = $iron_ms; if (v < 1) printf \"%.2f\", v; else printf \"%d\", v + 0.5 }")

    # Append compare delta to result line
    compare_suffix=""
    if [ $COMPARE_MODE -eq 1 ] && [ -n "$iron_noopt_ms" ]; then
        noopt_ratio=$(awk "BEGIN { c = ($c_ms < 0.001) ? 0.001 : $c_ms; printf \"%.1f\", $iron_noopt_ms / c }")
        speedup_pct=$(awk "BEGIN { if ($iron_noopt_ms == 0) printf \"0.0\"; else printf \"%.1f\", ($iron_noopt_ms - $iron_ms) / $iron_noopt_ms * 100 }")
        compare_suffix=" [opt: ${ratio}x, noopt: ${noopt_ratio}x, speedup: ${speedup_pct}%]"
    fi

    if [ "$pass" -eq 1 ]; then
        passed=$((passed + 1))
        results+=("[PASS] $problem_name: ${ratio}x speed, ${mem_ratio}x memory (threshold: ${max_ratio}x) - C: ${c_display}ms/${c_mem_kb}KB, Iron: ${iron_display}ms/${iron_mem_kb}KB${compare_suffix}")
    else
        failed=$((failed + 1))
        results+=("[FAIL] $problem_name: ${ratio}x speed, ${mem_ratio}x memory (threshold: ${max_ratio}x) - C: ${c_display}ms/${c_mem_kb}KB, Iron: ${iron_display}ms/${iron_mem_kb}KB${compare_suffix}")
    fi

    # Accumulate JSON for --json output
    if [ $WRITE_JSON -eq 1 ]; then
        local_status="pass"
        [ "$pass" -ne 1 ] && local_status="fail"
        if [ -n "$json_results" ]; then
            json_results="${json_results},"
        fi
        json_results="${json_results}
    {\"name\":\"${problem_name}\",\"iron_ms\":${iron_ms},\"c_ms\":${c_ms},\"ratio\":${ratio},\"max_ratio\":${max_ratio},\"status\":\"${local_status}\",\"iron_mem_kb\":${iron_mem_kb},\"c_mem_kb\":${c_mem_kb}}"
    fi

    # Accumulate JSON for baseline
    if [ -n "$json_problems" ]; then
        json_problems="${json_problems},"
    fi
    json_problems="${json_problems}
    \"${problem_name}\": {\"c_ms\": ${c_ms}, \"iron_ms\": ${iron_ms}, \"ratio\": ${ratio}, \"iron_mem_kb\": ${iron_mem_kb}, \"c_mem_kb\": ${c_mem_kb}}"
done

# ── Print results ──────────────────────────────────────────────────────────
# Guarded to avoid `set -u` expansion errors when the group filter matches no
# benchmarks (e.g. `--only-parallel --problem <sequential-name>`).
if [ "${#results[@]}" -gt 0 ]; then
    for r in "${results[@]}"; do
        echo "$r"
    done
fi

echo ""
echo "Results: $passed/$total passed ($failed failed, $errors errors, $skipped skipped)"

# ── Write JSON results if requested ──────────────────────────────────────────
if [ $WRITE_JSON -eq 1 ]; then
    commit_hash=$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo "unknown")
    today=$(date +%Y-%m-%d)
    cat > "$JSON_FILE" <<JSON_EOF
{
  "date": "${today}",
  "commit": "${commit_hash}",
  "passed": ${passed},
  "failed": ${failed},
  "errors": ${errors},
  "total": ${total},
  "benchmarks": [${json_results}
  ]
}
JSON_EOF
    echo "JSON results written to $JSON_FILE"
fi

# ── Save baseline if requested ─────────────────────────────────────────────
if [ $SAVE_BASELINE -eq 1 ]; then
    mkdir -p "$BASELINES_DIR"
    commit_hash=$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo "unknown")
    today=$(date +%Y-%m-%d)

    baseline_file="$BASELINES_DIR/latest.json"
    cat > "$baseline_file" <<BASELINE_EOF
{
  "commit": "${commit_hash}",
  "date": "${today}",
  "problems": {${json_problems}
  }
}
BASELINE_EOF
    echo ""
    echo "Baseline saved to $baseline_file (commit: $commit_hash)"
fi

# ── Check regression against baseline ──────────────────────────────────────
if [ $CHECK_REGRESSION -eq 1 ]; then
    baseline_file="$BASELINES_DIR/latest.json"
    if [ ! -f "$baseline_file" ]; then
        echo ""
        echo "WARNING: No baseline found at $baseline_file"
        echo "Run with --save-baseline first to create one."
        exit 0
    fi

    echo ""
    echo "=== Regression Check (>5% slowdown = FAIL) ==="

    regression_found=0

    if command -v jq &>/dev/null; then
        for problem_dir in "$PROBLEMS_DIR"/*/; do
            problem_name="$(basename "$problem_dir")"

            if [ -n "$FILTER_PROBLEM" ] && [ "$problem_name" != "$FILTER_PROBLEM" ]; then
                continue
            fi

            if ! benchmark_matches_group "$problem_name"; then
                continue
            fi

            baseline_ratio=$(jq -r ".problems.\"${problem_name}\".ratio // empty" "$baseline_file" 2>/dev/null)
            if [ -z "$baseline_ratio" ]; then
                echo "  [NEW] $problem_name: no baseline entry"
                continue
            fi

            # Find current ratio from results
            current_ratio=""
            if [ "${#results[@]}" -gt 0 ]; then
                for r in "${results[@]}"; do
                    if echo "$r" | grep -q "$problem_name"; then
                        current_ratio=$(echo "$r" | grep -oE '[0-9]+\.[0-9]+x speed' | grep -oE '[0-9]+\.[0-9]+' | head -1)
                        break
                    fi
                done
            fi

            if [ -z "$current_ratio" ]; then
                echo "  [SKIP] $problem_name: no current result"
                continue
            fi

            # Check if current is >5% worse than baseline
            regression=$(awk "BEGIN {
                if ($baseline_ratio == 0) { print 0; exit }
                pct_change = ($current_ratio - $baseline_ratio) / $baseline_ratio * 100
                if (pct_change > 5.0) print 1; else print 0
            }")

            pct=$(awk "BEGIN {
                if ($baseline_ratio == 0) { printf \"0.0\"; exit }
                printf \"%.1f\", ($current_ratio - $baseline_ratio) / $baseline_ratio * 100
            }")

            if [ "$regression" -eq 1 ]; then
                echo "  [REGRESSION] $problem_name: ${current_ratio}x vs baseline ${baseline_ratio}x (+${pct}%)"
                regression_found=1
            else
                echo "  [OK] $problem_name: ${current_ratio}x vs baseline ${baseline_ratio}x (${pct}%)"
            fi
        done
    else
        echo "  WARNING: jq not available, cannot parse baseline JSON"
        echo "  Install jq for regression detection support"
    fi

    if [ $regression_found -eq 1 ]; then
        echo ""
        echo "REGRESSION DETECTED: One or more benchmarks regressed >5% from baseline"
        exit 1
    else
        echo ""
        echo "No regressions detected"
    fi
fi

if [ "$failed" -gt 0 ] || [ "$errors" -gt 0 ]; then
    exit 1
fi
exit 0
