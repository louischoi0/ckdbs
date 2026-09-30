#!/bin/bash
# Scenario 2 PostgreSQL 18.6 floor at f2f1ee7. The PG twin drives ONE booking
# connection (it has no --bookers); the KDS cells of 9a0525d used --bookers 8.
# To make a like-for-like comparison this script also runs KDS with
# --bookers 1 (cores 1, group and strict), from a hashed copy of the 9a0525d
# release binary (src/include/CMakeLists.txt identical at f2f1ee7), one cell
# after each PG cell, never concurrently.
set -uo pipefail
source /home/cdkbs/bench-runs/pg18-f2f1ee7/lib.sh
KBIN=$RUN/kds_server
S2_ARGS="--organizations 300 --ships 30 --operations 300 --cargos 4000 --bookings 3000 --verify 100 --seed 1"

pgcell() {  # tag mode
    TAG=$1; SC=$2
    want "$TAG" || return
    echo "== $TAG start $(date -u +%FT%TZ) ==" >> "$RUN/timeline"
    precheck "$TAG"
    fresh_pg "$TAG" || { echo "== $TAG PG INIT FAILED ==" >> "$RUN/timeline"; return; }
    EXTRA=""
    [ "$SC" != default ] && EXTRA="--synchronous-commit $SC"
    python3 "$RUN/pg_s2_shim.py" --port "$PGPORT" --user "$(id -un)" $S2_ARGS $EXTRA \
        --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    stop_pg "$TAG"
    echo "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) ==" >> "$RUN/timeline"
}

kdscell() {  # tag port cores durability
    TAG=$1; PORT=$2; CORES=$3; DUR=$4
    want "$TAG" || return
    DIR=$RUN/data/$TAG
    echo "== $TAG start $(date -u +%FT%TZ) ==" >> "$RUN/timeline"
    precheck "$TAG"
    rm -rf "$DIR"; mkdir -p "$DIR"
    { echo "data_file = $DIR/kds.db"; echo "cores = $CORES"; echo "port = $PORT"
      echo "durability = $DUR"; echo "log_dir = $DIR"; echo "log_file = kds.log"; echo "log_level = warn"; } > "$RUN/conf/$TAG.conf"
    nohup "$KBIN" --config "$RUN/conf/$TAG.conf" > "$DIR/stdout.log" 2>&1 &
    echo $! > "$DIR/pid"
    for i in $(seq 1 150); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break; sleep 0.2; done
    K="python3 $TOOLS/scenario2_freight.py --port $PORT --suffix ${TAG//-/_} $S2_ARGS --bookers 1 --sync"
    $K --schema-only > "$RUN/logs/$TAG.schema" 2>&1
    $K --load-only > "$RUN/logs/$TAG.load" 2>&1
    $K --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    PID=$(cat "$DIR/pid"); kill -TERM "$PID" 2>/dev/null
    for i in $(seq 1 50); do kill -0 "$PID" 2>/dev/null || break; sleep 0.2; done
    kill -0 "$PID" 2>/dev/null && kill -KILL "$PID" 2>/dev/null
    cp "$DIR/kds.log" "$RUN/logs/$TAG.serverlog" 2>/dev/null
    rm -rf "$DIR"
    echo "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) ==" >> "$RUN/timeline"
}

echo "S2 START $(date -u +%FT%TZ)" >> "$RUN/timeline"
for R in 1 2 3; do
    pgcell s2-pg-on-r$R default
    kdscell s2-kds-b1-c1-g-r$R 15710 1 group
    pgcell s2-pg-off-r$R off
    kdscell s2-kds-b1-c1-s-r$R 15711 1 strict
done
echo "S2 END $(date -u +%FT%TZ)" >> "$RUN/timeline"
touch "$RUN/s2.done"
