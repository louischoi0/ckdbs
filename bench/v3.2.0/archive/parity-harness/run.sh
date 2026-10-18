#!/bin/bash
# run.sh BINARY OUTDIR [parity_bench.py args...]
# Starts one KDS server (cores = 1) from a hashed copy of BINARY, runs
# tools/parity_bench.py against it and the PG 18 twin on 15434, stops the
# server. bench/README.md's five rules: Release binary built at the measured
# commit, data on /dev/root (ext4), a copy hashed, load per cell recorded by
# the driver, port 15791 chosen. OUTDIR/status is rewritten per cell (the
# watchdog's file); OUTDIR/done marks the end.
set -uo pipefail
BIN=$1; OUT=$2; shift 2
HERE=$(cd "$(dirname "$0")" && pwd)
TOOLS=$(cd "$HERE/../../../../tools" && pwd)
RUN=/home/cdkbs/bench-runs/parity
PORT=15791
mkdir -p "$OUT" "$RUN"
rm -rf "$RUN/data"; mkdir -p "$RUN/data"
cp "$BIN" "$RUN/kds_server"
sha256sum "$RUN/kds_server" > "$OUT/binary.sha256"
df -T "$RUN/data" > "$OUT/device.txt"
{ echo "data_file = $RUN/data/kds.db"; echo "cores = 1"; echo "port = $PORT"
  echo "durability = group"; echo "log_dir = $RUN/data"; echo "log_file = kds.log"
  echo "log_level = warn"; echo "buffer_pool_frames = 65536"; } > "$OUT/kds.conf"
{ date -u +%FT%TZ; cat /proc/loadavg; pgrep -a -f "cc1plus|cmake --build|ctest" | grep -v pgrep || echo "no build"; } > "$OUT/precheck.txt"
nohup "$RUN/kds_server" --config "$OUT/kds.conf" > "$RUN/data/stdout.log" 2>&1 &
PID=$!
for i in $(seq 1 150); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break; sleep 0.2; done
PYTHONPATH=$TOOLS python3 "$TOOLS/parity_bench.py" --kds-port $PORT --kds-pid $PID --pg-port 15434 \
    --json "$OUT/cells.json" --status "$OUT/status" "$@" > "$OUT/table.md" 2> "$OUT/stderr.log"
echo $? > "$OUT/rc"
kill $PID; wait $PID 2>/dev/null
cp "$RUN/data/kds.log" "$OUT/kds.serverlog" 2>/dev/null
touch "$OUT/done"
