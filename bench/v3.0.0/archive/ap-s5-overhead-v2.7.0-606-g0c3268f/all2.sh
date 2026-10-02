#!/bin/bash
cd /home/cdkbs/bench-runs/ap-s5
for r in 6 7 8 9 10 11 12 13 14 15; do
  for n in 200 1000 10000; do
    ./pair.sh point-n$n-run$r ap_overhead_benchmark.py --cell point --rows $n --ops 8000 --walk-ops 1000 --blocks 16
  done
  for n in 1000 10000 60000; do
    ./pair.sh scan-n$n-run$r ap_overhead_benchmark.py --cell scan --rows $n --ops 1000 --blocks 10
  done
  if false; then
    for n in 1000 10000 60000; do
      SOLO=1 ./pair.sh fn-n$n-run$r ap_overhead_benchmark.py --cell fn --rows $n --ops 600 --blocks 12
    done
  fi
done
echo ALLDONE > all2.done
