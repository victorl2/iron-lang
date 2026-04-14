#!/usr/bin/env bash
# Phase 69 Plan 01 (COV-02): llvm-cov driver for Iron.
#
# Configures a fresh build-cov/ tree with IRON_ENABLE_COVERAGE=ON, runs the
# full ctest suite (minus the benchmark label), merges the profraw outputs
# via llvm-profdata, and emits:
#   build-cov/coverage/index.html    - human-readable HTML report
#   build-cov/coverage/summary.json  - machine-readable per-file totals
#
# The summary.json schema is consumed by .github/workflows/coverage.yml
# (Plan 02) for PR delta comments and by COV-04's baseline generation
# (Plan 03) for the per-file coverage table.
#
# Usage:
#   bash scripts/coverage.sh                    # full pipeline
#   bash scripts/coverage.sh --skip-build       # reuse existing build-cov/
#   bash scripts/coverage.sh --output-dir DIR   # override build-cov/coverage
#   bash scripts/coverage.sh --verbose          # show llvm-cov show output
#   bash scripts/coverage.sh --help             # print usage
#
# Exit codes:
#   0  success
#   1  cmake/build/ctest failure
#   2  llvm-profdata or llvm-cov missing from PATH

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="$REPO_ROOT/build-cov"
OUTPUT_DIR=""
SKIP_BUILD=0
VERBOSE=0

usage() {
    cat <<EOF
Usage: bash scripts/coverage.sh [--skip-build] [--output-dir DIR] [--verbose]

Runs the Iron test suite with llvm-cov instrumentation and emits HTML +
summary.json reports under build-cov/coverage/ (or --output-dir DIR).

Requires clang, llvm-profdata, llvm-cov, cmake, ninja in PATH.
EOF
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        --help|-h) usage ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --verbose) VERBOSE=1; shift ;;
        *) echo "ERROR: unknown arg: $1" >&2; exit 1 ;;
    esac
done

if [ -z "$OUTPUT_DIR" ]; then
    OUTPUT_DIR="$BUILD_DIR/coverage"
fi

# macOS xcrun integration: llvm-profdata and llvm-cov ship with the Xcode
# Command Line Tools but are NOT on the default PATH, while /usr/bin/clang
# IS (and is a sysroot-aware shim that must NOT be shadowed by the CLT's own
# bare clang binary, which fails to find the SDK). We therefore APPEND the
# CLT bin directory so the llvm tools become discoverable as a fallback
# without changing which `clang` the compiler and ironc subprocesses see.
if [ -d /Library/Developer/CommandLineTools/usr/bin ]; then
    case ":$PATH:" in
        *":/Library/Developer/CommandLineTools/usr/bin:"*) ;;
        *) PATH="$PATH:/Library/Developer/CommandLineTools/usr/bin"; export PATH ;;
    esac
fi
# Homebrew llvm fallback (Linux + macOS brew installs). Appended so upstream
# clang doesn't mask the system clang.
for brew_llvm in /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin; do
    if [ -d "$brew_llvm" ]; then
        case ":$PATH:" in
            *":$brew_llvm:"*) ;;
            *) PATH="$PATH:$brew_llvm"; export PATH ;;
        esac
    fi
done

# Tool probe — exit 2 if either llvm tool is missing.
for tool in llvm-profdata llvm-cov; do
    if ! command -v "$tool" &>/dev/null; then
        echo "ERROR: $tool not found in PATH" >&2
        echo "  Install: sudo apt-get install llvm (Linux) or xcode-select --install (macOS)" >&2
        exit 2
    fi
done

echo "=== Iron coverage run ==="
echo "Repo: $REPO_ROOT"
echo "Build: $BUILD_DIR"
echo "Output: $OUTPUT_DIR"
echo ""

# 1) Configure + build build-cov/ (unless --skip-build)
if [ "$SKIP_BUILD" -eq 0 ]; then
    echo "[1/5] Configuring build-cov with IRON_ENABLE_COVERAGE=ON ..."
    rm -rf "$BUILD_DIR"
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_COMPILER=clang \
        -DIRON_ENABLE_COVERAGE=ON \
        -G Ninja || { echo "ERROR: cmake configure failed" >&2; exit 1; }

    echo "[2/5] Building ..."
    cmake --build "$BUILD_DIR" || { echo "ERROR: build failed" >&2; exit 1; }
else
    echo "[1/5] --skip-build: reusing existing $BUILD_DIR"
    echo "[2/5] --skip-build: skipping build"
fi

# 2) Run ctest with LLVM_PROFILE_FILE pointing to a per-test profraw template.
#    %p = PID, %m = binary signature -- handles parallel + multi-binary collisions.
echo "[3/5] Running ctest -LE benchmark with LLVM_PROFILE_FILE ..."
mkdir -p "$BUILD_DIR/profraw"
export LLVM_PROFILE_FILE="$BUILD_DIR/profraw/%p-%m.profraw"
# ASAN_OPTIONS only matters if a downstream wrapper accidentally re-enables
# sanitizers — with IRON_ENABLE_COVERAGE=ON the mutex prevents ASAN anyway.
ctest --test-dir "$BUILD_DIR" --output-on-failure -j4 -LE benchmark
TEST_RC=$?
if [ "$TEST_RC" -ne 0 ]; then
    echo "WARN: ctest exited with $TEST_RC (continuing to extract coverage from partial runs)" >&2
fi

# 3) Merge all .profraw files into a single .profdata
echo "[4/5] Merging profraw files via llvm-profdata ..."
mkdir -p "$OUTPUT_DIR"
PROFDATA="$OUTPUT_DIR/merged.profdata"
PROFRAW_COUNT=$(find "$BUILD_DIR/profraw" -name '*.profraw' 2>/dev/null | wc -l | tr -d ' ')
if [ "$PROFRAW_COUNT" -eq 0 ]; then
    echo "ERROR: no .profraw files produced — tests did not run instrumented" >&2
    exit 1
fi
echo "  merging $PROFRAW_COUNT profraw files"
llvm-profdata merge -sparse "$BUILD_DIR"/profraw/*.profraw -o "$PROFDATA" \
    || { echo "ERROR: llvm-profdata merge failed" >&2; exit 1; }

# 4) Emit HTML report via llvm-cov show. We target every built binary so
#    the report covers the full instrumented surface. `-object` repeats
#    once per binary. Filter source to src/ only (exclude tests/, vendor).
echo "[5/5] Rendering HTML + summary.json ..."
BINARIES=()
for bin in "$BUILD_DIR/ironc" "$BUILD_DIR/iron"; do
    [ -x "$bin" ] && BINARIES+=("-object" "$bin")
done
# Unit-test binaries under tests/unit/ also produce profraw — include a few
# representative ones so their coverage on src/ is reflected in the merged
# report. Prefer globbing so new tests are picked up automatically.
while IFS= read -r -d '' t; do
    BINARIES+=("-object" "$t")
done < <(find "$BUILD_DIR/tests/unit" -maxdepth 1 -type f -perm -u+x -print0 2>/dev/null)

if [ ${#BINARIES[@]} -eq 0 ]; then
    echo "ERROR: no instrumented binaries found under $BUILD_DIR" >&2
    exit 1
fi

# llvm-cov treats trailing positional args as source file paths (NOT a
# directory filter). To restrict the report to src/ we use
# -ignore-filename-regex to exclude third-party + build-output + test
# scaffolding. Anchored at path boundaries to avoid over-matching.
IGNORE_REGEX='(^|/)(tests|vendor|_deps|build|build-cov|unity)(/|$)|^/(Applications|Library|usr)/'

# HTML report
IRON_COV_SHOW_LOG="$OUTPUT_DIR/llvm-cov-show.log"
llvm-cov show "${BINARIES[@]}" \
    -instr-profile="$PROFDATA" \
    -format=html \
    -output-dir="$OUTPUT_DIR" \
    -show-line-counts-or-regions \
    -use-color \
    -ignore-filename-regex="$IGNORE_REGEX" \
    > "$IRON_COV_SHOW_LOG" 2>&1
SHOW_RC=$?
if [ "$SHOW_RC" -ne 0 ]; then
    echo "ERROR: llvm-cov show failed (rc=$SHOW_RC)" >&2
    tail -20 "$IRON_COV_SHOW_LOG" >&2
    exit 1
fi
[ "$VERBOSE" -eq 1 ] && cat "$IRON_COV_SHOW_LOG"

# JSON export → summary.json via python summarizer
llvm-cov export "${BINARIES[@]}" \
    -instr-profile="$PROFDATA" \
    -format=text \
    -ignore-filename-regex="$IGNORE_REGEX" \
    > "$OUTPUT_DIR/export.json" \
    || { echo "ERROR: llvm-cov export failed" >&2; exit 1; }

COMMIT_SHA="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
GENERATED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

python3 - "$OUTPUT_DIR/export.json" "$OUTPUT_DIR/summary.json" "$COMMIT_SHA" "$GENERATED_AT" "$REPO_ROOT" <<'PYEOF'
import json, sys, os

export_path, summary_path, sha, generated_at, repo_root = sys.argv[1:6]
with open(export_path) as f:
    data = json.load(f)

files_out = {}
for export_obj in data.get("data", []):
    for fobj in export_obj.get("files", []):
        path = fobj.get("filename", "")
        # Normalize to repo-relative
        if path.startswith(repo_root + "/"):
            path = path[len(repo_root) + 1 :]
        s = fobj.get("summary", {})
        lines = s.get("lines", {})
        branches = s.get("branches", {})
        line_pct = float(lines.get("percent", 0.0))
        branch_pct = float(branches.get("percent", 0.0))
        lines_total = int(lines.get("count", 0))
        lines_covered = int(lines.get("covered", 0))
        files_out[path] = {
            "line": round(line_pct, 2),
            "branch": round(branch_pct, 2),
            "lines_total": lines_total,
            "lines_covered": lines_covered,
        }

total_lines = sum(f["lines_total"] for f in files_out.values())
total_covered = sum(f["lines_covered"] for f in files_out.values())
line_total_pct = (100.0 * total_covered / total_lines) if total_lines > 0 else 0.0

# branch totals from the original export (sum covered/count across all files)
b_count = 0
b_covered = 0
for export_obj in data.get("data", []):
    for fobj in export_obj.get("files", []):
        b = fobj.get("summary", {}).get("branches", {})
        b_count += int(b.get("count", 0))
        b_covered += int(b.get("covered", 0))
branch_total_pct = (100.0 * b_covered / b_count) if b_count > 0 else 0.0

out = {
    "generated_at": generated_at,
    "commit_sha": sha,
    "files": files_out,
    "totals": {
        "line": round(line_total_pct, 2),
        "branch": round(branch_total_pct, 2),
    },
}
with open(summary_path, "w") as f:
    json.dump(out, f, indent=2, sort_keys=True)
print(f"Wrote {summary_path} ({len(files_out)} files)")
PYEOF
PY_RC=$?
if [ "$PY_RC" -ne 0 ]; then
    echo "ERROR: summary.json generation failed" >&2
    exit 1
fi

echo ""
echo "=== Coverage run complete ==="
echo "HTML:    $OUTPUT_DIR/index.html"
echo "Summary: $OUTPUT_DIR/summary.json"
echo ""
if [ -f "$OUTPUT_DIR/summary.json" ]; then
    python3 -c "import json; d=json.load(open('$OUTPUT_DIR/summary.json')); print(f\"Totals: line={d['totals']['line']}% branch={d['totals']['branch']}% files={len(d['files'])}\")"
fi

exit 0
