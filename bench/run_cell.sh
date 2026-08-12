#!/usr/bin/env bash
# One measured cell of the scenario3 matrix: a fresh server, a fresh data
# file, one driver invocation, then teardown.
#
# It exists because a benchmark whose numbers cannot be traced back to the
# machine state that produced them is a data dump, not a measurement. Every
# cell therefore records `uptime` immediately before and after the driver and
# `pgrep -c cc1plus` before it, so a reader can see whether a build was
# competing for the two vCPUs -- the exact contention that voided the
# 2026-08-08 refresh of bench/results-scenario3-library.md.
#
#   ./bench/run_cell.sh <cell-name> <config> -- <driver args...>
#
# Environment: S3ROOT (default $HOME/bench-s3), KDS_SERVER, KDS_PORT.
set -euo pipefail

S3ROOT=${S3ROOT:-$HOME/bench-s3}
KDS_SERVER=${KDS_SERVER:-./build-release/kds_server}
KDS_PORT=${KDS_PORT:-15432}
DRIVER=${DRIVER:-./tools/scenario3_library.py}

# `pgrep -c` prints its count *and* exits non-zero when nothing matched, so a
# bare `|| echo 0` fallback appends a second line and yields "0\n0".
cc1plus_count() {
    local n
    n=$(pgrep -c cc1plus 2>/dev/null | head -1)
    echo "${n:-0}"
}

if [ $# -lt 3 ]; then
    echo "usage: $0 <cell-name> <config> -- <driver args...>" >&2
    exit 2
fi
CELL=$1; CONFIG=$2; shift 2
[ "${1:-}" = "--" ] && shift

DB="$S3ROOT/db/$CELL.kds"
WAL="$S3ROOT/wal/$CELL"
LOG="$S3ROOT/logs/$CELL.log"
JSON="$S3ROOT/json/$CELL.json"
mkdir -p "$S3ROOT/db" "$S3ROOT/wal" "$S3ROOT/logs" "$S3ROOT/json" "$S3ROOT/conf"

# A cell never inherits another cell's pages or log.
rm -rf "$DB" "$WAL"
mkdir -p "$WAL"

{
    echo "=== cell: $CELL"
    echo "=== config: $CONFIG"
    echo "=== driver: $DRIVER $*"
    echo "=== commit: $(git rev-parse --short HEAD) ($(git rev-parse --abbrev-ref HEAD))"
    echo "=== dirty: $([ -n "$(git status --porcelain)" ] && echo true || echo false)"
    echo "=== server mtime: $(stat -c %y "$KDS_SERVER")"
    echo "=== cc1plus before: $(cc1plus_count)"
    echo "=== uptime before: $(uptime)"
} >"$LOG"

# The config file carries the data_file and wal_dir for this cell only.
CELLCONF="$S3ROOT/conf/$CELL.conf"
cat "$CONFIG" >"$CELLCONF"
{
    echo "data_file = $DB"
    echo "wal_dir = $WAL"
    echo "port = $KDS_PORT"
} >>"$CELLCONF"

"$KDS_SERVER" --config "$CELLCONF" >>"$LOG" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null || true; wait $SRV 2>/dev/null || true' EXIT

# Wait for the listener rather than sleeping a guess.
READY=0
for _ in $(seq 1 200); do
    if python3 -c "import socket,sys
s = socket.socket()
s.settimeout(0.2)
sys.exit(0 if s.connect_ex(('127.0.0.1', $KDS_PORT)) == 0 else 1)" 2>/dev/null; then
        READY=1
        break
    fi
    sleep 0.1
done
if [ "$READY" -ne 1 ]; then
    echo "=== server never listened on $KDS_PORT" >>"$LOG"
    exit 3
fi

set +e
python3 "$DRIVER" --port "$KDS_PORT" --json "$JSON" "$@" >>"$LOG" 2>&1
RC=$?
set -e

CC_AFTER=$(cc1plus_count)
echo "=== driver rc: $RC" >>"$LOG"
echo "=== cc1plus after: $CC_AFTER" >>"$LOG"
echo "=== uptime after: $(uptime)" >>"$LOG"

# A compiler that started *during* the cell contaminates it just as surely as
# one that was running before it. Say so in the cell's own result line rather
# than leaving it to be inferred from a load average later.
if [ "$CC_AFTER" -gt 0 ]; then
    echo "=== CONTENDED: $CC_AFTER cc1plus at cell end" >>"$LOG"
    echo "$CELL rc=$RC CONTENDED json=$JSON log=$LOG"
    exit 8
fi

kill $SRV 2>/dev/null || true
wait $SRV 2>/dev/null || true
trap - EXIT

echo "$CELL rc=$RC json=$JSON log=$LOG"
exit $RC
