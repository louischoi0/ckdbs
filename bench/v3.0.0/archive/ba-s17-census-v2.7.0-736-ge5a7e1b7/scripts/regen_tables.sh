#!/bin/bash
# Regenerates tables_s17.md (relaxed curves, second core, latency, PostgreSQL control, host shift, carve and checkpoint).
R=/home/cdkbs/bench-runs/ba-s17
S4=/home/cdkbs/ckdbs/.claude/worktrees/ba-open-marks/bench/v3.0.0/archive/ba-s4-census-v2.7.0-725-gc390a624
cd $R
python3 -I tables_s17.py $R/kds $S4 $R/pgctl > tables_s17.md
echo >> tables_s17.md
python3 -I tables_s17b.py $R/kds $S4/kds >> tables_s17.md
grep -c '^###' tables_s17.md
