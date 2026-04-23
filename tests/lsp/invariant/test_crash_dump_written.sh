#!/usr/bin/env bash
# Phase 7 Plan 07-01 Task 01 (HARD-14, D-02) -- CTest invariant: a
# deliberate SIGSEGV delivered to a running `ironls` produces a
# 3-section crash dump at $XDG_STATE_HOME/iron-lsp/crashes/.
#
# Usage: test_crash_dump_written.sh <path/to/ironls>
#
# Exit codes:
#   0  - dump file exists with all 3 === sections + IRON_VERSION_FULL
#   1  - dump missing or malformed
#   2  - harness error (ironls failed to start, etc.)

set -euo pipefail

IRONLS="${1:-}"
if [[ -z "$IRONLS" || ! -x "$IRONLS" ]]; then
    echo "FAIL: ironls binary missing or not executable: '$IRONLS'" >&2
    exit 2
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT
mkdir -p "$TMPDIR/iron-lsp/crashes"
export XDG_STATE_HOME="$TMPDIR"

# Start ironls reading from a FIFO so we can keep stdin open while we
# send a SIGSEGV. Otherwise the server would exit on stdin EOF before
# our signal lands.
FIFO="$TMPDIR/in"
mkfifo "$FIFO"
# Keep the FIFO alive by spawning a tiny holder that sleeps with the
# write end open. This way ironls's read(stdin) blocks until we
# explicitly end the test. Opening fd 3 with >>"$FIFO" would block the
# current shell until a reader appears; the holder approach is idiomatic.
( sleep 30 > "$FIFO" ) &
HOLDER_PID=$!
# Give the holder a tick to open the fifo.
sleep 0.1

"$IRONLS" --log-level=DEBUG < "$FIFO" > "$TMPDIR/out.txt" 2>&1 &
IRONLS_PID=$!

# Wait up to 2s for the server to come up (log file appears).
for _ in $(seq 1 20); do
    if [[ -d "$TMPDIR/iron-lsp" ]]; then break; fi
    sleep 0.1
done
if ! kill -0 "$IRONLS_PID" 2>/dev/null; then
    echo "FAIL: ironls exited before we could signal it" >&2
    cat "$TMPDIR/out.txt" >&2
    exit 2
fi

# Deliver SIGSEGV.
kill -SEGV "$IRONLS_PID" || true
# Poll for exit (don't rely on wait, which can block on writer thread).
for _ in $(seq 1 50); do
    if ! kill -0 "$IRONLS_PID" 2>/dev/null; then break; fi
    sleep 0.1
done
# Stop the fifo holder.
kill "$HOLDER_PID" 2>/dev/null || true
wait "$HOLDER_PID" 2>/dev/null || true

# Find the .dmp file.
DMP="$(find "$TMPDIR/iron-lsp/crashes" -name '*.dmp' -type f | head -n1)"
if [[ -z "$DMP" ]]; then
    echo "FAIL: no .dmp file produced under $TMPDIR/iron-lsp/crashes/" >&2
    ls -la "$TMPDIR/iron-lsp/crashes" >&2 || true
    exit 1
fi

# Verify the three === sections are present.
MISSING=()
grep -q '=== BACKTRACE ===' "$DMP"           || MISSING+=("BACKTRACE")
grep -q '=== IN-FLIGHT REQUESTS ===' "$DMP"  || MISSING+=("IN-FLIGHT REQUESTS")
grep -q '=== DOCUMENT STATE ===' "$DMP"      || MISSING+=("DOCUMENT STATE")
grep -q 'IRON_VERSION_FULL='    "$DMP"       || MISSING+=("IRON_VERSION_FULL")

if [[ ${#MISSING[@]} -gt 0 ]]; then
    echo "FAIL: dump missing required sections: ${MISSING[*]}" >&2
    echo "--- dump contents ---" >&2
    cat "$DMP" >&2
    echo "--- end dump ---" >&2
    exit 1
fi

echo "PASS: $DMP"
exit 0
