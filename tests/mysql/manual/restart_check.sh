#!/bin/bash
# restart_check.sh - restart the MySQL server under a running pool.
#
#   tests/mysql/manual/restart_check.sh <ironc> <restart-command>
#
# Builds restart.iron, runs it against IRON_MYSQL_URL for RESTART_SECONDS
# (default 30), and runs <restart-command> (for example
# "podman restart mysql-test") after a third of that time. Passes when the
# program reports recovery.
set -u
here=$(cd "$(dirname "$0")" && pwd)
ironc=$1; restart=$2
secs=${RESTART_SECONDS:-30}
bin=$(mktemp -d)/restart
"$ironc" build "$here/restart.iron" -o "$bin" >/dev/null || exit 1
RESTART_SECONDS=$secs "$bin" & pid=$!
sleep $((secs / 3))
echo "== restarting: $restart"
sh -c "$restart"
wait $pid
