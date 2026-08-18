#!/usr/bin/env bash
# One scenario2 cell: fresh server, fresh data file, one driver run, clean
# shutdown - the unit bench/results-scenario2-freight.md's matrix is built
# from. Every cell gates on wait_quiet.sh, records the machine's load for its
# own life, and hashes the binary it started, so a row that shared the box
# with a compiler can be discarded rather than reported.
#
#   run_s2_cell.sh <label> <server-binary> <port> <driver-dir> [-- driver flags...]
#
# Pass a **copy** of build-release/kds_server as <server-binary>, never the
# build tree's own: the tree is shared, and a rebuild landing between two
# cells would measure two engines under one heading. The .txt output records
# the copy's sha256 so the document can name what it measured.
#
# Env: EXTRA_CONF   extra "key = value" lines appended to the cell's config
#      CORES        the config's `cores` (default 1)
#      ROOT         where cells and output land (default $HOME/bench-s2-refresh)
#      OUTDIR       where .json/.txt land (default $ROOT/out)
set -euo pipefail

label=$1; binary=$2; port=$3; driverdir=$4; shift 4
[[ "${1:-}" == "--" ]] && shift

ROOT=${ROOT:-$HOME/bench-s2-refresh}
OUTDIR=${OUTDIR:-$ROOT/out}
CELL=$ROOT/cells/$label
mkdir -p "$OUTDIR" "$CELL"
rm -f "$CELL"/*.db "$CELL"/*.wal "$CELL"/*.log 2>/dev/null || true

conf=$CELL/kds.conf
cat > "$conf" <<EOF
data_file = $CELL/s2.db
cores = ${CORES:-1}
placement = creating
port = $port
durability = group
log_dir = $CELL
log_file = server.log
log_level = info
EOF
if [[ -n "${EXTRA_CONF:-}" ]]; then printf '%s\n' "$EXTRA_CONF" >> "$conf"; fi

# Nothing else may hold the port: an early run of the 2026-08-11 matrix
# reached a foreign server and measured its data file.
if ss -ltn 2>/dev/null | grep -q ":$port "; then
    echo "port $port already bound - refusing" >&2; exit 2
fi

"$driverdir/bench/wait_quiet.sh"

echo "== $label ==" | tee "$OUTDIR/$label.txt"
{ echo "binary: $binary"; echo "binary mtime: $(date -u -r "$binary" +%FT%TZ)";
  echo "binary sha256: $(sha256sum "$binary" | cut -d' ' -f1)";
  echo "uptime before: $(uptime)"; } | tee -a "$OUTDIR/$label.txt"

"$binary" --config "$conf" > "$CELL/stdout.log" 2>&1 &
srv=$!
for _ in $(seq 1 100); do
    ss -ltn 2>/dev/null | grep -q ":$port " && break
    kill -0 $srv 2>/dev/null || { echo "server died" >&2; tail -20 "$CELL/stdout.log" >&2; exit 3; }
    sleep 0.2
done

# Sample the load for the life of the run.
( while kill -0 $srv 2>/dev/null; do
      cut -d' ' -f1-3 /proc/loadavg; sleep 5
  done ) > "$CELL/load.samples" &
sampler=$!

set +e
python3 "$driverdir/tools/scenario2_freight.py" --port "$port" --json "$OUTDIR/$label.json" "$@" \
    >> "$OUTDIR/$label.txt" 2>&1
rc=$?
set -e

kill $sampler 2>/dev/null || true
kill -TERM $srv 2>/dev/null || true
wait $srv 2>/dev/null || true

{ echo "driver exit: $rc";
  echo "load samples max: $(sort -g "$CELL/load.samples" 2>/dev/null | tail -1)";
  echo "data file bytes: $(stat -c%s "$CELL/s2.db" 2>/dev/null || echo NA)";
  echo "uptime after: $(uptime)"; } | tee -a "$OUTDIR/$label.txt"
exit $rc
