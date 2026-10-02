#!/bin/bash
cd /home/cdkbs/bench-runs/ap-s5
for r in 1 2 3; do
  for n in 1000 10000 60000; do
    SOLO=1 ./pair.sh fnix-n$n-run$r ap_overhead_benchmark.py --cell fn --rows $n --ops 600 --blocks 12 --index
  done
done
