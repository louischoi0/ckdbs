#!/bin/bash
# usage: run_all.sh <lifetimes> <rounds>
# Copies of both binaries live in /home/louis/ck-tmp/dc-close/run/ (A.bin, B.bin).  Each lifetime starts
# fresh servers A, B and A2 (a second server of A's binary: the noise-floor control), interleaved.
# Before each lifetime waits until no cc1plus/cmake --build/ctest is running (the wait is recorded).
R=/home/louis/ck-tmp/dc-close/run
D=/home/louis/ckdbs/.claude/worktrees/dc-select-for-update/bench/v3.4.13/archive/dc-close-v3.4.13-14-gcd80d31d
cd $R
quiet() {
  held=0
  while pgrep -f "cc1plus|cmake --build|ctest" > /dev/null; do
    echo "$(date -u +%FT%TZ) held: $(cat /proc/loadavg)" >> $R/held.log; held=1; sleep 15
  done
}
for i in $(seq 1 $1); do
  quiet
  echo "$(date -u +%FT%TZ) life $i start load $(cat /proc/loadavg)" >> $R/lives.log
  taskset -c 4 python3 -I $D/point_arms.py --arm A=$R/A.bin --arm B=$R/B.bin --arm A2=$R/A2.bin \
    --wd $R/wd-life$i --json $D/point-life$i.json --status $R/status.txt --life $i --rounds $2 --ops-upd 3000 \
    --base-port $((27700 + i * 10)) > $D/point-life$i.log 2>&1
  rm -rf $R/wd-life$i/*.db $R/wd-life$i/*.wal
  echo "$(date -u +%FT%TZ) life $i end" >> $R/lives.log
done
echo "$(date -u +%FT%TZ) ALLDONE" > $R/status.txt
