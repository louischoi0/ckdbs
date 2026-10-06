#!/bin/bash
cd /home/cdkbs/bench-runs/ap-s5
./pair.sh point-n1000-run7 ap_overhead_benchmark.py --cell point --rows 1000 --ops 8000 --walk-ops 1000 --blocks 16
./pair.sh point-n200-run14 ap_overhead_benchmark.py --cell point --rows 200 --ops 8000 --walk-ops 1000 --blocks 16
