#!/bin/bash
cd /home/cdkbs/bench-runs/ap-s5
./pairpin.sh pin-point-n10000-run2 ap_overhead_benchmark.py --cell point --rows 10000 --ops 8000 --walk-ops 1000 --blocks 16
./pairpin.sh pin-scan-n10000-run2 ap_overhead_benchmark.py --cell scan --rows 10000 --ops 1000 --blocks 10
./pairpin.sh pin-scan-n60000-run3 ap_overhead_benchmark.py --cell scan --rows 60000 --ops 1000 --blocks 10
