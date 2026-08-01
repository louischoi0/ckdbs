#!/bin/sh
# Starts a throwaway ckdbs server, runs the v2 grammar scripts against it,
# and shuts it down again - so neither script needs a server you started
# by hand, and neither touches a database you care about.
#
#   tools/run_grammar_v2.sh            check (pass/fail) then demo (readable)
#   tools/run_grammar_v2.sh check      only the assertions
#   tools/run_grammar_v2.sh demo       only the walkthrough
#
# The data file lives in a mktemp directory and is deleted on exit. The
# port is high and fixed; override with KDS_PORT=... if it collides.
#
# Exit status is the check script's: 0 when every case matched the outcome
# it expected. `demo` alone always exits 0 - a refusal there is content.
set -e

MODE="${1:-all}"
PORT="${KDS_PORT:-15499}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERVER="$ROOT/build/kds_server"

if [ ! -x "$SERVER" ]; then
    echo "no server binary at $SERVER - build it first:" >&2
    echo "  ./scripts/build.sh" >&2
    exit 1
fi

WORKDIR="$(mktemp -d)"
SERVER_PID=""

cleanup() {
    # Kill by the pid we started, never by name: pkill -f 'kds_server'
    # also matches the shell running this script.
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM

"$SERVER" "$WORKDIR/grammar.db" --port "$PORT" --log-level warn \
    > "$WORKDIR/server.log" 2>&1 &
SERVER_PID=$!

# Wait for the port rather than sleeping a guessed interval.
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
case "$MODE" in
    check) python3 grammar_v2_check.py --port "$PORT" ;;
    demo)  python3 grammar_v2_demo.py  --port "$PORT" ;;
    all)
        python3 grammar_v2_check.py --port "$PORT"
        echo
        echo "########################################################################"
        echo
        python3 grammar_v2_demo.py --port "$PORT"
        ;;
    *)
        echo "usage: $0 [check|demo|all]" >&2
        exit 2
        ;;
esac
