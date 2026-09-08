#!/bin/bash
# AM-S6: the interleaved A/B the operator lifted the suspension for.
#
#   A = f6ed10c, AR0 M0's engine (the archived AL-S8 binary)
#   B = HEAD,    M1 and everything since
#
# Same host, same drivers (BTREE, cd5c60e), same arguments as AL-S8's
# cells. Two passes over the eight cells; pass 1 runs A then B, pass 2
# runs B then A, so neither arm always warms the page cache for the other.
#
# TAG names files; SUF names relations and must be a bare identifier.
set -uo pipefail
RUN=/home/cdkbs/bench-runs/am-s6-ab
TOOLS=/home/cdkbs/ckdbs/tools
S0_ARGS="--users 100 --accounts-per-user 3 --assets 30 --traders 8 --txn-per-user 50 --verify 200 --seed 1 --sync"
S2_ARGS="--organizations 300 --ships 30 --operations 300 --cargos 4000 --bookers 8 --bookings 3000 --verify 100 --seed 1 --sync"

start() {  # tag port cores dur peer arm
    TAG="$1"; PORT="$2"; CORES="$3"; DUR="$4"; PEER="$5"; ARM="$6"
    CONF="$RUN/conf/$TAG.conf"
    cat > "$CONF" <<EOF
data_file = $RUN/data/$TAG.db
port = $PORT
log_dir = $RUN/logs
log_file = $TAG.log
log_level = warn
cores = $CORES
durability = $DUR
placement = namespace
peer_listeners = $PEER
EOF
    cd "$RUN"
    nohup "./kds_server_$ARM" --config "$CONF" > "$RUN/logs/$TAG.server.out" 2>&1 &
    echo $! > "$RUN/pids/$TAG.pid"
    for i in $(seq 1 100); do
        if (echo > /dev/tcp/127.0.0.1/$PORT) 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    return 1
}

stop() {
    TAG="$1"
    PID=$(cat "$RUN/pids/$TAG.pid" 2>/dev/null) || return 0
    if ps -p "$PID" > /dev/null 2>&1; then
        kill "$PID"
        for i in $(seq 1 100); do ps -p "$PID" >/dev/null 2>&1 || break; sleep 0.1; done
        ps -p "$PID" >/dev/null 2>&1 && kill -9 "$PID"
    fi
    rm -f "$RUN/data/$TAG.db"
}

one0() {  # cell arm pass port cores dur peer
    CELL="$1"; ARM="$2"; P="$3"; PORT="$4"; CORES="$5"; DUR="$6"; PEER="$7"
    TAG="$CELL-$ARM-p$P"; SUF=$(echo "$CELL$ARM$P" | tr -d '-')
    bash "$RUN/precheck.sh" > "$RUN/logs/$TAG.precheck" 2>&1
    if ! start "$TAG" "$PORT" "$CORES" "$DUR" "$PEER" "$ARM"; then
        echo "$TAG SERVER FAILED" >> "$RUN/timeline"; return
    fi
    python3 "$TOOLS/scenario0_stockmarket.py" --port "$PORT" --suffix "$SUF" $S0_ARGS \
        --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    RC=$?
    stop "$TAG"
    echo "$TAG rc=$RC tps=$(grep -E '^  TPS ' $RUN/logs/$TAG.stdout | awk '{print $2}')" >> "$RUN/timeline"
}

one2() {  # cell arm pass port cores dur peer
    CELL="$1"; ARM="$2"; P="$3"; PORT="$4"; CORES="$5"; DUR="$6"; PEER="$7"
    TAG="$CELL-$ARM-p$P"; SUF=$(echo "$CELL$ARM$P" | tr -d '-')
    bash "$RUN/precheck.sh" > "$RUN/logs/$TAG.precheck" 2>&1
    if ! start "$TAG" "$PORT" "$CORES" "$DUR" "$PEER" "$ARM"; then
        echo "$TAG SERVER FAILED" >> "$RUN/timeline"; return
    fi
    python3 "$TOOLS/scenario2_freight.py" --port "$PORT" --suffix "$SUF" $S2_ARGS --schema-only \
        > "$RUN/logs/$TAG.schema" 2>&1
    python3 "$TOOLS/scenario2_freight.py" --port "$PORT" --suffix "$SUF" $S2_ARGS --load-only \
        > "$RUN/logs/$TAG.load" 2>&1
    python3 "$TOOLS/scenario2_freight.py" --port "$PORT" --suffix "$SUF" $S2_ARGS \
        --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    RC=$?
    stop "$TAG"
    echo "$TAG rc=$RC tps=$(grep -E '^  committed ' $RUN/logs/$TAG.stdout | awk '{print $3}')" >> "$RUN/timeline"
}

pair0() {  # cell pass first second cores dur peer
    one0 "$1" "$3" "$2" 15620 "$5" "$6" "$7"
    one0 "$1" "$4" "$2" 15630 "$5" "$6" "$7"
}
pair2() {
    one2 "$1" "$3" "$2" 15621 "$5" "$6" "$7"
    one2 "$1" "$4" "$2" 15631 "$5" "$6" "$7"
}

echo "AB2 START $(date -u +%FT%TZ)" >> "$RUN/timeline"
for P in 3 4; do
    if [ $((P % 2)) = 1 ]; then F=A; S=B; else F=B; S=A; fi
    echo "-- pass $P: $F then $S --" >> "$RUN/timeline"
    pair0 s0-c1-g "$P" $F $S 1 group  off
    pair2 s2-c8-s "$P" $F $S 8 strict on
    pair2 s2-c1-s "$P" $F $S 1 strict off
    pair2 s2-c1-g "$P" $F $S 1 group  off
    pair0 s0-c8-g "$P" $F $S 8 group  on
    pair0 s0-c1-s "$P" $F $S 1 strict off
    pair2 s2-c8-g "$P" $F $S 8 group  on
    pair0 s0-c8-s "$P" $F $S 8 strict on
done
echo "AB2 END $(date -u +%FT%TZ)" >> "$RUN/timeline"
touch "$RUN/done2.flag"
