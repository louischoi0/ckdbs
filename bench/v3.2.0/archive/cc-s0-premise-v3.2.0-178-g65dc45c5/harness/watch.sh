#!/bin/bash
# watch.sh PROGRESS_FILE DONE_FILE LABEL : every 60 s record the progress file's age; flag ORPHAN when it is stale > 180 s
S=/tmp/claude-1000/-home-cdkbs-ckdbs/00ec4c4c-8445-418e-92d3-c235d2b282e7/scratchpad/cc-s0
P=$1; D=$2; L=$3
while [ ! -e "$D" ]; do
    now=$(date +%s); m=$(stat -c %Y "$P" 2>/dev/null || echo 0); age=$((now - m))
    echo "$(date -u +%FT%TZ) $L age=${age}s load=$(cut -d' ' -f1 /proc/loadavg)" >> $S/watch.$L.log
    if [ $age -gt 180 ]; then echo "$(date -u +%FT%TZ) ORPHAN age=$age" >> $S/watch.$L.flag; fi
    sleep 60
done
echo "$(date -u +%FT%TZ) finished" >> $S/watch.$L.log
