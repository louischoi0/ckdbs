#!/usr/bin/env bash
# OPT-001 interleaved A/B sweep: 200 / 1000 / 10000 rows.
#
# Arm A binary: .../opt001/run/kds_server_A   (004da62, v2.7.0-34-g004da62)
# Arm B binary: .../opt001/run/kds_server_B   (ea1d9d0, v2.7.0-38-gea1d9d0)
#
# Fresh server + fresh data dir per (arm, row count) - per this role's own
# rule 6, catalog rows are never reclaimed and undo never purges, so a
# second run on one file is not a repeat of the first.
set -euo pipefail

SCRATCH=/tmp/claude-1000/-home-cdkbs-ckdbs/e52ac4d4-a740-442c-8ac0-ad035bc82a3b/scratchpad/opt001
RUN_AB=/home/cdkbs/ckdbs/.claude/worktrees/path-optimizer/bench/run_ab_server.sh
DRIVER=$SCRATCH/opt001_ab.py
OUT=/home/cdkbs/ckdbs/.claude/worktrees/path-optimizer/CIP/OPT-001-update-decode-order/archive
DATADIR=/home/cdkbs/bench-opt001

PORT_A=15921
PORT_B=15922

for ROWS in 200 1000 10000; do
    echo "==== rows=$ROWS ===="
    rm -rf "$DATADIR/a-$ROWS" "$DATADIR/b-$ROWS"
    PID_A=$("$RUN_AB" "$SCRATCH/run/kds_server_A" "$PORT_A" "$DATADIR/a-$ROWS" relaxed)
    PID_B=$("$RUN_AB" "$SCRATCH/run/kds_server_B" "$PORT_B" "$DATADIR/b-$ROWS" relaxed)
    echo "server A pid=$PID_A  server B pid=$PID_B"

    python3 "$DRIVER" \
        --port-a "$PORT_A" --port-b "$PORT_B" --pid-a "$PID_A" --pid-b "$PID_B" \
        --rows "$ROWS" --seed 20260901 \
        --lat-rounds 20 --lat-select 20 --lat-update 20 --lat-delete 6 --lat-wide 15 --lat-subchain 4 \
        --cpu-rounds 20 --cpu-select 800 --cpu-update 800 --cpu-delete 60 --cpu-wide 500 --cpu-subchain 80 \
        --json "$OUT/run$ROWS.json" 2>&1 | tee "$OUT/run$ROWS.log"

    kill "$PID_A" "$PID_B" 2>/dev/null || true
    wait "$PID_A" "$PID_B" 2>/dev/null || true
    rm -rf "$DATADIR/a-$ROWS" "$DATADIR/b-$ROWS"
    echo "==== rows=$ROWS done ===="
done
