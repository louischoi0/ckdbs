#!/usr/bin/env bash
# One scenario2 core-matrix cell. Derived from bench/run_s2_cell.sh, with
# two additions this matrix needs: `SHOW META` is read from a core-0
# connection *before* the server is stopped (the cross-owner-2PC and
# range-split counters are the mechanism this matrix is about, and they die
# with the process), and the config takes `peer_listeners`.
#
#   s2_cell.sh <label> <cores> <peer_listeners on|off> <port> -- driver flags...
set -uo pipefail

REPO=/home/ubuntu/ckdbs/.claude/worktrees/v2.7.0
ROOT=$HOME/bench-s2-cores
BIN=$ROOT/run/kds_server
OUTDIR=$ROOT/out

label=$1; cores=$2; plisten=$3; port=$4; shift 4
[[ "${1:-}" == "--" ]] && shift

CELL=$ROOT/cells/$label
mkdir -p "$OUTDIR" "$CELL"
rm -f "$CELL"/*.db "$CELL"/*.wal "$CELL"/*.log "$CELL"/load.samples 2>/dev/null

conf=$CELL/kds.conf
cat > "$conf" <<EOF
data_file = $CELL/s2.db
cores = $cores
placement = creating
port = $port
durability = ${DURABILITY:-group}
peer_listeners = $plisten
log_dir = $CELL
log_file = server.log
log_level = info
EOF
if [[ -n "${EXTRA_CONF:-}" ]]; then printf '%s\n' "$EXTRA_CONF" >> "$conf"; fi

if ss -ltn 2>/dev/null | grep -q ":$port "; then
    echo "port $port already bound - refusing" >&2; exit 2
fi

bash "$REPO/bench/wait_quiet.sh"

echo "== $label ==" | tee "$OUTDIR/$label.txt"
{ echo "cores: $cores   peer_listeners: $plisten   port: $port";
  echo "extra conf: ${EXTRA_CONF:-none}";
  echo "driver flags: $*";
  echo "binary sha256: $(sha256sum "$BIN" | cut -d' ' -f1)";
  echo "uptime before: $(uptime)"; } | tee -a "$OUTDIR/$label.txt"

"$BIN" --config "$conf" > "$CELL/stdout.log" 2>&1 &
srv=$!
for _ in $(seq 1 150); do
    ss -ltn 2>/dev/null | grep -q ":$port " && break
    kill -0 $srv 2>/dev/null || { echo "server died" >&2; tail -20 "$CELL/stdout.log" >&2; exit 3; }
    sleep 0.2
done

( while kill -0 $srv 2>/dev/null; do
      cut -d' ' -f1-3 /proc/loadavg; sleep 5
  done ) > "$CELL/load.samples" &
sampler=$!

/usr/bin/time -v -o "$CELL/srv.time" true 2>/dev/null
srvstat_before=$(cat /proc/$srv/stat 2>/dev/null | awk '{print $14" "$15}')

python3 "$REPO/tools/scenario2_freight.py" --port "$port" --json "$OUTDIR/$label.json" "$@" \
    >> "$OUTDIR/$label.txt" 2>&1
rc=$?

srvstat_after=$(cat /proc/$srv/stat 2>/dev/null | awk '{print $14" "$15}')

# The counters this matrix turns on, read while the process is still alive.
python3 "$REPO/tools/ckdbs_cli.py" --port "$port" "SHOW META" > "$OUTDIR/$label.meta" 2>&1

kill $sampler 2>/dev/null
kill -TERM $srv 2>/dev/null
wait $srv 2>/dev/null

{ echo "driver exit: $rc";
  echo "server utime+stime ticks before: $srvstat_before";
  echo "server utime+stime ticks after:  $srvstat_after";
  echo "load samples max: $(sort -g "$CELL/load.samples" 2>/dev/null | tail -1)";
  echo "data file bytes: $(stat -c%s "$CELL/s2.db" 2>/dev/null || echo NA)";
  echo "uptime after: $(uptime)"; } | tee -a "$OUTDIR/$label.txt"
exit $rc
