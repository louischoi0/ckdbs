#!/bin/bash
# Measurement watchdog: every 60 s check the master by pid file and the status file's age.
# An orphaned master (pid gone, plan.done absent) or a stalled one (status older than 1200 s)
# is restarted at once; run_plan.sh is idempotent per cell.
RUN=/home/cdkbs/bench-runs/v3.3.0-38-ga40882af
echo $$ > "$RUN/watchdog2.pid"
launch() {
    setsid nohup bash "$RUN/run_plan.sh" >> "$RUN/logs/master.out" 2>&1 < /dev/null &
}
while [ ! -e "$RUN/plan.done" ]; do
    now=$(date +%s)
    mp=$(cat "$RUN/master.pid" 2>/dev/null)
    age=$(( now - $(stat -c %Y "$RUN/status" 2>/dev/null || echo "$now") ))
    echo "$(date -u +%FT%TZ) master=${mp:-none} alive=$([ -n "$mp" ] && kill -0 "$mp" 2>/dev/null && echo y || echo n) status_age=${age}s status=$(cat $RUN/status 2>/dev/null)" >> "$RUN/watchdog2.log"
    if [ -z "$mp" ] || ! kill -0 "$mp" 2>/dev/null; then
        echo "$(date -u +%FT%TZ) ORPHAN: master gone, restarting" >> "$RUN/watchdog2.log"
        launch
    elif [ "$age" -gt 1200 ]; then
        echo "$(date -u +%FT%TZ) STALL: status ${age}s old, killing master group and restarting" >> "$RUN/watchdog2.log"
        kill -TERM -- "-$mp" 2>/dev/null; sleep 3; kill -KILL -- "-$mp" 2>/dev/null
        launch
    fi
    sleep 60
done
echo "$(date -u +%FT%TZ) plan.done seen, watchdog exits" >> "$RUN/watchdog2.log"
