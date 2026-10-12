#!/bin/bash
# Stall census: the M1b workload with no backup, A then B then B then A (cores = 1, server on 0,2,4,6, reactor on 4): the same
# 540,000-row ballast, one 40 s window. Is the share of the window inside statements over 1 ms (checkpoint / device stalls) A's too?
R=/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05
P=/tmp/claude-1000/-home-cdkbs-ckdbs/f6bce74e-e48d-4d48-817a-4e239db00c8e/scratchpad/s10-progress.txt
cd $R
mkdir -p $R/out/stall
rm -f $R/stall.done
python3 -I $R/drivers/sampler.py $R/logs/host-samples-stall.jsonl &
SAMPLER=$!
for spec in A1 B1 B2 A2; do
  bin=${spec:0:1}
  echo "$(date -u +%H:%M) stall census $spec start load $(cut -d' ' -f1-3 /proc/loadavg)" >> $P
  taskset -c 7 python3 $R/drivers/br_cost.py backup --cores-one 1 --binary $bin --tag stall-$spec --ballast-rows 540000 --reps 0 --window 40 --port 31821 --out $R/out/stall --progress $P > $R/logs/stall-$spec.log 2>&1
  rm -rf $R/work/m1b-c1-stall-$spec
done
echo "$(date -u +%H:%M) stall census done" >> $P
kill $SAMPLER
touch $R/stall.done
