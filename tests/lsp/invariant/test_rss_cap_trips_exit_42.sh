#!/usr/bin/env bash
# Phase 7 Plan 07-02 Task 01 (HARD-15, D-03) -- CTest invariant: running
# `ironls` with IRON_LSP_RSS_CAP_BYTES=65536 (64 KiB, tiny cap that any
# real LSP allocation will exceed almost immediately) trips the cap
# inside the 5-second sampling window and exits with code exactly 42.
#
# The test does NOT even need to send LSP traffic: the server's own
# initialize-path allocations (stb_ds maps, arena blocks, yyjson
# allocator pools) push RSS well above 64 KiB within the first
# sampling interval.
#
# Usage: test_rss_cap_trips_exit_42.sh <path/to/ironls>
#
# Exit codes:
#   0  - ironls exited with code 42 within 30s; marker file present
#   1  - wrong exit code OR no marker file
#   2  - harness error (ironls binary missing, fifo failure, etc.)

set -euo pipefail

IRONLS="${1:-}"
if [[ -z "$IRONLS" || ! -x "$IRONLS" ]]; then
    echo "FAIL: ironls binary missing or not executable: '$IRONLS'" >&2
    exit 2
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT
mkdir -p "$TMPDIR/iron-lsp"
export XDG_STATE_HOME="$TMPDIR"

# Tiny cap: 64 KiB. The process will exceed this during normal startup.
export IRON_LSP_RSS_CAP_BYTES=65536

# Keep a FIFO open on stdin so ironls's reader thread doesn't hit EOF
# before the sampler thread's first 5-second tick. (See
# test_crash_dump_written.sh for the same idiom.)
FIFO="$TMPDIR/in"
mkfifo "$FIFO"
( sleep 40 > "$FIFO" ) &
HOLDER_PID=$!
sleep 0.1

"$IRONLS" --log-level=INFO < "$FIFO" > "$TMPDIR/out.txt" 2> "$TMPDIR/err.txt" &
IRONLS_PID=$!

# Poll for exit (the sampler first sleeps 5s, then checks; the cap
# should trip on the first check). Allow up to 30s per plan.
EXIT_CODE=""
for _ in $(seq 1 60); do
    if ! kill -0 "$IRONLS_PID" 2>/dev/null; then
        # Process has exited -- collect exit code via wait.
        set +e
        wait "$IRONLS_PID"
        EXIT_CODE=$?
        set -e
        break
    fi
    sleep 0.5
done

# Stop the fifo holder.
kill "$HOLDER_PID" 2>/dev/null || true
wait "$HOLDER_PID" 2>/dev/null || true

if [[ -z "$EXIT_CODE" ]]; then
    echo "FAIL: ironls still running after 30s; expected exit 42" >&2
    kill -KILL "$IRONLS_PID" 2>/dev/null || true
    echo "--- stdout ---" >&2
    cat "$TMPDIR/out.txt" >&2 || true
    echo "--- stderr ---" >&2
    cat "$TMPDIR/err.txt" >&2 || true
    exit 1
fi

if [[ "$EXIT_CODE" != "42" ]]; then
    echo "FAIL: ironls exited $EXIT_CODE, expected 42" >&2
    echo "--- stdout ---" >&2
    cat "$TMPDIR/out.txt" >&2 || true
    echo "--- stderr ---" >&2
    cat "$TMPDIR/err.txt" >&2 || true
    exit 1
fi

# Find the marker file under $XDG_STATE_HOME/iron-lsp/.
MARKER="$(find "$TMPDIR/iron-lsp" -maxdepth 1 -name 'rss-restart-*.log' -type f | head -n1)"
if [[ -z "$MARKER" ]]; then
    echo "FAIL: no rss-restart-*.log marker under $TMPDIR/iron-lsp/" >&2
    ls -la "$TMPDIR/iron-lsp" >&2 || true
    exit 1
fi

# Sanity-check that the marker contains the expected header.
if ! grep -q 'Iron LSP RSS-cap restart marker' "$MARKER"; then
    echo "FAIL: marker file $MARKER missing header line" >&2
    cat "$MARKER" >&2 || true
    exit 1
fi

echo "PASS: ironls exited 42 at $EXIT_CODE; marker=$MARKER"
exit 0
