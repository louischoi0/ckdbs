#!/bin/sh
# 60 s resuming watchdog over BQ-S2's chain (CLAUDE.md, 2026-10-12): a dead
# chain without all.done is relaunched (builds and cells skip what exists);
# a cell or probe running past 600 s has its driver and server killed; one
# refill pass relaunches the matrix once for cells without a result.
S2=/home/cdkbs/bench-runs/bq/s2
OUT=$S2/out
LOG=$OUT/watchdog.log
mkdir -p $OUT
refills=0; seen=0; cellstart=0; lastgate=""
say() { echo "$(date -u '+%H:%M:%S') $*" >> "$LOG"; }
launch() {
    nohup bash $S2/chain.sh > /dev/null 2>&1 &
    echo $! > $OUT/chain.pid
    say "chain launched pid $! ($1)"
}
missing() {
    n=0
    for d in $OUT/relaxed/*/*/r* $OUT/group/*/*/r*; do
        [ -d "$d" ] || continue
        ls "$d"/*.json > /dev/null 2>&1 || n=$((n + 1))
    done
    echo $n
}
[ -f $OUT/chain.pid ] && kill -0 "$(cat $OUT/chain.pid)" 2>/dev/null || launch "start"
while true; do
    if [ -f $OUT/all.done ]; then
        m=$(missing)
        if [ "$m" -gt 0 ] && [ "$refills" -lt 1 ]; then
            refills=1; rm -f $OUT/all.done; launch "refill $m cell(s) without a result"
        else
            say "BQ-S2 chain DONE (cells without a result: $m)"; exit 0
        fi
    fi
    if ! kill -0 "$(cat $OUT/chain.pid)" 2>/dev/null && [ ! -f $OUT/all.done ]; then
        if grep -q 'build failed' $OUT/build.console 2>/dev/null; then
            say "BUILD FAILED - not relaunching; see build.console"; exit 1
        fi
        launch "chain was dead without all.done"
    fi
    n=$(grep -c '"after"' $OUT/hoststamps.log 2>/dev/null); n=${n:-0}
    if [ "$n" -gt "$seen" ]; then
        grep '"after"' $OUT/hoststamps.log | tail -n $((n - seen)) | grep -v '"rc": 0' | cut -c1-200 | while read -r l; do say "FAILED $l"; done
        [ $((n / 10)) -gt $((seen / 10)) ] && say "progress: $n cells done"
        seen=$n
    fi
    last=$(tail -n 1 $OUT/hoststamps.log 2>/dev/null)
    case "$last" in
        *gate_waited_s*)
            if [ "$last" != "$lastgate" ]; then lastgate="$last"; cellstart=$(date +%s); fi
            age=$(( $(date +%s) - cellstart ))
            if [ "$age" -gt 600 ]; then
                for p in $(pgrep -f 'tools/bl_mixed.py --bin') $(pgrep -f 'kds_server.blmixed') $(pgrep -f 's2/mount_probe.py'); do kill $p 2>/dev/null; done
                say "HUNG cell ${age}s: killed its driver and server"
                cellstart=$(date +%s)
            fi ;;
    esac
    sleep 60
done
