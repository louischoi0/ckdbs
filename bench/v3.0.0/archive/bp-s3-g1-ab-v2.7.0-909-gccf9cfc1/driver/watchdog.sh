#!/bin/sh
# 60 s resuming watchdog over BP's v2 chain (CLAUDE.md, 2026-10-12): a dead
# chain without all.done is relaunched (its cells skip what has a result); a
# cell past 900 s is killed, which ends that cell with rc != 0 and lets the
# chain go on; one refill pass relaunches the chain once it ends, to retry
# the cells that failed. Events go to watchdog.log.
R=/home/cdkbs/bench-runs/bp-s1/v2
OUT=$R/out
LOG=$OUT/watchdog.log
refills=0
say() { echo "$(date -u '+%H:%M:%S') $*" >> "$LOG"; }
launch() {
    nohup bash $R/chain.sh > $OUT/chain.console 2>&1 &
    echo $! > $OUT/chain.pid
    say "chain launched pid $! ($1)"
}
[ -f $OUT/chain.pid ] && kill -0 "$(cat $OUT/chain.pid)" 2>/dev/null || launch "start"
seen=0
while true; do
    if [ -f $OUT/all.done ]; then
        fails=$(grep -c 'rc=[1-9]' $OUT/hoststamps.log)
        if [ "$fails" -gt 0 ] && [ "$refills" -lt 1 ]; then
            refills=1; rm -f $OUT/all.done; launch "refill after $fails failed cell(s)"
        else
            say "BP v2 chain DONE (failed cells over all passes: $fails)"; exit 0
        fi
    fi
    kill -0 "$(cat $OUT/chain.pid)" 2>/dev/null || { [ -f $OUT/all.done ] || launch "chain was dead without all.done"; }
    n=$(grep -c ' after ' $OUT/hoststamps.log 2>/dev/null)
    if [ "${n:-0}" -gt "$seen" ]; then
        grep ' after ' $OUT/hoststamps.log | tail -n $((n - seen)) | grep 'rc=[1-9]' | while read -r l; do say "FAILED $l"; done
        [ $((n / 5)) -gt $((seen / 5)) ] && say "progress: $n cells done; last: $(grep ' after ' $OUT/hoststamps.log | tail -1 | cut -c21-)"
        seen=$n
    fi
    if [ -f $OUT/cell.start ]; then
        age=$(( $(date +%s) - $(stat -c %Y $OUT/cell.start) ))
        if [ $age -gt 900 ]; then
            # The cell's whole subtree (driver, its server, its clients),
            # deepest first, so no server outlives its driver on the port.
            kids=$(pgrep -P "$(cat $OUT/chain.pid)")
            for c in $kids; do
                for g in $(pgrep -P $c); do
                    for h in $(pgrep -P $g); do kill $h 2>/dev/null; done
                    kill $g 2>/dev/null
                done
                kill $c 2>/dev/null
            done
            say "HUNG cell ${age}s: killed the chain's child"
            touch $OUT/cell.start
        fi
    fi
    sleep 60
done
