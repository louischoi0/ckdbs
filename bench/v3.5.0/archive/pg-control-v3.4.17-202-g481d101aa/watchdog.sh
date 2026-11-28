#!/bin/bash
# 60 s watchdog: an orphaned master (pid gone, all.done absent) is relaunched at once; a stalled
# one (status older than 1200 s) is killed and relaunched. run_all.sh is idempotent per cell.
RUN=/home/ukjin/bench-runs/pg-control-v3.4.17-202-g481d101aa
echo $$ > "$RUN/watchdog.pid"
launch() { setsid nohup bash "$RUN/run_all.sh" >> "$RUN/logs/master.out" 2>&1 < /dev/null & }
mkdir -p "$RUN/logs"
while [ ! -e "$RUN/all.done" ]; do
    [ -e "$RUN/flag/pg-start-failed" ] && { echo "$(date -u +%FT%TZ) PG start failed; watchdog exits" >> "$RUN/watchdog.log"; exit 1; }
    now=$(date +%s); mp=$(cat "$RUN/master.pid" 2>/dev/null)
    age=$(( now - $(stat -c %Y "$RUN/status" 2>/dev/null || echo "$now") ))
    echo "$(date -u +%FT%TZ) master=${mp:-none} alive=$([ -n "$mp" ] && kill -0 "$mp" 2>/dev/null && echo y || echo n) status_age=${age}s status=$(cat $RUN/status 2>/dev/null)" >> "$RUN/watchdog.log"
    if [ -z "$mp" ] || ! kill -0 "$mp" 2>/dev/null; then
        echo "$(date -u +%FT%TZ) ORPHAN: master gone, launching" >> "$RUN/watchdog.log"; launch
    elif [ "$age" -gt 1200 ] && ! grep -q "waiting for a quiet host" "$RUN/status"; then
        echo "$(date -u +%FT%TZ) STALL: status ${age}s old, killing master group and relaunching" >> "$RUN/watchdog.log"
        kill -TERM -- "-$mp" 2>/dev/null; sleep 3; kill -KILL -- "-$mp" 2>/dev/null; launch
    fi
    sleep 60
done
echo "$(date -u +%FT%TZ) all.done seen, watchdog exits" >> "$RUN/watchdog.log"
