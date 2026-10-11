#!/bin/bash
# Stage bisection of the relaxed carve: S4, S7 part 2 (before S12), S12, B - alternating.
D=/home/cdkbs/bench-runs/carve-diag
for round in 1 2 3; do
  for v in 66bb11b2 91a14db0 7fa2bd96 B; do
    out=$D/out3-$v-$round
    mkdir -p $out $D/work
    python3 $D/driver/ba_census.py --engine kds --bin $D/bin/kds_server-$v --port 15651 \
      --shape insert1 --durability relaxed --cores 1 --sessions 16 --server-cpus 0 \
      --out $out --runs 1 --seconds 8 --warmup 2 --client-cpus 4,5,6,7 \
      --max-load 2 --max-wait 300 --workdir $D/work >> $D/run3.log 2>&1 || echo "FAILED $v $round" >> $D/run3.log
  done
done
