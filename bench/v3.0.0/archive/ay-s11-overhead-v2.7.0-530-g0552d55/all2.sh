#!/bin/bash
cd /home/cdkbs/bench-runs/ay-s11
for r in 1 2 3 4 5; do
  ./pair.sh c2-run$r fk_overhead_benchmark.py --cell price --ops 4000 --blocks 8
  if [ $r -le 3 ]; then ./pair.sh c3-run$r fk_overhead_benchmark.py --cell ledger --ks 1,64,1024,4096,16384 --rows-per-k 32768; fi
done
echo ALLDONE > all2.done
