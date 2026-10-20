#!/bin/bash
# CE close master: units (pair, binary) in the order A B | B A | A B; each unit = sweep+ttfb (server pinned CPU 2, client CPU 6),
# scenario 1 c1-g, scenario 3 10,000 loans unindexed (unpinned). Idempotent per cell via $RUN/done/<cell>. Then the point-statement census.
set -uo pipefail
source /home/cdkbs/bench-runs/ce-close-v3.3.0-52-g9e568b44/lib.sh
echo $$ > "$RUN/master.pid"
export PYTHONPATH=$TOOLS
kill_stale_servers

binpath() { [ "$1" = A ] && echo "$BIN_A" || echo "$BIN_B"; }

sweep() {  # unit-tag bin
    local U=$1 B=$2 TAG=$1-sweep PORT=15761 PID k=0
    [ -e "$RUN/done/$TAG" ] && return
    if [ "$(free_kb)" -lt 2097152 ]; then status "STOPPED: free space under 2 GB"; tl "== STOPPED low disk =="; exit 1; fi
    status "$TAG start"; tl "== $TAG start $(date -u +%FT%TZ) =="
    precheck "$TAG"
    start_server "$TAG" "$PORT" "$(binpath $B)" 2 >> "$RUN/logs/$TAG.startup" 2>&1 || { tl "== $TAG SERVER FAILED =="; return; }
    PID=$(cat "$RUN/data/$TAG/pid")
    taskset -c 6 python3 $RUN/rowcost/ce_s0_rows.py load --kds-port $PORT > "$RUN/logs/$TAG.load" 2>&1
    taskset -c 6 python3 $RUN/rowcost/ce_s0_rows.py verify --kds-port $PORT > "$RUN/logs/$TAG.verify" 2>&1 || { tl "== $TAG VERIFY FAILED =="; stop_server "$TAG"; return; }
    sleep 2
    while :; do
        k=$((k + 1)); status "$TAG sweep pass $k"
        taskset -c 6 python3 $RUN/rowcost/ce_s0_rows.py run --kds-port $PORT --kds-pid "$PID" --out $RUN/rowcost/cells-$U.jsonl \
            --progress $RUN/status.cell --reps 1 --target-s 2.5 --modes send,split >> "$RUN/logs/$TAG.run" 2>&1
        local RC=$?
        [ "$RC" -eq 0 ] && break
        tl "== $TAG pass $k rc=$RC (contaminated or failed) =="
        [ "$k" -ge 5 ] && { tl "== $TAG GIVING UP after $k passes =="; break; }
        precheck "$TAG-pass$k"
    done
    k=0
    while :; do
        k=$((k + 1)); status "$TAG ttfb pass $k"
        local T0=$(date +%s)
        taskset -c 6 python3 $TOOLS/ttfb_probe.py --port $PORT --daily-stats-sizes 1,100,1000,10000,60480 --daily-stats-filter 3 \
            --reps 100 --max-seconds 5 --json $RUN/json/ttfb-$U.json > $RUN/logs/ttfb-$U.txt 2>&1
        local T1=$(date +%s)
        sleep 3
        if python3 $RUN/cellvalid.py $T0 $T1 >> "$RUN/logs/ttfb-$U.validity" 2>&1; then break; fi
        tl "== $TAG ttfb pass $k contaminated =="
        [ "$k" -ge 4 ] && { tl "== $TAG ttfb GIVING UP =="; break; }
        precheck "$TAG-ttfb$k"
    done
    stop_server "$TAG" >> "$RUN/logs/$TAG.startup" 2>&1
    tl "== $TAG end $(date -u +%FT%TZ) =="
    touch "$RUN/done/$TAG"
}

scen() {  # unit-tag kind(s1|s3) bin
    local U=$1 K=$2 B=$3 TAG=$1-$2 PORT k=0
    [ "$K" = s1 ] && PORT=15610 || PORT=15620
    [ -e "$RUN/done/$TAG" ] && return
    while :; do
        k=$((k + 1))
        if [ "$(free_kb)" -lt 2097152 ]; then status "STOPPED: free space under 2 GB"; tl "== STOPPED low disk =="; exit 1; fi
        status "$TAG start attempt $k"; tl "== $TAG start $(date -u +%FT%TZ) =="
        precheck "$TAG"
        if ! start_server "$TAG" "$PORT" "$(binpath $B)" "" >> "$RUN/logs/$TAG.startup" 2>&1; then tl "== $TAG SERVER FAILED =="; return; fi
        date +%s > "$RUN/logs/$TAG.t0"
        if [ "$K" = s1 ]; then
            python3 "$TOOLS/scenario1_backtest.py" --port "$PORT" --suffix ${U//-/_}_s1 --seed 1 \
                --bars-clustered btree --verify --sync --analyze --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
        else
            python3 "$TOOLS/scenario3_library.py" --port "$PORT" --suffix ${U//-/_}_s3 --loans 10000 \
                --index-mode none --server-indexes on --seed 1 --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
        fi
        echo "$?" > "$RUN/logs/$TAG.rc"
        date +%s > "$RUN/logs/$TAG.t1"
        stop_server "$TAG" >> "$RUN/logs/$TAG.startup" 2>&1
        tl "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) =="
        sleep 3
        if python3 $RUN/cellvalid.py "$(cat $RUN/logs/$TAG.t0)" "$(cat $RUN/logs/$TAG.t1)" >> "$RUN/logs/validity.log" 2>&1; then break; fi
        tl "== $TAG CONTAMINATED attempt $k: $(tail -1 $RUN/logs/validity.log) =="
        mkdir -p "$RUN/contaminated/$TAG.attempt$k"
        mv "$RUN"/logs/$TAG.* "$RUN/json/$TAG.json" "$RUN/contaminated/$TAG.attempt$k/" 2>/dev/null
        [ "$k" -ge 4 ] && { tl "== $TAG GIVING UP after $k attempts =="; return; }
    done
    touch "$RUN/done/$TAG"
}

unit() {  # pair bin
    local U=p$1-$2
    sweep "$U" "$2"
    scen "$U" s1 "$2"
    scen "$U" s3 "$2"
}

unit 1 A; unit 1 B
unit 2 B; unit 2 A
unit 3 A; unit 3 B

if [ ! -e "$RUN/done/census" ]; then
    status "census start"; tl "== census start $(date -u +%FT%TZ) =="
    rm -rf $RUN/wd-census; mkdir -p $RUN/wd-census
    GATE="--cell-quiet-pct 10 --cell-quiet-wait 30 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1"
    python3 $TOOLS/bn_census.py --arm A=$BIN_A --arm B=$BIN_B --workdir $RUN/wd-census --port 34021 \
        --runs 6 --pin-server 2 --pin-driver 4 --rows 1000 --scan-rows 1000 --ops 2000 --blocks 6 \
        --only noop,ping,select-pk,update-lit $GATE --quiet-load 2 --quiet-wait 30 --out $RUN/json/census.json > $RUN/logs/census.log 2>&1
    echo $? > $RUN/logs/census.rc
    rm -rf $RUN/wd-census
    tl "== census end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/census.rc) =="
    touch "$RUN/done/census"
fi
status "ALL DONE"
touch "$RUN/all.done"
