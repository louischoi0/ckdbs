#!/bin/bash
cd /home/cdkbs/bench-runs/pg18-f2f1ee7
bash monitor.sh > /dev/null 2>&1 &
date -u +%FT%TZ >> run.started
bash run_s1.sh > s1b.out 2>&1
kill "$(cat monitor.pid)" 2>/dev/null
touch all.done
