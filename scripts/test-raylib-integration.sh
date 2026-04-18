#!/bin/bash
# scripts/test-raylib-integration.sh
# Phase 73-04 driver: 12 integration tests × 2 targets + pure-superset guards.
#
# Runs the full v2.0.0-alpha raylib binding regression surface in a single
# command. Each --target=web invocation overwrites dist/web/; this harness
# uses exit-code-only semantics (no artifact retention).
#
# Categories (12): win input draw2d coll tex text audio draw3d model shader math file
# Targets (2):    native (always) + web (skipped if emcc not in PATH — D3 residual)
# Pure-superset guards (3): examples/pong/pong.iron
#                            tests/manual/game_raylib.iron
#                            tests/integration/web/hello_raylib.iron
#
# Exit 0: all in-scope builds PASS (web target PASS or SKIP both acceptable).
# Exit 1: any native build FAIL, or any web build FAIL when emcc is available.

set -u

CATEGORIES=(win input draw2d coll tex text audio draw3d model shader math file)
FAIL=0

# Detect emsdk availability once; web matrix is deferred per Phase 73-01
# deferred-items.md D3 when emcc is absent (environment limitation).
if command -v emcc > /dev/null 2>&1; then
    WEB_AVAILABLE=1
else
    WEB_AVAILABLE=0
fi

echo "================================================================"
echo " Phase 73-04 raylib integration test matrix"
echo " 12 categories × 2 targets = 24 build invocations"
echo " + 3 pure-superset guards (pong, game_raylib, hello_raylib_web)"
if [ "$WEB_AVAILABLE" -eq 0 ]; then
    echo " NOTE: emcc not in PATH — web target SKIPPED (D3 residual)."
fi
echo "================================================================"

for cat in "${CATEGORIES[@]}"; do
    test="tests/integration/raylib/$cat/${cat}_test.iron"
    if [ ! -f "$test" ]; then
        echo "MISSING: $test"
        FAIL=1
        continue
    fi

    printf "%-10s native ... " "$cat"
    if ./build/ironc build "$test" > /dev/null 2>&1; then
        echo "PASS"
    else
        echo "FAIL"
        FAIL=1
    fi

    printf "%-10s web    ... " "$cat"
    if [ "$WEB_AVAILABLE" -eq 1 ]; then
        if ./build/ironc build --target=web "$test" > /dev/null 2>&1; then
            echo "PASS"
        else
            echo "FAIL"
            FAIL=1
        fi
    else
        echo "SKIP (emcc)"
    fi
done

echo "----------------------------------------------------------------"
echo " Pure-superset guards (API-11)"
echo "----------------------------------------------------------------"

printf "pong        native ... "
if ./build/ironc build examples/pong/pong.iron > /dev/null 2>&1; then
    echo "PASS"
else
    echo "FAIL"
    FAIL=1
fi

printf "game_raylib native ... "
if ./build/ironc build tests/manual/game_raylib.iron > /dev/null 2>&1; then
    echo "PASS"
else
    echo "FAIL"
    FAIL=1
fi

printf "hello_raylib web   ... "
if [ "$WEB_AVAILABLE" -eq 1 ]; then
    if ./build/ironc build --target=web tests/integration/web/hello_raylib.iron > /dev/null 2>&1; then
        echo "PASS"
    else
        echo "FAIL"
        FAIL=1
    fi
else
    echo "SKIP (emcc)"
fi

echo "================================================================"
if [ "$FAIL" -eq 0 ]; then
    if [ "$WEB_AVAILABLE" -eq 1 ]; then
        echo " ALL 27 BUILDS GREEN — API-11 + API-12 closed"
    else
        echo " ALL NATIVE BUILDS GREEN (15 of 27) — web matrix deferred per D3"
        echo " API-11 + API-12 closed at native target; web parity inherited"
        echo " to post-alpha emsdk-equipped re-run."
    fi
else
    echo " FAILURES DETECTED — see above"
fi
echo "================================================================"

exit $FAIL
