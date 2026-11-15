#!/bin/bash
# follow-on: mount (A2/B2, 3 reps x 3 cells) and the assertion cell (assertion_overhead_benchmark, A2 vs B2).
S=/tmp/claude-1000/-home-louis-ckdbs/27596f25-ceea-4694-a7ec-1a2566519ac2/scratchpad
T=/home/louis/ckdbs/.claude/worktrees/audit-g1-g15-fixes/tools
RUN=/home/louis/bench-runs/audit-g16-close/f5; OUT=$S/out5; mkdir -p $RUN $OUT
ST=$OUT/status.txt; : > $ST
say(){ echo "$(date -u +%H:%M:%S) $*" >> $ST; }
( while :; do echo "$(date -u +%s) $(tail -1 $ST)" > $OUT/heartbeat; sleep 15; done ) &
HB=$!; trap 'kill $HB 2>/dev/null' EXIT
mt(){ python3 -u $T/mount_cost_benchmark.py --binary $S/bin/wrap-$1.sh --label $1 --rows $2 --mounts 7 --durability relaxed $5 \
    --port 15481 --scratch $RUN/$1-$2-$3 --json $OUT/$4-$1-r$3.json --force > $OUT/$4-$1-r$3.txt 2>&1
  rm -rf $RUN/$1-$2-$3; }
orders=("A2 B2" "B2 A2" "A2 B2")
for cell in "0 mount" "4000 mount" "4000 mountcrash"; do
  set -- $cell; rows=$1; name=$2; flag=""; [ $name = mountcrash ] && flag="--crash"
  for rep in 1 2 3; do
    for l in ${orders[$((rep-1))]}; do say "$name $l rows=$rows rep=$rep"; mt $l $rows $rep $name-$rows $flag; done
  done
done
say "mount done"
SRV=$S/m1m4/srv.py
for pre in 200 1000 10000; do
  for l in A2 B2; do
    python3 $SRV start --binary $S/bin/kds_server-$l --workdir $RUN/as-$pre-$l --port $([ $l = A2 ] && echo 15701 || echo 15702) --debug-text-port 0 --cores 4 --taskset 2-5 --durability relaxed --buffer-pool-frames 65536 --ash on --label as-$pre-$l > $OUT/as-$pre-$l.start.txt 2>&1
  done
  say "assertion preload=$pre"
  taskset -c 6-7 python3 -u $T/assertion_overhead_benchmark.py --port 15702 --label B2 --ab-port 15701 --ab-label A2 --preload $pre --ops 8000 --blocks 16 --json $OUT/assert-$pre.json > $OUT/assert-$pre.txt 2>&1
  for l in A2 B2; do python3 $SRV stop --workdir $RUN/as-$pre-$l > $OUT/as-$pre-$l.stop.txt 2>&1; done
done
say DONE
