#!/bin/bash
# spill (shape 2) and mount (shape 3), interleaved A/B. status file polled by the watchdog.
S=/tmp/claude-1000/-home-louis-ckdbs/27596f25-ceea-4694-a7ec-1a2566519ac2/scratchpad
T=/home/louis/ckdbs/.claude/worktrees/audit-g1-g15-fixes/tools
RUN=/home/louis/bench-runs/audit-g-close; OUT=$S/out; mkdir -p $RUN $OUT
ST=$OUT/status.txt; : > $ST
say(){ echo "$(date -u +%H:%M:%S) $*" | tee -a $ST; }
sp(){ # label bin rows rep
  python3 -u $T/varheap_spill_benchmark.py --binary $2 --label $1 --rows $3 --durability strict \
    --port 15461 --scratch $RUN/sp-$1-$3-$4 --json $OUT/spill-$1-$3-r$4.json --force > $OUT/spill-$1-$3-r$4.txt 2>&1
  rm -rf $RUN/sp-$1-$3-$4; }
mt(){ # label bin rows rep
  python3 -u $T/mount_cost_benchmark.py --binary $2 --label $1 --rows $3 --mounts 7 --durability relaxed \
    --port 15481 --scratch $RUN/mt-$1-$3-$4 --json $OUT/mount-$1-$3-r$4.json --force > $OUT/mount-$1-$3-r$4.txt 2>&1
  rm -rf $RUN/mt-$1-$3-$4; }
for rows in 200 1000 10000; do
 for rep in 1 2; do
  if [ $rep = 1 ]; then ord="A B"; else ord="B A"; fi
  for l in $ord; do say "spill $l rows=$rows rep=$rep"; sp $l $S/bin/wrap-$l.sh $rows $rep; done
 done
done
for rows in 0 4000; do
 for rep in 1 2 3; do
  if [ $((rep%2)) = 1 ]; then ord="A B"; else ord="B A"; fi
  for l in $ord; do say "mount $l rows=$rows rep=$rep"; mt $l $S/bin/wrap-$l.sh $rows $rep; done
 done
done
say DONE
