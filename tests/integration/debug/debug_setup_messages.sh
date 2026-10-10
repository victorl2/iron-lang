#!/bin/bash
# What `iron debug`, `iron dap` and `iron debug --check` say when a piece
# of the debugging setup is missing (#312): Python 3 for the adapter,
# lldb-dap or gdb 14+, a debugger for `iron debug`. Each case runs with
# PATH set to a directory of our own, holding only the tools (real or
# fake) the case needs.
#
# usage: debug_setup_messages.sh <iron> <python3>
set -u
iron=$1
# The interpreter itself, not a shim that needs the usual PATH.
python=$("$2" -c 'import os, sys; print(os.path.realpath(sys.executable))')
root=$(mktemp -d "${TMPDIR:-/tmp}/iron_dbgsetup_XXXXXX")
trap 'rm -rf "$root"' EXIT
fail=0
case "$(uname -s)" in Darwin) os=macos;; *) os=linux;; esac

check() {   # check <name> <file> <expected text>
    if grep -qF -- "$3" "$2"; then
        echo "ok: $1"
    else
        echo "FAIL: $1: expected \"$3\" in:"; sed 's/^/    /' "$2"; fail=1
    fi
}

# An `initialize` request, as an editor sends first.
init_request() {
    local body='{"seq":1,"type":"request","command":"initialize","arguments":{"adapterID":"iron"}}'
    printf 'Content-Length: %d\r\n\r\n%s' "${#body}" "$body"
}

bin_none=$root/none; mkdir -p "$bin_none"

# 1. iron dap without Python: the editor gets the guidance as the error
#    response to initialize, and stderr has it too.
init_request | PATH=$bin_none "$iron" dap > "$root/1.out" 2> "$root/1.err"
rc=$?
[ $rc -eq 1 ] || { echo "FAIL: iron dap without Python exits $rc, not 1"; fail=1; }
check "dap/no python: DAP error response" "$root/1.out" '"request_seq":1,"command":"initialize","success":false'
check "dap/no python: says what is missing" "$root/1.out" 'iron dap needs Python 3.8 or later on PATH'
check "dap/no python: shows the user" "$root/1.out" '"showUser":true'
check "dap/no python: stderr" "$root/1.err" 'iron dap needs Python 3.8 or later'
if [ $os = macos ]; then
    check "dap/no python: macOS hint" "$root/1.err" 'xcode-select --install'
else
    check "dap/no python: Linux hint" "$root/1.err" 'sudo apt install python3'
fi
check "dap/no python: points at --check" "$root/1.err" 'iron debug --check'

# 2. A python3 that is Python 2 does not count.
bin_py2=$root/py2; mkdir -p "$bin_py2"
printf '#!/bin/sh\necho 2.7\n' > "$bin_py2/python3"; chmod +x "$bin_py2/python3"
init_request | PATH=$bin_py2 "$iron" dap > "$root/2.out" 2> "$root/2.err"
check "dap/python 2: names what it found" "$root/2.err" "$bin_py2/python3 is Python 2.7"

# 3. Python, but IRON_DAP_ADAPTER names nothing.
bin_py=$root/py; mkdir -p "$bin_py"
ln -s "$python" "$bin_py/python3"
init_request | PATH=$bin_py IRON_DAP_ADAPTER=$root/no-such-lldb-dap "$iron" dap > "$root/3.out" 2> "$root/3.err"
check "dap/bad IRON_DAP_ADAPTER: no debugger" "$root/3.out" 'no debugger found'
check "dap/bad IRON_DAP_ADAPTER: names it" "$root/3.out" "IRON_DAP_ADAPTER is $root/no-such-lldb-dap, which is neither a program on PATH nor a file"
check "dap/bad IRON_DAP_ADAPTER: says how to fix it" "$root/3.out" 'or unset it to search PATH'

# 4. Python, gdb 12 only: too old for DAP, and the message says so.
bin_gdb=$root/gdb12; mkdir -p "$bin_gdb"
ln -s "$python" "$bin_gdb/python3"
printf '#!/bin/sh\necho "GNU gdb (GDB) 12.1"\n' > "$bin_gdb/gdb"; chmod +x "$bin_gdb/gdb"
if [ -e /opt/homebrew/opt/llvm/bin/lldb-dap ] || [ -e /usr/local/opt/llvm/bin/lldb-dap ] ||
   { [ $os = macos ] && /usr/bin/xcode-select -p >/dev/null 2>&1 &&
     /usr/bin/xcrun -f lldb-dap >/dev/null 2>&1; }; then
    echo "skip: gdb 12 case (Xcode's or Homebrew's lldb-dap is found outside PATH)"
else
    init_request | PATH=$bin_gdb "$iron" dap > "$root/4.out" 2> "$root/4.err"
    check "dap/old gdb: no debugger" "$root/4.out" 'no debugger found'
    check "dap/old gdb: says why" "$root/4.out" "$bin_gdb/gdb is gdb 12; its DAP mode needs gdb 14 or later"
    if [ $os = macos ]; then
        check "dap/old gdb: macOS hint" "$root/4.out" 'xcode-select --install'
    else
        check "dap/old gdb: Linux hint" "$root/4.out" 'sudo apt install lldb'
    fi
    PATH=$bin_gdb "$iron" dap --check > "$root/4c.out" 2>&1
    rc=$?
    [ $rc -eq 1 ] || { echo "FAIL: iron dap --check exits $rc with no adapter, not 1"; fail=1; }
    check "dap --check/old gdb: not found" "$root/4c.out" 'lldb-dap, or gdb 14 or later: not found'
    check "dap --check/old gdb: python ok" "$root/4c.out" 'ok   Python 3.'
fi

# 5. iron debug --check with nothing on PATH: every piece reported, exit 1.
PATH=$bin_none "$iron" debug --check > "$root/5.out" 2>&1
rc=$?
[ $rc -eq 1 ] || { echo "FAIL: iron debug --check exits $rc with nothing installed, not 1"; fail=1; }
check "debug --check: gdb missing" "$root/5.out" '--   gdb: not found'
check "debug --check: lldb missing" "$root/5.out" '--   lldb: not found'
check "debug --check: python missing" "$root/5.out" '--   Python 3.8 or later: not found'
check "debug --check: summary" "$root/5.out" 'iron debug: not ready, no debugger'
check "debug --check: formatters" "$root/5.out" 'iron_lldb.py'

# 6. iron debug --check with a working gdb: iron debug is ready.
bin_gdb15=$root/gdb15; mkdir -p "$bin_gdb15"
printf '#!/bin/sh\necho "GNU gdb (GDB) 15.1"\n' > "$bin_gdb15/gdb"; chmod +x "$bin_gdb15/gdb"
PATH=$bin_gdb15 "$iron" debug --check > "$root/6.out" 2>&1
check "debug --check: gdb found" "$root/6.out" "ok   GNU gdb (GDB) 15.1 ($bin_gdb15/gdb)"
check "debug --check: ready" "$root/6.out" 'iron debug: ready (gdb)'

exit $fail
