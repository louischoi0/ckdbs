#!/bin/bash
# poll.sh FLAGFILE SECONDS : wait up to SECONDS for FLAGFILE, then print the run's state
S=/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-bm-scan-kernel-open/382ae513-4d71-443e-8517-51d482efafba/scratchpad/s6r
R=/home/cdkbs/bench-runs/bm-s6r
n=0
while [ ! -f "$1" ] && [ $n -lt "$2" ]; do sleep 2; n=$((n+2)); done
date -u +%T
cat /proc/loadavg
tail -3 $S/progress
echo "bn1000 records: $(grep -c '"shape"' $R/out/bn1000.log)"
echo "bmscan lines: $(wc -l < $R/out/bmscan-bn.log 2>/dev/null)"
pgrep -a -f "bn_census.py|bm_scan.py" | cut -c1-80
ls $S/*.done 2>/dev/null
