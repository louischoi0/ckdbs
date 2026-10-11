#!/bin/bash
H=/tmp/claude-1000/-home-cdkbs-ckdbs/305b0005-d7d2-422c-b0ed-247e8d0b8e88/scratchpad/bw
R=/home/cdkbs/bench-runs/bw/res
for d in c2-n1000 c2-n200 c2-n10000 c3-n1000 c3-n200 c3-n10000 c3c-n1000 c3c-n200 c3c-n10000 c3b-n1000; do
  BW_PRINT=0 taskset -c 0 python3 $H/bw_an23.py $R/$d
  taskset -c 0 python3 $H/bw_agg23.py $R/$d/summary.json | head -1
done
taskset -c 0 python3 $H/gen_tables.py $R /home/cdkbs/bench-runs/bw/tbl
