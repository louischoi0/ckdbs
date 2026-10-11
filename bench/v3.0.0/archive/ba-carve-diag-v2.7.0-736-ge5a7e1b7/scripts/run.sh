#!/bin/bash
D=/home/cdkbs/bench-runs/carve-diag
W=/home/cdkbs/ckdbs/.claude/worktrees/relaxed-carve-regression
rm -f $D/done
for round in 1 2 3; do
  for v in A B noprep; do
    mkdir -p $D/out-$v-$round $D/work
    python3 "$W/tools/ba_census.py" --engine kds --bin $D/bin/kds_server-$v --port 15651 \
      --shape insert1 --durability relaxed --cores 1 --sessions 16 --server-cpus 0 \
      --out $D/out-$v-$round --runs 1 --seconds 8 --warmup 2 --client-cpus 4,5,6,7 \
      --max-load 2 --max-wait 300 --workdir $D/work >> $D/run.log 2>&1 || echo "FAILED $v $round" >> $D/run.log
  done
done
touch $D/done
