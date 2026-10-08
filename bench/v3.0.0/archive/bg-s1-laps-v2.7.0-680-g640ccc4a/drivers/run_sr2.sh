#!/bin/bash
# BG-S1's laps cell, at 640ccc4a, with bench/README.md's per-cell host record.
R=/home/cdkbs/bench-runs/bg-s1
cd /home/cdkbs/ckdbs/.claude/worktrees/bg-s1-laps
{
  echo "== run2 start $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "binary sha256 $(sha256sum $R/bin/kds_server-L | cut -d' ' -f1)"
  echo "device $(df -T $R | tail -1 | awk '{print $1, $2}')"
  echo "loadavg $(cat /proc/loadavg)"
  echo "competing: $(pgrep -a -f 'cc1plus|cmake --build|ctest|kds_tests' | cut -c1-120 | tr '\n' ';')"
} >> $R/sr2.log
python3 -I $R/driver/scanres_laps.py L 16384 3538944 --out $R/scanres2-16384-L.json >> $R/sr2.log 2>&1
rc=$?
{
  echo "loadavg-end $(cat /proc/loadavg)"
  echo "== run2 end $(date -u +%Y-%m-%dT%H:%M:%SZ) rc=$rc"
  echo done
} >> $R/sr2.log
