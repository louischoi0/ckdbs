#!/bin/bash
# Scenario 2, one extra knob: --isolation repeatable-read, cores 1 and 8, group.
# Same arguments as run_s02.sh otherwise. Run once each (a probe of the
# invariant failures, not a matrix).
set -uo pipefail
source /home/cdkbs/bench-runs/rebaseline-9a0525d/lib.sh
S2_ARGS="--organizations 300 --ships 30 --operations 300 --cargos 4000 --bookers 8 --bookings 3000 --verify 100 --seed 1 --sync --isolation repeatable-read"
cell2() {
    TAG=$1; SUF=$2; PORT=$3; CORES=$4; DUR=$5
    echo "== $TAG start $(date -u +%FT%TZ) ==" >> "$RUN/timeline"
    precheck "$TAG"
    if ! start_server "$TAG" "$PORT" "$CORES" "$DUR" >> "$RUN/logs/$TAG.startup" 2>&1; then
        echo "== $TAG SERVER FAILED ==" >> "$RUN/timeline"; return; fi
    python3 "$TOOLS/scenario2_freight.py" --port "$PORT" --suffix "$SUF" $S2_ARGS --schema-only > "$RUN/logs/$TAG.schema" 2>&1
    python3 "$TOOLS/scenario2_freight.py" --port "$PORT" --suffix "$SUF" $S2_ARGS --load-only > "$RUN/logs/$TAG.load" 2>&1
    python3 "$TOOLS/scenario2_freight.py" --port "$PORT" --suffix "$SUF" $S2_ARGS --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    stop_server "$TAG" >> "$RUN/logs/$TAG.startup" 2>&1
    echo "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) ==" >> "$RUN/timeline"
}
cell2 rr-s2-c1-g s2rrc1g 15604 1 group
cell2 rr-s2-c8-g s2rrc8g 15605 8 group
touch "$RUN/s2rr.done"
