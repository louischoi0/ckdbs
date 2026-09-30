#!/bin/bash
# Scenario 0 PostgreSQL 18.6 floor at f2f1ee7. Same driver arguments as the
# KDS cells (bench/v3.0.0/archive/scenario0-v2.7.0-531-g9a0525d/run_s02.sh):
# --users 100 --accounts-per-user 3 --assets 30 --traders 8 --txn-per-user 50
# --verify 200 --seed 1 --sync. Fresh cluster (initdb) per cell.
# Cells: pg-on (synchronous_commit = on, server default) x3, and the labelled
# NON-DURABLE pg-off x3.
set -uo pipefail
source /home/cdkbs/bench-runs/pg18-f2f1ee7/lib.sh
S0_ARGS="--users 100 --accounts-per-user 3 --assets 30 --traders 8 --txn-per-user 50 --verify 200 --seed 1 --sync"
cell() {  # tag sc-mode
    TAG=$1; SC=$2
    want "$TAG" || return
    echo "== $TAG start $(date -u +%FT%TZ) ==" >> "$RUN/timeline"
    precheck "$TAG"
    fresh_pg "$TAG" || { echo "== $TAG PG INIT FAILED ==" >> "$RUN/timeline"; return; }
    EXTRA=""
    [ "$SC" != default ] && EXTRA="--synchronous-commit $SC"
    python3 "$TOOLS/pg_scenario0_stockmarket.py" --port "$PGPORT" --user "$(id -un)" $S0_ARGS $EXTRA \
        --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    stop_pg "$TAG"
    echo "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) ==" >> "$RUN/timeline"
}
echo "S0 START $(date -u +%FT%TZ)" >> "$RUN/timeline"
for R in 1 2 3; do
    cell s0-pg-on-r$R default
    cell s0-pg-off-r$R off
done
echo "S0 END $(date -u +%FT%TZ)" >> "$RUN/timeline"
touch "$RUN/s0.done"
