#!/bin/bash
# Scenarios 1-4 full rebaseline at v3.3.0-38-ga40882af. Arguments, cell sets, repeat counts and
# ports are the 2026-09-30 rebaseline's (bench/v3.0.0/archive/scenario*-v2.7.0-531-g9a0525d/run_*.sh
# at 36449d28). Idempotent: a cell with $RUN/done/<tag> is skipped, so a restart resumes.
set -uo pipefail
source /home/cdkbs/bench-runs/v3.3.0-38-ga40882af/lib.sh
echo $$ > "$RUN/master.pid"
export PYTHONPATH=$TOOLS
kill_stale_servers

S2_ARGS="--organizations 300 --ships 30 --operations 300 --cargos 4000 --bookers 8 --bookings 3000 --verify 100 --seed 1 --sync"

cell2() {  # tag suffix port cores durability [extra driver args]
    local TAG=$1 SUF=$2 PORT=$3 CORES=$4 DUR=$5; shift 5
    [ -e "$RUN/done/$TAG" ] && return
    status "$TAG start"
    tl "== $TAG start $(date -u +%FT%TZ) =="
    precheck "$TAG"
    if ! start_server "$TAG" "$PORT" "$CORES" "$DUR" >> "$RUN/logs/$TAG.startup" 2>&1; then
        tl "== $TAG SERVER FAILED =="; return; fi
    python3 "$TOOLS/scenario2_freight.py" --port "$PORT" --suffix "$SUF" $S2_ARGS "$@" \
        --schema-only > "$RUN/logs/$TAG.schema" 2>&1
    tl "  $TAG schema rc=$?"
    python3 "$TOOLS/scenario2_freight.py" --port "$PORT" --suffix "$SUF" $S2_ARGS "$@" \
        --load-only > "$RUN/logs/$TAG.load" 2>&1
    tl "  $TAG load rc=$?"
    python3 "$TOOLS/scenario2_freight.py" --port "$PORT" --suffix "$SUF" $S2_ARGS "$@" \
        --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    stop_server "$TAG" >> "$RUN/logs/$TAG.startup" 2>&1
    tl "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) =="
    status "$TAG end"
    touch "$RUN/done/$TAG"
}

cell2s() {  # like cell2, with a 1 s whole-host CPU sampler around the cell
    local TAG=$1
    [ -e "$RUN/done/$TAG" ] && return
    rm -f "$RUN/logs/$TAG.cpustop" "$RUN/logs/$TAG.cpu"
    bash "$RUN/cpusample.sh" "$RUN/logs/$TAG.cpu" "$RUN/logs/$TAG.cpustop" &
    local SP=$!
    cell2 "$@"
    touch "$RUN/logs/$TAG.cpustop"
    wait $SP 2>/dev/null
}

cell3() {  # tag cores durability loans mode
    local TAG=$1 CORES=$2 DUR=$3 LOANS=$4 MODE=$5 PORT=15620
    [ -e "$RUN/done/$TAG" ] && return
    status "$TAG start"
    tl "== $TAG start $(date -u +%FT%TZ) =="
    precheck "$TAG"
    if ! start_server "$TAG" "$PORT" "$CORES" "$DUR" >> "$RUN/logs/$TAG.startup" 2>&1; then
        tl "== $TAG SERVER FAILED =="; return; fi
    local EXTRA=""
    [ "$MODE" != none ] && EXTRA="--assert-index-reads"
    python3 "$TOOLS/scenario3_library.py" --port "$PORT" --suffix ${TAG//-/_} --loans "$LOANS" \
        --index-mode "$MODE" --server-indexes on --seed 1 $EXTRA \
        --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    stop_server "$TAG" >> "$RUN/logs/$TAG.startup" 2>&1
    tl "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) =="
    status "$TAG end"
    touch "$RUN/done/$TAG"
}

cell1() {  # tag driver-path cores durability
    local TAG=$1 DRV=$2 CORES=$3 DUR=$4 PORT=15610
    [ -e "$RUN/done/$TAG" ] && return
    status "$TAG start"
    tl "== $TAG start $(date -u +%FT%TZ) =="
    precheck "$TAG"
    if ! start_server "$TAG" "$PORT" "$CORES" "$DUR" >> "$RUN/logs/$TAG.startup" 2>&1; then
        tl "== $TAG SERVER FAILED =="; return; fi
    python3 "$DRV" --port "$PORT" --suffix ${TAG//-/_} --seed 1 \
        --bars-clustered btree --verify --sync --analyze \
        --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    stop_server "$TAG" >> "$RUN/logs/$TAG.startup" 2>&1
    tl "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) =="
    status "$TAG end"
    touch "$RUN/done/$TAG"
}

cell1x() {  # tag driver-path cores durability [extra driver args]
    local TAG=$1 DRV=$2 CORES=$3 DUR=$4 PORT=15610; shift 4
    [ -e "$RUN/done/$TAG" ] && return
    status "$TAG start"
    tl "== $TAG start $(date -u +%FT%TZ) =="
    precheck "$TAG"
    if ! start_server "$TAG" "$PORT" "$CORES" "$DUR" >> "$RUN/logs/$TAG.startup" 2>&1; then
        tl "== $TAG SERVER FAILED =="; return; fi
    python3 "$DRV" --port "$PORT" --suffix ${TAG//-/_} --seed 1 \
        --bars-clustered btree --verify --sync --analyze "$@" \
        --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    stop_server "$TAG" >> "$RUN/logs/$TAG.startup" 2>&1
    tl "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) =="
    status "$TAG end"
    touch "$RUN/done/$TAG"
}

cell4() {  # tag extra-conf-line
    local TAG=$1 CONF3=$2
    [ -e "$RUN/done/$TAG" ] && return
    status "$TAG start"
    tl "== $TAG start $(date -u +%FT%TZ) =="
    for A in off on declared; do precheck "$TAG-$A"; done
    local CONF1="decay_half_life = 5" CONF2="cabin_optimizer_snapshot_interval_ms = 500"
    start_server $TAG-off 15651 1 group "$CONF1" "$CONF2" "$CONF3" >> "$RUN/logs/$TAG.startup" 2>&1
    start_server $TAG-on 15652 1 group "$CONF1" "$CONF2" "$CONF3" >> "$RUN/logs/$TAG.startup" 2>&1
    start_server $TAG-declared 15653 1 group "$CONF1" "$CONF2" "$CONF3" >> "$RUN/logs/$TAG.startup" 2>&1
    local PO PN PD
    PO=$(cat "$RUN/data/$TAG-off/pid"); PN=$(cat "$RUN/data/$TAG-on/pid"); PD=$(cat "$RUN/data/$TAG-declared/pid")
    python3 "$TOOLS/scenario4_cabinopt_days.py" --port-off 15651 --port-on 15652 --port-declared 15653 \
        --pid-off "$PO" --pid-on "$PN" --pid-declared "$PD" --suffix a \
        --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    for A in off on declared; do stop_server $TAG-$A >> "$RUN/logs/$TAG.startup" 2>&1; done
    tl "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) =="
    status "$TAG end"
    touch "$RUN/done/$TAG"
}

# vcell fn tag [precheck-tag] args...: run a cell function; if the monitor saw a build/test process
# (or a foreign process >= 50 % CPU) inside its window, move its outputs to contaminated/ and run it again.
vcell() {
    local fn=$1 tag=$2 ptag=$3; shift 3
    [ -e "$RUN/done/$tag" ] && return
    local k=0
    while :; do
        "$fn" "$tag" "$@"
        [ -e "$RUN/done/$tag" ] || return
        sleep 4   # let the monitor take its sample after the cell
        if python3 "$RUN/analysis/cellvalid.py" "$tag" "$ptag" >> "$RUN/logs/validity.log" 2>&1; then return; fi
        k=$((k + 1))
        tl "== $tag CONTAMINATED attempt $k: $(tail -1 $RUN/logs/validity.log) =="
        mkdir -p "$RUN/contaminated/$tag.attempt$k"
        mv "$RUN"/logs/$tag.* "$RUN"/logs/$tag-*.* "$RUN/json/$tag.json" "$RUN"/conf/$tag.conf "$RUN"/conf/$tag-*.conf "$RUN/contaminated/$tag.attempt$k/" 2>/dev/null
        rm -f "$RUN/done/$tag"
        if [ "$k" -ge 8 ]; then touch "$RUN/flag/$tag.contaminated-final"; tl "== $tag GIVING UP after $k attempts =="; return; fi
    done
}


PGPORT=15434
PGDB=bench

cellpg() {  # tag script-name [args...]  (PostgreSQL 18.6 twin; fresh table names via suffix)
    local TAG=$1 SCRIPT=$2; shift 2
    [ -e "$RUN/done/$TAG" ] && return
    status "$TAG start"
    tl "== $TAG start $(date -u +%FT%TZ) =="
    precheck "$TAG"
    python3 "$TOOLS/$SCRIPT" --port $PGPORT --database $PGDB --suffix ${TAG//-/_} "$@" \
        --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    tl "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) =="
    status "$TAG end"
    touch "$RUN/done/$TAG"
}


# scenario 2 A/B: old client against new client, c1-g and b1-c1-g, A/B/A/B, 3 pairs
OLDT=$RUN/oldtools/tools; NEWT=$TOOLS
for I in 1 2 3; do
  for V in old new; do
    if [ $V = old ]; then TOOLS=$OLDT; else TOOLS=$NEWT; fi
    export PYTHONPATH=$TOOLS
    vcell cell2 ab-$V$I-s2-c1-g ab-$V$I-s2-c1-g abv${V}${I}c1g 15604 1 group
    vcell cell2 ab-$V$I-b1-s2-c1-g ab-$V$I-b1-s2-c1-g abv${V}${I}b1 15606 1 group --bookers 1
  done
done
TOOLS=$NEWT; export PYTHONPATH=$TOOLS
status "POST DONE"
touch "$RUN/post.done"
