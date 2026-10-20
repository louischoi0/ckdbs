#!/bin/bash
# Drain-path attribution: A (base 1f57f37b engine), B (CE 9e568b44), C (B without the per-row test-seam block).
# Four shapes, 5 rounds, order rotated per round, fresh server and data file per visit.
set -uo pipefail
source /home/cdkbs/bench-runs/ce-drain-attribution-v3.3.0-52-g9e568b44/lib.sh
echo $$ > "$RUN/master.pid"
export PYTHONPATH=$TOOLS
export SHAPES_ONLY=range-1,range-100,range-1000,range-10000
ROUNDS="ABC BCA CAB CBA BAC"
visit() {  # round bin
    local TAG=r$1-$2 B=$2 PORT=15761 PID k=0
    [ -e "$RUN/done/$TAG" ] && return
    if [ "$(free_kb)" -lt 2097152 ]; then status "STOPPED: free space under 2 GB"; exit 1; fi
    status "$TAG start"; tl "== $TAG start $(date -u +%FT%TZ) =="
    quiet_gate "$TAG"
    { echo "time: $(date -u +%FT%TZ)"; echo "loadavg: $(cat /proc/loadavg)"; echo "gate wait s: $(cat $RUN/logs/$TAG.gatewait)"; echo "free KB: $(free_kb)"; } > "$RUN/logs/$TAG.precheck"
    start_server "$TAG" "$PORT" "$RUN/bin/kds_server-$B" 2 >> "$RUN/logs/$TAG.startup" 2>&1 || { tl "== $TAG SERVER FAILED =="; return; }
    PID=$(cat "$RUN/data/$TAG/pid")
    taskset -c 6 python3 $RUN/rowcost/ce_s0_rows.py load --kds-port $PORT > "$RUN/logs/$TAG.load" 2>&1
    taskset -c 6 python3 $RUN/rowcost/ce_s0_rows.py verify --kds-port $PORT > "$RUN/logs/$TAG.verify" 2>&1 || { tl "== $TAG VERIFY FAILED =="; stop_server "$TAG"; return; }
    sleep 2
    while :; do
        k=$((k + 1)); status "$TAG pass $k"
        taskset -c 6 python3 $RUN/rowcost/ce_s0_rows.py run --kds-port $PORT --kds-pid "$PID" --out $RUN/json/drain-$TAG.jsonl \
            --progress $RUN/status.cell --reps 1 --target-s 2.5 --modes drain >> "$RUN/logs/$TAG.run" 2>&1
        [ "$?" -eq 0 ] && break
        tl "== $TAG pass $k contaminated/failed =="
        [ "$k" -ge 3 ] && break
        quiet_gate "$TAG-pass$k"
    done
    stop_server "$TAG" >> "$RUN/logs/$TAG.startup" 2>&1
    tl "== $TAG end $(date -u +%FT%TZ) =="
    touch "$RUN/done/$TAG"
}
r=0
for order in $ROUNDS; do
    r=$((r + 1))
    for b in $(echo $order | sed 's/./& /g'); do visit $r $b; done
done
status "DONE"; touch "$RUN/all.done"
