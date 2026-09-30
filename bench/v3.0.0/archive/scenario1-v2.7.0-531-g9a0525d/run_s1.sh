#!/bin/bash
# Scenario 1 (backtest) at 9a0525d. No v3 predecessor. Driver defaults
# (30 years x 8 symbols, btree bars, all sweeps, --verify on) plus --sync and
# --analyze. Cells: cores 1/8 x group, and cores 1 strict.
set -uo pipefail
source /home/cdkbs/bench-runs/rebaseline-9a0525d/lib.sh
PORT=15610
cell1() {  # tag cores durability
    TAG=$1; CORES=$2; DUR=$3
    echo "== $TAG start $(date -u +%FT%TZ) ==" >> "$RUN/timeline"
    precheck "$TAG"
    if ! start_server "$TAG" "$PORT" "$CORES" "$DUR" >> "$RUN/logs/$TAG.startup" 2>&1; then
        echo "== $TAG SERVER FAILED ==" >> "$RUN/timeline"; return; fi
    python3 "$TOOLS/scenario1_backtest.py" --port "$PORT" --suffix ${TAG//-/_} --seed 1 \
        --bars-clustered btree --verify --sync --analyze \
        --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    stop_server "$TAG" >> "$RUN/logs/$TAG.startup" 2>&1
    echo "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) ==" >> "$RUN/timeline"
}
echo "S1 START $(date -u +%FT%TZ)" >> "$RUN/timeline"
cell1 s1-c1-g 1 group
cell1 s1-c8-g 8 group
cell1 s1-c1-s 1 strict
echo "S1 END $(date -u +%FT%TZ)" >> "$RUN/timeline"
touch "$RUN/s1.done"
