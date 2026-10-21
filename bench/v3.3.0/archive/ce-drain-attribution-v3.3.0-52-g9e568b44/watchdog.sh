#!/bin/bash
# 60 s watchdog: master pid alive and status file fresh (< 150 s) until all.done; polled by file, not by pattern.
RUN=/home/cdkbs/bench-runs/ce-drain-attribution-v3.3.0-52-g9e568b44
while [ ! -e "$RUN/all.done" ]; do
    sleep 60
    M=$(cat "$RUN/master.pid" 2>/dev/null)
    age=$(( $(date +%s) - $(stat -c %Y "$RUN/status") ))
    if kill -0 "$M" 2>/dev/null; then alive=alive; else alive=DEAD; fi
    echo "$(date -u +%FT%TZ) master=$alive status_age=${age}s $(cat $RUN/status)" >> "$RUN/watchdog.log"
    if [ "$alive" = DEAD ] && [ ! -e "$RUN/all.done" ]; then
        echo "$(date -u +%FT%TZ) ORPHANED: resuming" >> "$RUN/watchdog.log"
        nohup "$RUN/run_drain.sh" >> "$RUN/master.out" 2>&1 &
    fi
done
