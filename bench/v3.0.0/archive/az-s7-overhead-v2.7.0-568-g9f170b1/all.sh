#!/bin/bash
cd /home/cdkbs/bench-runs/az-s7b
for r in 1 2 3 4 5; do
  for n in 200 1000 10000; do
    ./pair.sh c1-n$n-run$r assertion_overhead_benchmark.py --preload $n --ops 8000 --blocks 16
  done
  ./pair.sh c2-run$r fk_overhead_benchmark.py --cell price --ops 4000 --blocks 8
  if [ $r -le 5 ]; then ./pair.sh c3-run$r fk_overhead_benchmark.py --cell ledger --ks 1,64,1024,4096,16384 --rows-per-k 32768; fi
done
echo ALLDONE > all.done
