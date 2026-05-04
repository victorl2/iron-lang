#!/usr/bin/env bash
# Phase 7 Plan 07-01 Task 02 (HARD-20, D-08) -- CTest invariant: when
# the editor parent dies, `ironls` exits within 5s via the prctl
# PR_SET_PDEATHSIG path on Linux (kqueue on macOS).
#
# Usage: test_parent_death_detection.sh <path/to/ironls>
#
# Exit codes:
#   0  - ironls exited within 5s of its parent death
#   1  - ironls still running 5s after parent death
#   2  - harness error

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

# The test design:
#   - This script (S) forks a child script (P1 "fake editor") via bash.
#   - P1 spawns ironls as a direct child and immediately exits.
#   - Linux kernel reparents ironls to init + delivers SIGTERM via
#     PR_SET_PDEATHSIG (installed inside ironls's main()).
#   - S polls `kill -0 <ironls_pid>` for up to 10s; PASS if ironls is
#     gone within 5s, FAIL otherwise.

# Create a pipe whose write end P1 holds and whose read end ironls reads.
# We also tee ironls's PID to a file so S can observe it after P1 exits.
FIFO="$TMPDIR/ironls_pid"

# P1 script: start ironls with its stdin as /dev/null, record the pid,
# exit.
bash -c "
    '$IRONLS' --log-level=DEBUG > /dev/null 2>&1 < /dev/null &
    IPID=\$!
    echo \$IPID > '$FIFO'
    # Give ironls time to run prctl(PR_SET_PDEATHSIG) BEFORE we die.
    sleep 0.5
    # Now exit -- the kernel will reparent ironls to init and deliver SIGTERM.
    exit 0
" &
P1_PID=$!

# Wait for P1 to finish AND for the pid file to appear.
wait "$P1_PID" 2>/dev/null || true
if [[ ! -f "$FIFO" ]]; then
    echo "FAIL: P1 never wrote the pid file" >&2
    exit 2
fi
IRONLS_PID="$(cat "$FIFO")"
if [[ -z "$IRONLS_PID" ]]; then
    echo "FAIL: pid file was empty" >&2
    exit 2
fi

# Poll for ironls exit. Target: exits within 5s. Allow 10s before giving up.
ELAPSED=0
DEADLINE_MS=10000
STEP_MS=100
while kill -0 "$IRONLS_PID" 2>/dev/null; do
    if (( ELAPSED >= DEADLINE_MS )); then
        echo "FAIL: ironls (pid=$IRONLS_PID) still alive after 10s" >&2
        kill -9 "$IRONLS_PID" 2>/dev/null || true
        exit 1
    fi
    sleep 0.1
    ELAPSED=$(( ELAPSED + STEP_MS ))
done

if (( ELAPSED > 5000 )); then
    echo "FAIL: ironls took ${ELAPSED}ms to exit (threshold: 5000ms)" >&2
    exit 1
fi

echo "PASS: ironls exited ${ELAPSED}ms after parent death"
exit 0
