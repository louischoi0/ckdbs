#!/bin/bash
# CE-S0 master: stage A = row-cost sweep + time-to-first-batch on one fresh server (server pinned to CPU 2, client to
# CPU 6, the attribution file's pinning); stage B = scenario 1 c1-g and scenario 3 (10,000 loans, unindexed) x3, unpinned
# as the morning's matrix. Idempotent: a stage/cell with $RUN/done/<tag> is skipped, so a restart resumes.
set -uo pipefail
source /home/cdkbs/bench-runs/ce-s0-v3.3.0-50-g1f57f37b/lib.sh
echo $$ > "$RUN/master.pid"
export PYTHONPATH=$TOOLS
RC=$RUN/rowcost
kill_stale_servers

stageA() {
    [ -e "$RUN/done/rowcost" ] && return
    local PORT=15761 TAG=rowcost PID k=0
    status "$TAG start"; tl "== $TAG start $(date -u +%FT%TZ) =="
    precheck "$TAG"
    start_server "$TAG" "$PORT" 1 group 2 >> "$RUN/logs/$TAG.startup" 2>&1 || { tl "== $TAG SERVER FAILED =="; return; }
    PID=$(cat "$RUN/data/$TAG/pid")
    taskset -c 6 python3 $RC/ce_s0_rows.py load --kds-port $PORT > "$RUN/logs/$TAG.load" 2>&1
    taskset -c 6 python3 $RC/ce_s0_rows.py verify --kds-port $PORT > "$RUN/logs/$TAG.verify" 2>&1 || { tl "== $TAG VERIFY FAILED =="; return; }
    sleep 3
    while :; do
        k=$((k + 1))
        status "$TAG sweep pass $k"
        taskset -c 6 python3 $RC/ce_s0_rows.py run --kds-port $PORT --kds-pid "$PID" --out $RC/cells.jsonl \
            --progress $RUN/status --reps 3 --target-s 3 --modes send,split,drain >> "$RUN/logs/$TAG.run" 2>&1
        local RCODE=$?
        sleep 4
        if [ "$RCODE" -eq 0 ] && python3 $RUN/cellvalid.py --jsonl $RC/cells.jsonl >> "$RUN/logs/$TAG.validity" 2>&1; then break; fi
        tl "== $TAG pass $k ended rc=$RCODE (contaminated or failed), waiting for a quiet host and redoing the rest =="
        [ "$k" -ge 40 ] && { tl "== $TAG GIVING UP after $k passes =="; break; }
        precheck "$TAG-pass$k"
    done
    # time to first byte / first batch on the same server, same pinning
    k=0
    while :; do
        k=$((k + 1))
        status "$TAG ttfb pass $k"
        local T0=$(date +%s)
        taskset -c 6 python3 $RUN/ttfb_probe.py --port $PORT --daily-stats-sizes 1,100,1000,10000,60480 --daily-stats-filter 3 \
            --reps 100 --max-seconds 8 --json $RUN/json/ttfb.json > $RUN/logs/ttfb.txt 2>&1
        local T1=$(date +%s)
        sleep 4
        if python3 $RUN/cellvalid.py $T0 $T1 >> "$RUN/logs/ttfb.validity" 2>&1; then break; fi
        tl "== ttfb pass $k contaminated =="
        [ "$k" -ge 40 ] && break
        precheck "ttfb-pass$k"
    done
    stop_server "$TAG" >> "$RUN/logs/$TAG.startup" 2>&1
    tl "== $TAG end $(date -u +%FT%TZ) =="
    touch "$RUN/done/rowcost"
}

cell1() {  # tag
    local TAG=$1 PORT=15610
    status "$TAG start"; tl "== $TAG start $(date -u +%FT%TZ) =="
    precheck "$TAG"
    if ! start_server "$TAG" "$PORT" 1 group "" >> "$RUN/logs/$TAG.startup" 2>&1; then tl "== $TAG SERVER FAILED =="; return; fi
    date +%s > "$RUN/logs/$TAG.t0"
    python3 "$TOOLS/scenario1_backtest.py" --port "$PORT" --suffix ${TAG//-/_} --seed 1 \
        --bars-clustered btree --verify --sync --analyze --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    date +%s > "$RUN/logs/$TAG.t1"
    stop_server "$TAG" >> "$RUN/logs/$TAG.startup" 2>&1
    tl "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) =="
    touch "$RUN/done/$TAG"
}

cell3() {  # tag
    local TAG=$1 PORT=15620
    status "$TAG start"; tl "== $TAG start $(date -u +%FT%TZ) =="
    precheck "$TAG"
    if ! start_server "$TAG" "$PORT" 1 group "" >> "$RUN/logs/$TAG.startup" 2>&1; then tl "== $TAG SERVER FAILED =="; return; fi
    date +%s > "$RUN/logs/$TAG.t0"
    python3 "$TOOLS/scenario3_library.py" --port "$PORT" --suffix ${TAG//-/_} --loans 10000 \
        --index-mode none --server-indexes on --seed 1 --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    date +%s > "$RUN/logs/$TAG.t1"
    stop_server "$TAG" >> "$RUN/logs/$TAG.startup" 2>&1
    tl "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) =="
    touch "$RUN/done/$TAG"
}

vcell() {  # fn tag : run a cell; if its window held a build/test process or a foreign >= 50 % process, set it aside and redo
    local fn=$1 tag=$2 k=0
    [ -e "$RUN/done/$tag" ] && return
    while :; do
        "$fn" "$tag"
        [ -e "$RUN/done/$tag" ] || return
        sleep 4
        if python3 $RUN/cellvalid.py "$(cat $RUN/logs/$tag.t0)" "$(cat $RUN/logs/$tag.t1)" >> "$RUN/logs/validity.log" 2>&1; then return; fi
        k=$((k + 1))
        tl "== $tag CONTAMINATED attempt $k: $(tail -1 $RUN/logs/validity.log) =="
        mkdir -p "$RUN/contaminated/$tag.attempt$k"
        mv "$RUN"/logs/$tag.* "$RUN/json/$tag.json" "$RUN"/conf/$tag.conf "$RUN/contaminated/$tag.attempt$k/" 2>/dev/null
        rm -f "$RUN/done/$tag"
        [ "$k" -ge 40 ] && { tl "== $tag GIVING UP after $k attempts =="; return; }
    done
}

stageA
for P in "" r2- r3-; do
    vcell cell1 ${P}s1-c1-g
    vcell cell3 ${P}s3-c1-n10000-none
done
status "ALL DONE"
touch "$RUN/all.done"
