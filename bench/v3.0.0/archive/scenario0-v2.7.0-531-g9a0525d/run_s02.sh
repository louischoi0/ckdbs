#!/bin/bash
# Scenario 0 and 2 rebaseline at 9a0525d: the eight cells of the f6ed10c
# BTREE run, same arguments, same cell order. peer_listeners was retired
# (AT-S8) and is not set; placement is retired (AT-S9) and is not set.
set -uo pipefail
P=${P:-}
source /home/cdkbs/bench-runs/rebaseline-9a0525d/lib.sh
S0_ARGS="--users 100 --accounts-per-user 3 --assets 30 --traders 8 --txn-per-user 50 --verify 200 --seed 1 --sync"
S2_ARGS="--organizations 300 --ships 30 --operations 300 --cargos 4000 --bookers 8 --bookings 3000 --verify 100 --seed 1 --sync"

cell0() {  # tag suffix port cores durability
    TAG=$1; SUF=$2; PORT=$3; CORES=$4; DUR=$5
    echo "== $TAG start $(date -u +%FT%TZ) ==" >> "$RUN/timeline"
    precheck "$TAG"
    if ! start_server "$TAG" "$PORT" "$CORES" "$DUR" >> "$RUN/logs/$TAG.startup" 2>&1; then
        echo "== $TAG SERVER FAILED ==" >> "$RUN/timeline"; return; fi
    python3 "$TOOLS/scenario0_stockmarket.py" --port "$PORT" --suffix "$SUF" $S0_ARGS \
        --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    stop_server "$TAG" >> "$RUN/logs/$TAG.startup" 2>&1
    echo "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) ==" >> "$RUN/timeline"
}

cell2() {
    TAG=$1; SUF=$2; PORT=$3; CORES=$4; DUR=$5
    echo "== $TAG start $(date -u +%FT%TZ) ==" >> "$RUN/timeline"
    precheck "$TAG"
    if ! start_server "$TAG" "$PORT" "$CORES" "$DUR" >> "$RUN/logs/$TAG.startup" 2>&1; then
        echo "== $TAG SERVER FAILED ==" >> "$RUN/timeline"; return; fi
    python3 "$TOOLS/scenario2_freight.py" --port "$PORT" --suffix "$SUF" $S2_ARGS \
        --schema-only > "$RUN/logs/$TAG.schema" 2>&1
    echo "  $TAG schema rc=$?" >> "$RUN/timeline"
    python3 "$TOOLS/scenario2_freight.py" --port "$PORT" --suffix "$SUF" $S2_ARGS \
        --load-only > "$RUN/logs/$TAG.load" 2>&1
    echo "  $TAG load rc=$?" >> "$RUN/timeline"
    python3 "$TOOLS/scenario2_freight.py" --port "$PORT" --suffix "$SUF" $S2_ARGS \
        --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    stop_server "$TAG" >> "$RUN/logs/$TAG.startup" 2>&1
    echo "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) ==" >> "$RUN/timeline"
}

echo "S02 START $(date -u +%FT%TZ)" >> "$RUN/timeline"
cell0 ${P}s0-c1-g s0c1g 15600 1 group
cell2 ${P}s2-c8-s s2c8s 15607 8 strict
cell2 ${P}s2-c1-s s2c1s 15606 1 strict
cell2 ${P}s2-c1-g s2c1g 15604 1 group
cell0 ${P}s0-c8-g s0c8g 15601 8 group
cell0 ${P}s0-c1-s s0c1s 15602 1 strict
cell2 ${P}s2-c8-g s2c8g 15605 8 group
cell0 ${P}s0-c8-s s0c8s 15603 8 strict
echo "S02 END $(date -u +%FT%TZ)" >> "$RUN/timeline"
touch "$RUN/s02${P}.done"
