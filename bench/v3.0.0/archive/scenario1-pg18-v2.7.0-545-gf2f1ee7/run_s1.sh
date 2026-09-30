#!/bin/bash
# Scenario 1 PostgreSQL 18.6 standalone floor at f2f1ee7: driver defaults, one run.
# KDS refuses scenario 1 (SUS-1), so there is no KDS counterpart.
set -uo pipefail
source /home/cdkbs/bench-runs/pg18-f2f1ee7/lib.sh
TAG=s1-pg-r1
echo "== $TAG start $(date -u +%FT%TZ) ==" >> "$RUN/timeline"
precheck "$TAG"
fresh_pg "$TAG" || { echo "== $TAG PG INIT FAILED ==" >> "$RUN/timeline"; exit 1; }
python3 "$TOOLS/pg_scenario1_backtest.py" --port "$PGPORT" --user "$(id -un)" --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
echo "$?" > "$RUN/logs/$TAG.rc"
stop_pg "$TAG"
echo "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) ==" >> "$RUN/timeline"
touch "$RUN/s1.done"
