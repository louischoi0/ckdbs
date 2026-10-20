#!/bin/bash
# Join-order probe: scenario 3 (n=10,000) and scenario 1 with join_order planned (default) and written,
# with the full ANALYZE replies dumped. Run after run_all.sh finished; same quiet gate and validity loop.
set -uo pipefail
source /home/cdkbs/bench-runs/v3.3.0-33-g9682b725/lib.sh
echo $$ > "$RUN/master.pid"
export PYTHONPATH=$TOOLS
mkdir -p "$RUN/plans"
PLANRUN=/home/cdkbs/bench-run/planrun.py

cellpl3() {  # tag order mode
    local TAG=$1 ORD=$2 MODE=$3 PORT=15620
    [ -e "$RUN/done/$TAG" ] && return
    status "$TAG start"; tl "== $TAG start $(date -u +%FT%TZ) =="
    precheck "$TAG"
    local EXTRA="" CONFL=""
    [ "$MODE" != none ] && EXTRA="--assert-index-reads"
    [ "$ORD" = written ] && CONFL="join_order = written"
    start_server "$TAG" "$PORT" 1 group "$CONFL" >> "$RUN/logs/$TAG.startup" 2>&1 || { tl "== $TAG SERVER FAILED =="; return; }
    python3 "$PLANRUN" s3 "$RUN/plans/$TAG.json" --port "$PORT" --suffix ${TAG//-/_} --loans 10000 \
        --index-mode "$MODE" --server-indexes on --seed 1 $EXTRA --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    stop_server "$TAG" >> "$RUN/logs/$TAG.startup" 2>&1
    tl "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) =="; status "$TAG end"; touch "$RUN/done/$TAG"
}

cellpl1() {  # tag order
    local TAG=$1 ORD=$2 PORT=15610
    [ -e "$RUN/done/$TAG" ] && return
    status "$TAG start"; tl "== $TAG start $(date -u +%FT%TZ) =="
    precheck "$TAG"
    local CONFL=""
    [ "$ORD" = written ] && CONFL="join_order = written"
    start_server "$TAG" "$PORT" 1 group "$CONFL" >> "$RUN/logs/$TAG.startup" 2>&1 || { tl "== $TAG SERVER FAILED =="; return; }
    python3 "$PLANRUN" s1 "$RUN/plans/$TAG.json" --port "$PORT" --suffix ${TAG//-/_} --seed 1 \
        --bars-clustered btree --verify --sync --analyze --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    stop_server "$TAG" >> "$RUN/logs/$TAG.startup" 2>&1
    tl "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) =="; status "$TAG end"; touch "$RUN/done/$TAG"
}

vcell() {
    local fn=$1 tag=$2; shift 2
    [ -e "$RUN/done/$tag" ] && return
    local k=0
    while :; do
        "$fn" "$tag" "$@"
        [ -e "$RUN/done/$tag" ] || return
        sleep 4
        if python3 "$RUN/analysis/cellvalid.py" "$tag" "$tag" >> "$RUN/logs/validity.log" 2>&1; then return; fi
        k=$((k + 1))
        tl "== $tag CONTAMINATED attempt $k: $(tail -1 $RUN/logs/validity.log) =="
        mkdir -p "$RUN/contaminated/$tag.attempt$k"
        mv "$RUN"/logs/$tag.* "$RUN/json/$tag.json" "$RUN"/conf/$tag.conf "$RUN/contaminated/$tag.attempt$k/" 2>/dev/null
        rm -f "$RUN/done/$tag"
        [ "$k" -ge 8 ] && return
    done
}

for R in 1 2 3; do
  for ORD in planned written; do
    vcell cellpl3 pl-s3-$ORD-none-r$R $ORD none
    vcell cellpl1 pl-s1-$ORD-r$R $ORD
  done
done
for ORD in planned written; do vcell cellpl3 pl-s3-$ORD-all-r1 $ORD all; done
status "PLAN DONE"
touch "$RUN/plan.done"
