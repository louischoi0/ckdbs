#!/bin/bash
G=/tmp/claude-1000/-home-louis-ckdbs/27596f25-ceea-4694-a7ec-1a2566519ac2/scratchpad/g21
until grep -q "DONE" $G/status6.txt; do sleep 2; done
sleep 5
echo "$(date -u +%T) f4 BEGIN" > $G/f4out/status.txt
rm -rf /home/louis/bench-runs/audit-g21-close/f4w; mkdir -p /home/louis/bench-runs/audit-g21-close/f4w
KDS_TOOLS=/home/louis/ckdbs/.claude/worktrees/audit-g1-g15-fixes/tools python3 -u /home/louis/ckdbs/.claude/worktrees/audit-g1-g15-fixes/bench/v3.4.10/archive/f4-overhead-v3.4.10-1-g4f5b2e17/scripts/f4_ab.py --arm A=$G/bin/kds_server-A --arm B=$G/bin/kds_server-B --arm A2=$G/bin/kds_server-A2 --workdir /home/louis/bench-runs/audit-g21-close/f4w --port 31970 --runs 10 --out $G/f4out/full.json --progress $G/f4out/progress > $G/f4out/full.log 2>&1
echo "$(date -u +%T) f4 END rc=$?" >> $G/f4out/status.txt
