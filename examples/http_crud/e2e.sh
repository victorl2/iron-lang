#!/usr/bin/env bash
# e2e.sh — build and run the full http_crud client/server exercise.
#
# Phase 1: fresh store  -> server + client (full CRUD incl. binary payload)
# Phase 2: server restart -> client_verify (persistence across process death)
# The raw store file is hex-dumped after each phase so the binary layout
# (IRDB magic, version, big-endian length prefixes) is visible in the log.
#
# Requires `ironc` on PATH (override with IRONC=/path/to/ironc).
# Usage: e2e.sh [workdir]   (defaults to a fresh mktemp -d)
set -euo pipefail

IRONC="${IRONC:-ironc}"
DIR="$(cd "$(dirname "$0")" && pwd)"
WORK="${1:-$(mktemp -d)}"
mkdir -p "$WORK"
cd "$WORK"
rm -f records.bin server client client_verify server1.log server2.log client1.log client2.log

echo "== building =="
"$IRONC" build "$DIR/server.iron" -o server
"$IRONC" build "$DIR/client.iron" -o client
"$IRONC" build "$DIR/client_verify.iron" -o client_verify

echo "== phase 1: fresh store =="
./server > server1.log 2>&1 &
SRV=$!
sleep 1
./client | tee client1.log
wait "$SRV"
echo "-- server1 log --"
cat server1.log
echo "-- records.bin after phase 1 --"
od -A d -t x1 -c records.bin
grep -q "ALL CHECKS PASSED" client1.log

echo "== phase 2: restart + verify persistence =="
./server > server2.log 2>&1 &
SRV=$!
sleep 1
./client_verify | tee client2.log
wait "$SRV"
echo "-- server2 log --"
cat server2.log
echo "-- records.bin after phase 2 --"
od -A d -t x1 -c records.bin
grep -q "ALL CHECKS PASSED" client2.log

echo "E2E COMPLETE: both phases passed"
