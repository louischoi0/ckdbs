#!/usr/bin/env bash
# CP-S0 cell M5 (candidate C3's price, BP-H10's shape): a held range fence
# beside point writers it does not cover. One kds_server (the base binary,
# 4 cores, relaxed, pinned 2-5), both ports open; tools/cp_s0_fence.py run
# four times against it - {kwp, text} x {1, 4 writers} - each on its own
# relation. Sized to about 2-3 minutes at 0.1-0.3 ms per statement over KWP
# from Python (run_m1m4.sh's figure; four GIL-sharing writer threads cost
# more per statement than one), bounded under 7 by `timeout` per cell. A
# cell `timeout` ends closes its sockets, and the server rolls an open
# transaction back at the disconnect (`CloseClient`, tcp_server.cpp:505,
# :939), so a killed cell leaves no fence standing for the next one; its
# finished arms are in progress-<cell>.json (`blocks`).
# The driver is passed `--server-pid` from the sidecar, so each arm also
# carries the server's utime+stime per thread (the order's "point write's
# CPU"); the latency delta stays the primary number.
#
# Launcher interface assumed (read from m1m4/srv.py's argparse):
#   srv.py start --binary BIN --workdir DIR --port P --debug-text-port T
#                --cores N --taskset 2-5 --durability relaxed [--allow-tmpfs] [--label L]
#   srv.py stop  --workdir DIR
# (not `--bin` / `--pin` / `--text-port`). srv.py refuses a tmpfs workdir
# unless --allow-tmpfs; the session scratchpad is on tmpfs on this box, so
# the default workdir below passes it and the sidecar records the df -T
# (bench/README.md rule 2 is then stated as violated in the results file).
# Set M5_WORKDIR_BASE to an ext4 path (e.g. /var/tmp/cp-s0-m5) to honour it.
#
# CPU layout on this box: SMT pairs (0,4) (1,5) (2,6) (3,7). The server's
# `--taskset 2-5` puts one reactor on each physical core; the driver runs on
# 0,1, which are the siblings of reactors on 4,5 - no placement on 8 CPUs
# keeps a 4-reactor server and the driver on distinct physical cores.
#
# Watchdog: the driver rewrites $OUT/progress-<cell>.json after every arm;
# $OUT/DONE is written at the end. A stale progress file (> 60 s) with no
# DONE is an orphaned run.

set -euo pipefail

SCRATCH=/tmp/claude-1000/-home-louis-ckdbs/5bdaae98-e052-4814-8a2c-eb67c5cf878d/scratchpad/cp-close
M5="$SCRATCH/m5"
TREE=${M5_TREE:-/home/louis/ckdbs/.claude/worktrees/worktree-cp-lock-model-exploration}
BIN=${M5_BIN:-$SCRATCH/bin/kds_server-base}
SRV=${M5_SRV:-$SCRATCH/m1m4/srv.py}
DRIVER="$TREE/tools/cp_s0_fence.py"

PORT=${M5_PORT:-15561}            # KWP/1, chosen (bench/README.md rule 5: never 15432)
TEXT_PORT=${M5_TEXT_PORT:-15562}  # debug_text_port
CORES=${M5_CORES:-4}
SERVER_CPUS=${M5_SERVER_CPUS:-2-5}
DRIVER_CPUS=${M5_DRIVER_CPUS:-0,1}
ROWS=${M5_ROWS:-4096}
OPS=${M5_OPS:-3000}               # point UPDATEs per writer per arm per block
BLOCKS=${M5_BLOCKS:-8}            # interleaved blocks, each fenced + unfenced
CELL_TIMEOUT=${M5_CELL_TIMEOUT:-100s}   # 4 writers x 3000 ops x 16 arms = 192,000 statements at up to 0.5 ms
WORKDIR_BASE=${M5_WORKDIR_BASE:-$M5/srv}

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
OUT="$M5/out-$STAMP"
WORKDIR="$WORKDIR_BASE/m5-$STAMP"
mkdir -p "$OUT" "$WORKDIR_BASE"

for f in "$BIN" "$SRV" "$DRIVER"; do
    [ -r "$f" ] || { echo "run_m5.sh: missing $f" >&2; exit 2; }
done

{
    echo "started_utc=$(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    echo "tree=$TREE"
    echo "describe=$(git -C "$TREE" describe --tags --always 2>/dev/null || echo unknown)"
    echo "head=$(git -C "$TREE" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "binary=$BIN"
    echo "binary_sha256=$(sha256sum "$BIN" | cut -d' ' -f1)"
    echo "workdir=$WORKDIR"
    echo "server_cpus=$SERVER_CPUS driver_cpus=$DRIVER_CPUS cores=$CORES"
    echo "rows=$ROWS ops=$OPS blocks=$BLOCKS"
    echo "loadavg=$(cat /proc/loadavg)"
    echo "competing=$(pgrep -a -f 'cc1plus|cmake --build|ctest' | tr '\n' ';' || true)"
} | tee "$OUT/run.txt"

ALLOW_TMPFS=()
FSTYPE=$(df -T "$WORKDIR_BASE" | awk 'NR==2 {print $2}')
if [ "$FSTYPE" = "tmpfs" ]; then
    ALLOW_TMPFS=(--allow-tmpfs)
    echo "workdir_fstype=tmpfs (bench/README.md rule 2 violated; set M5_WORKDIR_BASE to an ext4 path)" | tee -a "$OUT/run.txt"
else
    echo "workdir_fstype=$FSTYPE" | tee -a "$OUT/run.txt"
fi

stop_server() {
    set +e
    python3 "$SRV" stop --workdir "$WORKDIR" > "$OUT/server-stop.json" 2>&1
    cp -f "$WORKDIR/server.json" "$OUT/server.json" 2>/dev/null
    cp -f "$WORKDIR/server.out" "$OUT/server.out" 2>/dev/null
    echo "stopped_utc=$(date -u '+%Y-%m-%d %H:%M:%S UTC')" >> "$OUT/run.txt"
}
trap stop_server EXIT

python3 "$SRV" start --binary "$BIN" --workdir "$WORKDIR" --port "$PORT" \
    --debug-text-port "$TEXT_PORT" --cores "$CORES" --taskset "$SERVER_CPUS" \
    --durability relaxed --label m5 "${ALLOW_TMPFS[@]}" | tee "$OUT/server-start.json"
SERVER_PID=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["pid"])' "$WORKDIR/server.json")
echo "server_pid=$SERVER_PID" | tee -a "$OUT/run.txt"

# Cells: protocol x writers. kwp spreads sessions over the cores (BP-H10's
# cross-core shape); text puts every session on core 0 (one reactor, no
# cross-core latch contention). 4 writers first on each protocol: it is the
# cell C3's rule reads.
for protocol in kwp text; do
    if [ "$protocol" = kwp ]; then port=$PORT; else port=$TEXT_PORT; fi
    for writers in 4 1; do
        cell="$protocol-w$writers"
        echo "== cell $cell $(date -u '+%H:%M:%S UTC')" | tee -a "$OUT/run.txt"
        set +e
        timeout --foreground "$CELL_TIMEOUT" taskset -c "$DRIVER_CPUS" \
            python3 "$DRIVER" --port "$port" --protocol "$protocol" --rows "$ROWS" \
            --writers "$writers" --ops "$OPS" --blocks "$BLOCKS" \
            --label "$cell" --suffix "${cell//-/_}" --server-pid "$SERVER_PID" \
            --progress "$OUT/progress-$cell.json" --json "$OUT/m5-$cell.json" \
            > "$OUT/m5-$cell.txt" 2> "$OUT/m5-$cell.err"
        rc=$?
        set -e
        echo "cell=$cell rc=$rc" | tee -a "$OUT/run.txt"
        if [ "$rc" -ne 0 ]; then
            echo "run_m5.sh: cell $cell exited $rc; $OUT/m5-$cell.err ends:" >&2
            tail -n 20 "$OUT/m5-$cell.err" >&2 || true
        fi
    done
done

date -u '+%Y-%m-%d %H:%M:%S UTC' > "$OUT/DONE"
echo "results under $OUT"
