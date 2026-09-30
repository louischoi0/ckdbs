#!/bin/bash
# Full matrix, pass B (monitored). CELLS_ONLY, if set, restricts to those cell tags.
cd /home/cdkbs/bench-runs/pg18-f2f1ee7
bash monitor.sh > /dev/null 2>&1 &
MON=$!
date -u +%FT%TZ >> run.started
bash run_s0.sh > s0.out 2>&1
bash run_s2.sh > s2.out 2>&1
bash run_s34.sh > s34.out 2>&1
if [ -z "${CELLS_ONLY:-}" ]; then bash run_s1.sh > s1.out 2>&1; fi
kill "$(cat monitor.pid)" 2>/dev/null
touch all.done
