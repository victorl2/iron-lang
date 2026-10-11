#!/usr/bin/env bash
# Break on panic (#312, #388): a panic stops the debugger on the Iron line
# that panicked.
#
# A check of a --debug build (a failed assert, an index out of bounds,
# unwrap() of a null Box) runs a breakpoint instruction in the Iron
# function when a debugger is attached, before the panic: plain gdb and
# plain LLDB, with no Iron script at all, stop there with the Iron
# function's locals, and continuing runs the usual panic (SIGABRT). The
# same holds in `iron debug` with gdb, with the commands `iron debug` gives
# LLDB, and through `iron dap` (dap_client.py --panic). A panic inside the
# runtime (out of memory) still stops on abort(), with the
# Iron frame selected by iron debug / iron dap.
#
# Usage: debug_panic_smoke.sh <ironc> <source dir>
# Each debugger is skipped when missing or unable to run a program here.
set -euo pipefail

IRONC="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
IRON="$(dirname "$IRONC")/iron"
SRC_ROOT="$(cd "$2" && pwd)"
DIR="$SRC_ROOT/tests/integration/debug"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

# Each debugger session runs under a watchdog (#412): one that hangs fails
# the test within its limit, naming the fixture and the debugger, instead
# of running into ctest's timeout. macOS has no timeout(1), so the session
# runs in the background and the watchdog kills its process tree (the
# debugger, debugserver, the debuggee).
SESSION_LIMIT=${IRON_DEBUG_SESSION_LIMIT:-90}
kill_tree() {
    local c
    for c in $(pgrep -P "$1" 2>/dev/null); do kill_tree "$c"; done
    kill -9 "$1" 2>/dev/null || true
}
# session <what> <limit> <stdin file> <command...>: sets $out (its stdout
# and stderr) and $session_rc; when the watchdog fires the test fails.
session() {
    local what=$1 limit=$2 input=$3 pid start=$SECONDS
    shift 3
    "$@" < "$input" > "$work/session.out" 2>&1 &
    pid=$!
    while kill -0 "$pid" 2>/dev/null; do
        if [ $((SECONDS - start)) -ge "$limit" ]; then
            kill_tree "$pid"
            wait "$pid" 2>/dev/null || true
            echo "FAIL: $what did not finish within ${limit}s (watchdog); its output:"
            cat "$work/session.out"
            exit 1
        fi
        sleep 0.2
    done
    session_rc=0
    wait "$pid" || session_rc=$?
    out="$(cat "$work/session.out")"
}

# fixture | Iron function | line | a local and its value | panic message | final message
CASES=(
    "panic|check|6|total|12|assertion failed: total too large|^assertion failed: total too large"
    "panic_index|pick|3|i|5|index out of bounds (index 5, bound 3)|^iron: index out of bounds"
    "panic_null|main|8|before|7|unwrap() on null Box|^iron: panic: unwrap\\(\\) on null Box"
)

for c in "${CASES[@]}"; do
    IFS='|' read -r name _ <<< "$c"
    "$IRONC" build "$DIR/$name.iron" --debug -o "$name" >/dev/null 2>&1 ||
        { echo "FAIL: cannot build $name.iron with --debug"; exit 1; }
done
"$IRONC" build "$DIR/panic_runtime.iron" --debug -o panic_runtime >/dev/null 2>&1 ||
    { echo "FAIL: cannot build panic_runtime.iron with --debug"; exit 1; }

# Without a debugger a --debug build panics exactly as before: no trap, the
# usual message and exit status.
for c in "${CASES[@]}"; do
    IFS='|' read -r name func line var val text final <<< "$c"
    set +e
    out="$(./"$name" 2>&1)"
    rc=$?
    set -e
    [ $rc -eq 134 ] || [ $rc -eq 3 ] || { echo "FAIL: $name exits $rc without a debugger"; exit 1; }
    grep -qE "$final" <<< "$out" || { echo "FAIL: $name: no '$final'"; echo "$out"; exit 1; }
    ! grep -q "^panic: " <<< "$out" || { echo "FAIL: $name trapped without a debugger"; echo "$out"; exit 1; }
done
echo "no debugger: each panic prints its usual message and aborts"

gdb_runs=0
if command -v gdb >/dev/null 2>&1; then
    for c in "${CASES[@]}"; do
        IFS='|' read -r name func line var val text final <<< "$c"
        # Plain gdb: no Iron script, no breakpoint.
        session "gdb on $name" "$SESSION_LIMIT" /dev/null \
            gdb -nx -batch -ex run -ex frame -ex "print $var" -ex continue ./"$name"
        if ! grep -q "SIGTRAP" <<< "$out"; then
            if grep -qiE "ptrace|not permitted|cannot (run|create process)|Couldn't get registers" <<< "$out"; then
                echo "gdb cannot run a program here: skipped"; tail -3 <<< "$out"; break
            fi
            echo "FAIL: gdb: $name did not stop on the trap"; echo "$out"; exit 1
        fi
        gdb_runs=1
        grep -qE "^#0 +(0x[0-9a-f]+ in )?Iron_$func \(.*\) at (.*/)?$name\.iron:$line\$" <<< "$out" ||
            { echo "FAIL: gdb: $name stops outside $func at $name.iron:$line"; echo "$out"; exit 1; }
        grep -qE "\\\$1 = $val\b|\\\$1 = $val '" <<< "$out" ||
            { echo "FAIL: gdb: $var is not $val"; echo "$out"; exit 1; }
        grep -qF "panic: $text" <<< "$out" || { echo "FAIL: gdb: no 'panic: $text'"; echo "$out"; exit 1; }
        grep -qE "$final" <<< "$out" || { echo "FAIL: gdb: continuing does not panic"; echo "$out"; exit 1; }
        grep -q "SIGABRT" <<< "$out" || { echo "FAIL: gdb: continuing does not abort"; echo "$out"; exit 1; }
        echo "gdb: $name stops in $func at $name.iron:$line ($var = $val), continue aborts"
    done
    if [ $gdb_runs -eq 1 ] && [ -x "$IRON" ]; then
        # iron debug --gdb (formatters + break abort).
        printf 'run\nframe\nprint total\nkill\n' > "$work/input"
        session "iron debug --gdb on panic.iron" "$SESSION_LIMIT" "$work/input" \
            "$IRON" debug "$DIR/panic.iron" --gdb
        grep -q "Iron panic at panic.iron:6: assertion failed: total too large" <<< "$out" ||
            { echo "FAIL: iron debug --gdb: no 'Iron panic at panic.iron:6'"; echo "$out"; exit 1; }
        grep -qE "#0 +(0x[0-9a-f]+ in )?Iron_check \(total=12\)" <<< "$out" ||
            { echo "FAIL: iron debug --gdb: check's frame is not the stop"; echo "$out"; exit 1; }
        grep -qE '\$1 = 12' <<< "$out" || { echo "FAIL: iron debug --gdb: total is not 12"; echo "$out"; exit 1; }
        echo "gdb: iron debug stops on the panic at panic.iron:6"
        # A runtime panic: abort(), with the Iron frame selected.
        printf 'run\nframe\nkill\n' > "$work/input"
        session "iron debug --gdb on panic_runtime.iron" "$SESSION_LIMIT" "$work/input" \
            "$IRON" debug "$DIR/panic_runtime.iron" --gdb
        grep -q "Iron panic at panic_runtime.iron:2 (frame #" <<< "$out" ||
            { echo "FAIL: iron debug --gdb: the runtime panic does not select load"; echo "$out"; exit 1; }
        echo "gdb: a runtime panic stops on abort with panic_runtime.iron:2 selected"
    fi
else
    echo "gdb not installed: skipped"
fi

if command -v lldb >/dev/null 2>&1 && lldb --version >/dev/null 2>&1; then
    lldb_runs=0
    for c in "${CASES[@]}"; do
        IFS='|' read -r name func line var val text final <<< "$c"
        # Plain LLDB. In batch mode a stop on the trap ends the -o
        # commands; the -k ones run after it.
        session "lldb on $name" "$SESSION_LIMIT" /dev/null \
            lldb -b -o run -k "frame variable $var" -k continue ./"$name"
        if ! grep -q "stop reason" <<< "$out"; then
            echo "lldb cannot run the program here: skipped"; break
        fi
        lldb_runs=1
        grep -qE "frame #0: .*Iron_$func\(.*\) at $name\.iron:$line|frame #0: .*Iron_$func at $name\.iron:$line" <<< "$out" ||
            { echo "FAIL: lldb: $name stops outside $func at $name.iron:$line"; echo "$out"; exit 1; }
        grep -qE "\) $var = $val\b|\) $var = '" <<< "$out" ||
            { echo "FAIL: lldb: $var is not $val"; echo "$out"; exit 1; }
        grep -qF "panic: $text" <<< "$out" || { echo "FAIL: lldb: no 'panic: $text'"; echo "$out"; exit 1; }
        grep -q "SIGABRT" <<< "$out" || { echo "FAIL: lldb: continuing does not abort"; echo "$out"; exit 1; }
        echo "lldb: $name stops in $func at $name.iron:$line, continue aborts"
    done
    if [ $lldb_runs -eq 1 ]; then
        # The commands iron debug passes to LLDB.
        session "lldb (iron-panic-stop) on panic" "$SESSION_LIMIT" /dev/null \
            lldb -b -o "command script import $SRC_ROOT/src/debug/iron_lldb.py" \
                 -o iron-panic-stop -o run -o "frame variable total" \
                 -k "frame variable total" ./panic
        grep -q "Iron panic at panic.iron:6: assertion failed: total too large" <<< "$out" ||
            { echo "FAIL: lldb: no 'Iron panic at panic.iron:6'"; echo "$out"; exit 1; }
        grep -qE "frame #0: .*Iron_check\(total=12\) at panic.iron:6" <<< "$out" ||
            { echo "FAIL: lldb: check's frame is not the stop"; echo "$out"; exit 1; }
        grep -q "total = 12" <<< "$out" || { echo "FAIL: lldb: total is not 12"; echo "$out"; exit 1; }
        echo "lldb: iron-panic-stop stops on the panic at panic.iron:6"
        session "lldb (iron-panic-stop) on panic_runtime" "$SESSION_LIMIT" /dev/null \
            lldb -b -o "command script import $SRC_ROOT/src/debug/iron_lldb.py" \
                 -o iron-panic-stop -o run ./panic_runtime
        grep -qE "Iron panic at panic_runtime.iron:2 \(frame #" <<< "$out" ||
            { echo "FAIL: lldb: the runtime panic does not select load"; echo "$out"; exit 1; }
        echo "lldb: a runtime panic stops on abort with panic_runtime.iron:2 selected"
    fi
else
    echo "lldb not installed: skipped"
fi

if command -v python3 >/dev/null 2>&1 && [ -x "$IRON" ]; then
    dap_runs=0
    for spec in "panic:check:6:total too large:trap" \
                "panic_index:pick:3:index out of bounds:trap" \
                "panic_null:main:8:unwrap() on null Box:trap" \
                "panic_runtime:load:2:out of memory"; do
        name="${spec%%:*}"
        # dap_client.py bounds each of its steps; the watchdog is the backstop.
        session "iron dap on $name.iron" $((SESSION_LIMIT * 2)) /dev/null \
            python3 "$DIR/dap_client.py" --panic --expect="${spec#*:}" "$DIR/$name.iron" "$IRON" dap
        echo "$out"
        # 77: no DAP debugger here. Once a session ran, that is a failure.
        [ $session_rc -eq 77 ] && [ $dap_runs -eq 0 ] && break
        [ $session_rc -eq 0 ] || { echo "FAIL: iron dap does not stop on the panic of $name.iron"; exit 1; }
        dap_runs=1
    done
fi

# Panics whose check runs inside the runtime (#388): a list method's body
# (pop on an empty list, remove / insert out of range, on a runtime list, a
# generated object list and an interface list) and read_file. A --debug
# build repeats the check at the call site, so they trap on the Iron line
# like the checks above; continuing runs the runtime's usual panic.
runtime_check_panics() {
    # fixture | Iron function | line | a local and its value | panic message | final message
    local cases=(
        "panic_pop|main|4|first|7|pop from an empty list|^iron: pop from an empty list"
        "panic_remove|drop_at|3|i|5|index out of bounds (index 5, bound 3)|^iron: index out of bounds"
        "panic_insert|place|7|at|9|index out of bounds (index 9, bound 3)|^iron: index out of bounds"
        "panic_split|replace|14|slot|4|index out of bounds (index 4, bound 2)|^iron: index out of bounds"
        "panic_read_file|load|2|tries|2|read_file: cannot open '/nonexistent/iron-panic-test'|^panic: read_file: cannot open"
    )
    local c name func line var val text final out rc
    for c in "${cases[@]}"; do
        IFS='|' read -r name _ <<< "$c"
        "$IRONC" build "$DIR/$name.iron" --debug -o "$name" >/dev/null 2>&1 ||
            { echo "FAIL: cannot build $name.iron with --debug"; exit 1; }
    done
    for c in "${cases[@]}"; do
        IFS='|' read -r name func line var val text final <<< "$c"
        set +e
        out="$(./"$name" 2>&1)"
        rc=$?
        set -e
        [ $rc -eq 134 ] || [ $rc -eq 3 ] || { echo "FAIL: $name exits $rc without a debugger"; exit 1; }
        grep -qE "$final" <<< "$out" || { echo "FAIL: $name: no '$final'"; echo "$out"; exit 1; }
        ! grep -qE "^panic: .* at .*\.iron:[0-9]+$" <<< "$out" ||
            { echo "FAIL: $name trapped without a debugger"; echo "$out"; exit 1; }
    done
    echo "no debugger: each runtime check panics as before"

    if command -v gdb >/dev/null 2>&1; then
        for c in "${cases[@]}"; do
            IFS='|' read -r name func line var val text final <<< "$c"
            out="$(gdb -nx -batch -ex run -ex frame -ex "print $var" -ex continue ./"$name" 2>&1 || true)"
            if ! grep -q "SIGTRAP" <<< "$out"; then
                if grep -qiE "ptrace|not permitted|cannot (run|create process)|Couldn't get registers" <<< "$out"; then
                    echo "gdb cannot run a program here: skipped"; break
                fi
                echo "FAIL: gdb: $name did not stop on the trap"; echo "$out"; exit 1
            fi
            grep -qE "^#0 +(0x[0-9a-f]+ in )?Iron_$func \(.*\) at (.*/)?$name\.iron:$line\$" <<< "$out" ||
                { echo "FAIL: gdb: $name stops outside $func at $name.iron:$line"; echo "$out"; exit 1; }
            grep -qE "\\\$1 = $val\b" <<< "$out" || { echo "FAIL: gdb: $var is not $val"; echo "$out"; exit 1; }
            grep -F "panic: $text at " <<< "$out" | tr -d '\r' | grep -E "/?$name\.iron:$line\$" >/dev/null ||
                { echo "FAIL: gdb: no 'panic: $text'"; echo "$out"; exit 1; }
            grep -qE "$final" <<< "$out" || { echo "FAIL: gdb: continuing does not panic"; echo "$out"; exit 1; }
            grep -q "SIGABRT" <<< "$out" || { echo "FAIL: gdb: continuing does not abort"; echo "$out"; exit 1; }
            echo "gdb: $name stops in $func at $name.iron:$line ($var = $val), continue aborts"
        done
    fi

    if command -v lldb >/dev/null 2>&1 && lldb --version >/dev/null 2>&1; then
        for c in "${cases[@]}"; do
            IFS='|' read -r name func line var val text final <<< "$c"
            out="$(lldb -b -o run -k "frame variable $var" -k continue ./"$name" 2>&1 || true)"
            if ! grep -q "stop reason" <<< "$out"; then
                echo "lldb cannot run the program here: skipped"; break
            fi
            grep -qE "frame #0: .*Iron_$func\(.*\) at $name\.iron:$line|frame #0: .*Iron_$func at $name\.iron:$line" <<< "$out" ||
                { echo "FAIL: lldb: $name stops outside $func at $name.iron:$line"; echo "$out"; exit 1; }
            grep -qE "\) $var = $val\b" <<< "$out" || { echo "FAIL: lldb: $var is not $val"; echo "$out"; exit 1; }
            grep -F "panic: $text at " <<< "$out" | tr -d '\r' | grep -E "/?$name\.iron:$line\$" >/dev/null ||
                { echo "FAIL: lldb: no 'panic: $text'"; echo "$out"; exit 1; }
            grep -q "SIGABRT" <<< "$out" || { echo "FAIL: lldb: continuing does not abort"; echo "$out"; exit 1; }
            echo "lldb: $name stops in $func at $name.iron:$line, continue aborts"
        done
    fi

    if command -v python3 >/dev/null 2>&1 && [ -x "$IRON" ]; then
        for c in "${cases[@]}"; do
            IFS='|' read -r name func line var val text final <<< "$c"
            # --expect is colon separated: the message up to its details.
            text="${text%% (*}"
            text="${text##*: }"
            set +e
            python3 "$DIR/dap_client.py" --panic --expect="$func:$line:$text:trap" \
                "$DIR/$name.iron" "$IRON" dap
            rc=$?
            set -e
            [ $rc -eq 77 ] && break
            [ $rc -eq 0 ] || { echo "FAIL: iron dap does not stop on the panic of $name.iron"; exit 1; }
        done
    fi
}
runtime_check_panics
echo "PASS"
