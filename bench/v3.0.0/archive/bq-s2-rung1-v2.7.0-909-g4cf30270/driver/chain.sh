#!/bin/bash
# BQ-S2's host window: after BP's v2 chain releases the host, build the four
# measurement arms, then run the matrix. Resumable: each step skips what it
# already produced.
set -u
S2=/home/cdkbs/bench-runs/bq/s2
OUT=$S2/out
mkdir -p $OUT
# BP's watchdog says DONE only after its refill pass, unlike all.done.
until grep -q 'chain DONE' /home/cdkbs/bench-runs/bp-s1/v2/out/watchdog.log 2>/dev/null; do
    date +%s > $OUT/gate.beat; sleep 30
done
bash $S2/build_variants.sh >> $OUT/build.console 2>&1 || { echo "build failed" >> $OUT/build.console; exit 1; }
python3 $S2/s2_matrix.py $OUT 3 >> $OUT/matrix.console 2>&1
