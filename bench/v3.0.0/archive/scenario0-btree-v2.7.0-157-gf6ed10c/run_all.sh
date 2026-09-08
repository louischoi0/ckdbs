#!/bin/bash
# AS-Q6's BTREE re-baseline of f6ed10c: AL-S8's eight scenario cells, same
# engine binary, same driver arguments, the drivers changed only by the
# storage word (HEAP -> BTREE) at cd5c60e.
#
# Cell order is AL-S8's own interleave, so the two runs are comparable in
# ordering as well as in arguments.
#
# TAG names files; SUFFIX names relations and must be a bare identifier -
# the driver interpolates it into `CREATE TABLE users_<suffix>`, so a
# hyphen is a parse error at the server. AL-S8's own suffixes were
# unhyphenated for this reason.
set -uo pipefail
RUN=/home/cdkbs/bench-runs/asq6-btree-f6ed10c
TOOLS=/home/cdkbs/ckdbs/tools
S0_ARGS="--users 100 --accounts-per-user 3 --assets 30 --traders 8 --txn-per-user 50 --verify 200 --seed 1 --sync"
S2_ARGS="--organizations 300 --ships 30 --operations 300 --cargos 4000 --bookers 8 --bookings 3000 --verify 100 --seed 1 --sync"

cell0() {  # tag suffix port cores durability peer
    TAG="$1"; SUF="$2"; PORT="$3"; CORES="$4"; DUR="$5"; PEER="$6"
    echo "== $TAG start $(date -u +%FT%TZ) ==" >> "$RUN/timeline"
    bash "$RUN/precheck.sh" > "$RUN/logs/$TAG.precheck" 2>&1
    if ! bash "$RUN/start_server.sh" "$TAG" "$PORT" "$CORES" "$DUR" namespace "$PEER" \
            >> "$RUN/logs/$TAG.startup" 2>&1; then
        echo "== $TAG SERVER FAILED ==" >> "$RUN/timeline"; return
    fi
    python3 "$TOOLS/scenario0_stockmarket.py" --port "$PORT" --suffix "$SUF" $S0_ARGS \
        --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    bash "$RUN/stop_server.sh" "$TAG" >> "$RUN/logs/$TAG.startup" 2>&1
    rm -f "$RUN/data/$TAG.db"
    echo "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) ==" >> "$RUN/timeline"
}

cell2() {  # tag suffix port cores durability peer
    TAG="$1"; SUF="$2"; PORT="$3"; CORES="$4"; DUR="$5"; PEER="$6"
    echo "== $TAG start $(date -u +%FT%TZ) ==" >> "$RUN/timeline"
    bash "$RUN/precheck.sh" > "$RUN/logs/$TAG.precheck" 2>&1
    if ! bash "$RUN/start_server.sh" "$TAG" "$PORT" "$CORES" "$DUR" namespace "$PEER" \
            >> "$RUN/logs/$TAG.startup" 2>&1; then
        echo "== $TAG SERVER FAILED ==" >> "$RUN/timeline"; return
    fi
    python3 "$TOOLS/scenario2_freight.py" --port "$PORT" --suffix "$SUF" $S2_ARGS \
        --schema-only > "$RUN/logs/$TAG.schema" 2>&1
    echo "  $TAG schema rc=$?" >> "$RUN/timeline"
    python3 "$TOOLS/scenario2_freight.py" --port "$PORT" --suffix "$SUF" $S2_ARGS \
        --load-only > "$RUN/logs/$TAG.load" 2>&1
    echo "  $TAG load rc=$?" >> "$RUN/timeline"
    python3 "$TOOLS/scenario2_freight.py" --port "$PORT" --suffix "$SUF" $S2_ARGS \
        --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    bash "$RUN/stop_server.sh" "$TAG" >> "$RUN/logs/$TAG.startup" 2>&1
    rm -f "$RUN/data/$TAG.db"
    echo "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) ==" >> "$RUN/timeline"
}

echo "RUN START $(date -u +%FT%TZ)" >> "$RUN/timeline"
cell0 s0-c1-g s0c1g 15600 1 group  off
cell2 s2-c8-s s2c8s 15607 8 strict on
cell2 s2-c1-s s2c1s 15606 1 strict off
cell2 s2-c1-g s2c1g 15604 1 group  off
cell0 s0-c8-g s0c8g 15601 8 group  on
cell0 s0-c1-s s0c1s 15602 1 strict off
cell2 s2-c8-g s2c8g 15605 8 group  on
cell0 s0-c8-s s0c8s 15603 8 strict on
echo "RUN END $(date -u +%FT%TZ)" >> "$RUN/timeline"
touch "$RUN/done.flag"
