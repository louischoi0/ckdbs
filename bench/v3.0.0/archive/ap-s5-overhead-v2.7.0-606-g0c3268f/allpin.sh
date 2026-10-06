#!/bin/bash
cd /home/cdkbs/bench-runs/ap-s5
for r in 1 2 3 4 5 6 7 8 9 10; do
  ./pairpin.sh pin-point-n1000-run$r ap_overhead_benchmark.py --cell point --rows 1000 --ops 8000 --walk-ops 1000 --blocks 16
  ./pairpin.sh pin-point-n10000-run$r ap_overhead_benchmark.py --cell point --rows 10000 --ops 8000 --walk-ops 1000 --blocks 16
  ./pairpin.sh pin-scan-n10000-run$r ap_overhead_benchmark.py --cell scan --rows 10000 --ops 1000 --blocks 10
  ./pairpin.sh pin-scan-n60000-run$r ap_overhead_benchmark.py --cell scan --rows 60000 --ops 1000 --blocks 10
done
echo ALLDONE > allpin.done
