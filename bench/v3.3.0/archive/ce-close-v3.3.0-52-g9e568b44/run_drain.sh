#!/bin/bash
# Supplement: the `drain` client path (frame headers only, no cell touched) over the six shapes, A B | B A | A B, one run each.
set -uo pipefail
source /home/cdkbs/bench-runs/ce-close-v3.3.0-52-g9e568b44/lib.sh
echo $$ > "$RUN/master-drain.pid"
export PYTHONPATH=$TOOLS
kill_stale_servers
binpath() { [ "$1" = A ] && echo "$BIN_A" || echo "$BIN_B"; }
drain() {  # pair bin
    local U=p$1-$2 B=$2 TAG=p$1-$2-drain PORT=15761 PID k=0
    [ -e "$RUN/done/$TAG" ] && return
    if [ "$(free_kb)" -lt 2097152 ]; then status "STOPPED: free space under 2 GB"; exit 1; fi
    status "$TAG start"; tl "== $TAG start $(date -u +%FT%TZ) =="
    precheck "$TAG"
    start_server "$TAG" "$PORT" "$(binpath $B)" 2 >> "$RUN/logs/$TAG.startup" 2>&1 || { tl "== $TAG SERVER FAILED =="; return; }
    PID=$(cat "$RUN/data/$TAG/pid")
    taskset -c 6 python3 $RUN/rowcost/ce_s0_rows.py load --kds-port $PORT > "$RUN/logs/$TAG.load" 2>&1
    taskset -c 6 python3 $RUN/rowcost/ce_s0_rows.py verify --kds-port $PORT > "$RUN/logs/$TAG.verify" 2>&1 || { tl "== $TAG VERIFY FAILED =="; stop_server "$TAG"; return; }
    sleep 2
    while :; do
        k=$((k + 1)); status "$TAG pass $k"
        taskset -c 6 python3 $RUN/rowcost/ce_s0_rows.py run --kds-port $PORT --kds-pid "$PID" --out $RUN/rowcost/drain-$U.jsonl \
            --progress $RUN/status.cell --reps 1 --target-s 2.5 --modes drain >> "$RUN/logs/$TAG.run" 2>&1
        [ "$?" -eq 0 ] && break
        tl "== $TAG pass $k contaminated/failed =="
        [ "$k" -ge 5 ] && break
        precheck "$TAG-pass$k"
    done
    stop_server "$TAG" >> "$RUN/logs/$TAG.startup" 2>&1
    tl "== $TAG end $(date -u +%FT%TZ) =="
    touch "$RUN/done/$TAG"
}
drain 1 A; drain 1 B; drain 2 B; drain 2 A; drain 3 A; drain 3 B
status "DRAIN DONE"
touch "$RUN/drain.done"
touch "$RUN/all.done"
