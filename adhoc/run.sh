#!/bin/sh
# Runs the ad-hoc .sql scripts against a throwaway server and shuts it
# down again - so nothing here touches a database you care about.
#
#   adhoc/run.sh                     every script, in order
#   adhoc/run.sh 02_subqueries.sql   the setup, then just that one
#   adhoc/run.sh --quiet             only failures and unchecked statements
#
# The fixture (00_setup.sql) always runs first: every other script assumes
# its rows, and the ids are positional (alice=1 .. carol=4).
#
# To iterate on a query instead, start a server yourself and use the CLI or
# tools/run_sql.py directly - the point of a .sql script is that adding a
# query and its expected answer is two adjacent lines, no code change:
#
#   build/kds_server /tmp/scratch.db --port 15499 &
#   python3 tools/run_sql.py adhoc/00_setup.sql --port 15499
#   python3 tools/ckdbs_cli.py --port 15499 "SELECT * FROM acct"
#
# Exit status is run_sql.py's: 0 when every expectation held.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${KDS_PORT:-15498}"
SERVER="$ROOT/build/kds_server"

if [ ! -x "$SERVER" ]; then
    echo "no server binary at $SERVER - build it first:" >&2
    echo "  ./scripts/build.sh" >&2
    exit 1
fi

QUIET=""
SCRIPTS=""
for arg in "$@"; do
    case "$arg" in
        --quiet) QUIET="--quiet" ;;
        *) SCRIPTS="$SCRIPTS $ROOT/adhoc/$arg" ;;
    esac
done
if [ -z "$SCRIPTS" ]; then
    SCRIPTS="$ROOT/adhoc/00_setup.sql $ROOT/adhoc/01_joins.sql $ROOT/adhoc/02_subqueries.sql"
else
    # The fixture first, always - a named script assumes its rows.
    SCRIPTS="$ROOT/adhoc/00_setup.sql $SCRIPTS"
fi

WORKDIR="$(mktemp -d)"
SERVER_PID=""

cleanup() {
    # By pid, never by name: `pkill -f kds_server` also matches the shell
    # running this script.
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM

"$SERVER" "$WORKDIR/adhoc.db" --port "$PORT" --log-level warn > "$WORKDIR/server.log" 2>&1 &
SERVER_PID=$!

i=0
while [ "$i" -lt 50 ]; do
    if python3 -c "import socket,sys
try:
    socket.create_connection(('127.0.0.1', $PORT), timeout=0.2).close()
except OSError:
    sys.exit(1)" 2>/dev/null; then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done
if [ "$i" -ge 50 ]; then
    echo "server did not come up on port $PORT; log follows:" >&2
    cat "$WORKDIR/server.log" >&2
    exit 1
fi

cd "$ROOT/tools"
# NOT `exec`: exec replaces this shell, and with it the EXIT trap - which
# would orphan the server and leave the temp database behind. A later run
# then connects to the *old* server, finds the fixture already loaded, and
# reports doubled row counts. `set -e` is disabled around the call so a
# failing script still reaches the trap.
set +e
# shellcheck disable=SC2086
python3 run_sql.py $SCRIPTS --port "$PORT" $QUIET
STATUS=$?
set -e
exit "$STATUS"
