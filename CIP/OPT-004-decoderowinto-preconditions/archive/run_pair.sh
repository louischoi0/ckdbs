#!/usr/bin/env bash
# OPT-004 interleaved A/B sweep for one pair: 200 / 1000 / 10000 rows.
#
#   run_pair.sh <pair-name> <binary-a> <binary-b> <out-dir>
#
# Fresh server + fresh data dir per (arm, row count), per this role's own
# rule 6 - catalog rows are never reclaimed and undo never purges, so a
# second run on one file is not a repeat of the first.
set -euo pipefail

PAIR="$1"
BIN_A="$2"
BIN_B="$3"
OUT="$4"

RUN_AB=/home/cdkbs/ckdbs/.claude/worktrees/path-optimizer/bench/run_ab_server.sh
DRIVER=/tmp/claude-1000/-home-cdkbs-ckdbs/e52ac4d4-a740-442c-8ac0-ad035bc82a3b/scratchpad/opt004/driver/opt004_ab.py
DATADIR=/home/cdkbs/bench-opt004

mkdir -p "$OUT"

if [ "$PAIR" = "pair1" ]; then
    PORT_A=15931
    PORT_B=15932
else
    PORT_A=15941
    PORT_B=15942
fi

for ROWS in 200 1000 10000; do
    echo "==== $PAIR rows=$ROWS ===="
    rm -rf "$DATADIR/${PAIR}-a-$ROWS" "$DATADIR/${PAIR}-b-$ROWS"
    PID_A=$("$RUN_AB" "$BIN_A" "$PORT_A" "$DATADIR/${PAIR}-a-$ROWS" relaxed)
    PID_B=$("$RUN_AB" "$BIN_B" "$PORT_B" "$DATADIR/${PAIR}-b-$ROWS" relaxed)
    echo "server A pid=$PID_A  server B pid=$PID_B"

    case "$ROWS" in
        200)
            LAT="--lat-full-scan 15 --lat-analyze-scan 30 --lat-update 30 --lat-subchain 15"
            CPU="--cpu-full-scan 200 --cpu-analyze-scan 1200 --cpu-update 2000 --cpu-subchain 300"
            ;;
        1000)
            LAT="--lat-full-scan 12 --lat-analyze-scan 25 --lat-update 25 --lat-subchain 10"
            CPU="--cpu-full-scan 80 --cpu-analyze-scan 600 --cpu-update 1200 --cpu-subchain 120"
            ;;
        10000)
            LAT="--lat-full-scan 8 --lat-analyze-scan 20 --lat-update 20 --lat-subchain 6"
            CPU="--cpu-full-scan 25 --cpu-analyze-scan 250 --cpu-update 300 --cpu-subchain 20"
            ;;
    esac

    # shellcheck disable=SC2086
    python3 "$DRIVER" \
        --port-a "$PORT_A" --port-b "$PORT_B" --pid-a "$PID_A" --pid-b "$PID_B" \
        --rows "$ROWS" --seed 20260901 \
        --lat-rounds 15 --cpu-rounds 20 \
        $LAT $CPU \
        --json "$OUT/${PAIR}-run$ROWS.json" 2>&1 | tee "$OUT/${PAIR}-run$ROWS.log"

    kill "$PID_A" "$PID_B" 2>/dev/null || true
    wait "$PID_A" "$PID_B" 2>/dev/null || true
    rm -rf "$DATADIR/${PAIR}-a-$ROWS" "$DATADIR/${PAIR}-b-$ROWS"
    echo "==== $PAIR rows=$ROWS done ===="
done
