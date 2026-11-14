#!/bin/bash
# follow-up: mount cells, A=2789a457, B=713a84d0 (control), C=4cf04d4c, interleaved, rotating order.
S=/tmp/claude-1000/-home-louis-ckdbs/27596f25-ceea-4694-a7ec-1a2566519ac2/scratchpad
T=/home/louis/ckdbs/.claude/worktrees/audit-g1-g15-fixes/tools
RUN=/home/louis/bench-runs/audit-g-close/f2; OUT=$S/out2; mkdir -p $RUN $OUT
ST=$OUT/status.txt; : > $ST
say(){ echo "$(date -u +%H:%M:%S) $*" >> $ST; }
( while :; do echo "$(date -u +%s) $(tail -1 $ST)" > $OUT/heartbeat; sleep 15; done ) &
HB=$!; trap 'kill $HB 2>/dev/null' EXIT
mt(){ # label rows rep crashflag name
  python3 -u $T/mount_cost_benchmark.py --binary $S/bin/wrap-$1.sh --label $1 --rows $2 --mounts 7 --durability relaxed $5 \
    --port 15481 --scratch $RUN/$1-$2-$3 --json $OUT/$4-$1-r$3.json --force > $OUT/$4-$1-r$3.txt 2>&1
  rm -rf $RUN/$1-$2-$3; }
orders=("A B C" "C A B" "B C A")
for cell in "0 mount" "4000 mount" "4000 mountcrash"; do
  set -- $cell; rows=$1; name=$2; flag=""; [ $name = mountcrash ] && flag="--crash"
  for rep in 1 2 3; do
    for l in ${orders[$((rep-1))]}; do say "$name $l rows=$rows rep=$rep"; mt $l $rows $rep $name-$rows $flag; done
  done
done
say DONE
