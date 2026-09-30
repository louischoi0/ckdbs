#!/bin/bash
# Scenario 3 and 4 PostgreSQL 18.6 floor at f2f1ee7. Fresh cluster per cell.
# s3: --loans {200,1000,10000} x --index-mode {none,all} x 3 runs, --seed 1
#     (KDS cells: run_s3.sh at 9a0525d; the PG twin has no --server-indexes,
#     --verify or --assert-index-reads).
# s4: twin defaults (one unpaced business day, no index), 3 runs.
set -uo pipefail
source /home/cdkbs/bench-runs/pg18-f2f1ee7/lib.sh
cell3() {  # tag loans mode
    TAG=$1; N=$2; M=$3
    want "$TAG" || return
    echo "== $TAG start $(date -u +%FT%TZ) ==" >> "$RUN/timeline"
    precheck "$TAG"
    fresh_pg "$TAG" || { echo "== $TAG PG INIT FAILED ==" >> "$RUN/timeline"; return; }
    python3 "$TOOLS/pg_scenario3_library.py" --port "$PGPORT" --user "$(id -un)" --loans "$N" \
        --index-mode "$M" --seed 1 --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    stop_pg "$TAG"
    echo "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) ==" >> "$RUN/timeline"
}
cell4() {  # tag
    TAG=$1
    want "$TAG" || return
    echo "== $TAG start $(date -u +%FT%TZ) ==" >> "$RUN/timeline"
    precheck "$TAG"
    fresh_pg "$TAG" || { echo "== $TAG PG INIT FAILED ==" >> "$RUN/timeline"; return; }
    python3 "$TOOLS/pg_scenario4_cabinopt_days.py" --port "$PGPORT" --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    stop_pg "$TAG"
    echo "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) ==" >> "$RUN/timeline"
}
echo "S34 START $(date -u +%FT%TZ)" >> "$RUN/timeline"
for R in 1 2 3; do
    for N in 200 1000 10000; do
        for M in none all; do
            cell3 s3-pg-n$N-$M-r$R $N $M
        done
    done
    cell4 s4-pg-r$R
done
echo "S34 END $(date -u +%FT%TZ)" >> "$RUN/timeline"
touch "$RUN/s34.done"
