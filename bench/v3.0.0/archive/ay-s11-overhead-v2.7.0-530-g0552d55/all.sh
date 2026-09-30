#!/bin/bash
cd /home/cdkbs/bench-runs/ay-s11
for r in 1 2 3; do
  ./pair.sh c1-run$r lock_contention_benchmark.py --sessions 8 --rows 256 --ops 2000 --blocks 4
  ./pair.sh c2-run$r fk_overhead_benchmark.py --cell price --ops 4000 --blocks 8
  ./pair.sh c3-run$r fk_overhead_benchmark.py --cell ledger --ks 1,64,1024,4096,16384 --rows-per-k 16384
done
echo ALLDONE > all.done
