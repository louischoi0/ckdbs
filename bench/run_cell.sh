#!/usr/bin/env bash
# One measured cell of the scenario3 matrix: a fresh server, a fresh data
# file, one driver invocation, then teardown.
#
# It exists because a benchmark whose numbers cannot be traced back to the
# machine state that produced them is a data dump, not a measurement. Every
# cell therefore records `uptime` and `pgrep -c cc1plus` immediately before
# and after the driver, so a reader can see whether a build was
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
# bare `|| echo 0` fallback appends a second line and yields "0\n0". `|| true`
# keeps the count and drops the status, which also keeps `set -e` out of it:
# `n=$(failing-pipeline)` aborts the shell when this is called anywhere but
# inside a command substitution.
cc1plus_count() {
    local n
    n=$(pgrep -c cc1plus 2>/dev/null || true)
    echo "${n:-0}"
}

# True when something is already accepting connections on the port.
port_open() {
    python3 -c "import socket,sys
s = socket.socket()
s.settimeout(0.2)
sys.exit(0 if s.connect_ex(('127.0.0.1', $KDS_PORT)) == 0 else 1)" 2>/dev/null
}

SRV=
teardown() {
    [ -n "$SRV" ] || return 0
    kill "$SRV" 2>/dev/null || true
    wait "$SRV" 2>/dev/null || true
    SRV=
}

if [ $# -lt 3 ]; then
    echo "usage: $0 <cell-name> <config> -- <driver args...>" >&2
    exit 2
fi
CELL=$1; CONFIG=$2; shift 2
[ "${1:-}" = "--" ] && shift

# Checked before anything is deleted. $CELL becomes an `rm -rf` argument
# below, and an empty one expands "$S3ROOT/wal/$CELL" to the WAL *root*:
# one mistyped call would take every other cell's log directory with it.
case "$CELL" in
    ""|*/*)
        echo "cell name must be non-empty and contain no '/': '$CELL'" >&2
        exit 2
        ;;
esac
[ -r "$CONFIG" ] || { echo "config not readable: $CONFIG" >&2; exit 2; }
[ -x "$KDS_SERVER" ] || { echo "server not executable: $KDS_SERVER" >&2; exit 2; }

# A server left over from an earlier cell answers the readiness probe below
# just as well as this cell's does, and then serves the driver out of *its*
# data file under *its* config. That is a wrong number rather than a failed
# run, so refuse the port instead of measuring through it.
if port_open; then
    echo "port $KDS_PORT already has a listener; refusing to measure against it" >&2
    exit 4
fi

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

# The config file carries the data_file and wal_dir for this cell only. The
# blank line is load-bearing: a base config whose last line has no trailing
# newline would otherwise absorb `data_file` into that line's value, the key
# would go missing, and the server would fall back to its default `kds.db`
# in the CWD -- shared by every cell and never cleared by the rm above. The
# parser skips blank lines (src/server/config_file.cpp), so it costs nothing.
CELLCONF="$S3ROOT/conf/$CELL.conf"
{
    cat "$CONFIG"
    echo
    echo "data_file = $DB"
    echo "wal_dir = $WAL"
    echo "port = $KDS_PORT"
} >"$CELLCONF"

"$KDS_SERVER" --config "$CELLCONF" >>"$LOG" 2>&1 &
SRV=$!
trap teardown EXIT

# Wait for the listener rather than sleeping a guess, and stop waiting the
# moment the server is gone -- a refused bind or a bad config key otherwise
# costs the full 20 s before the same exit 3.
READY=0
for _ in $(seq 1 200); do
    if port_open; then
        READY=1
        break
    fi
    kill -0 "$SRV" 2>/dev/null || break
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
    # The driver writes its JSON before it can know the cell is being
    # discarded, so a complete-looking result is already on disk: leaving it
    # there lets a skip-if-exists matrix adopt the contended run as the
    # cell's number. Both artefacts move aside rather than being deleted,
    # because the re-run overwrites the log and the record that a discard
    # happened at all would otherwise vanish with it.
    mv -f "$JSON" "$JSON.contended" 2>/dev/null || true
    mv -f "$LOG" "$LOG.contended" 2>/dev/null || true
    echo "$CELL rc=$RC CONTENDED json=$JSON.contended log=$LOG.contended"
    exit 8
fi

teardown
trap - EXIT

echo "$CELL rc=$RC json=$JSON log=$LOG"
exit $RC
