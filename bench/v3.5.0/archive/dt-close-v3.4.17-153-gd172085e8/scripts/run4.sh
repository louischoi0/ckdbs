#!/bin/bash
cd /home/ukjin/workspaces/ckdbs/.claude/worktrees/dt-write-path-yield || exit 1
OUT=/home/ukjin/workspaces/ckdbs/.claude/worktrees/dt-write-path-yield/bench/v3.5.0/archive/dt-close-v3.4.17-153-gd172085e8
until [ -e $OUT/finished ]; do sleep 3; done
mkdir -p /home/ukjin/bench-runs/dt/close/w4
echo "$(date -u +%T) cell4 start load $(cat /proc/loadavg) competing: $(pgrep -a -f 'cc1plus|cmake --build|ctest' | grep -v pgrep | wc -l)" >> $OUT/driver-all.log
python3 -I tools/dr_close.py --part ab --bin-a /home/ukjin/bench-runs/dt/close/bin/kds_server.A --bin-b /home/ukjin/bench-runs/dt/close/bin/kds_server.B \
  --workdir /home/ukjin/bench-runs/dt/close/w4 --out $OUT/dr4-ab.json --rounds 15 --port 32781 --heartbeat $OUT/dr4.beat > $OUT/driver-dr4.log 2>&1
echo "$(date -u +%T) cell4 done rc=$? load $(cat /proc/loadavg)" >> $OUT/driver-all.log
touch $OUT/finished4
