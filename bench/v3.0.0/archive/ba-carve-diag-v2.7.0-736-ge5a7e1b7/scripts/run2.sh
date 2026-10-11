#!/bin/bash
# diag (B as built) against oldsync (BA-S13's persist reverted), alternating.
D=/home/cdkbs/bench-runs/carve-diag
rm -f $D/done2
for round in 1 2 3; do
  for v in diag oldsync; do
    out=$D/out2-$v-$round
    mkdir -p $out $D/work
    python3 $D/driver/ba_census.py --engine kds --bin $D/bin/kds_server-$v --port 15651 \
      --shape insert1 --durability relaxed --cores 1 --sessions 16 --server-cpus 0 \
      --out $out --runs 1 --seconds 8 --warmup 2 --client-cpus 4,5,6,7 \
      --max-load 2 --max-wait 300 --workdir $D/work >> $D/run2.log 2>&1 || echo "FAILED $v $round" >> $D/run2.log
    cp $D/last.stderr $out/server.stderr
  done
done
touch $D/done2
