#!/bin/bash
S=/tmp/claude-1000/-home-louis-ckdbs/27596f25-ceea-4694-a7ec-1a2566519ac2/scratchpad
WT=/home/louis/ckdbs/.claude/worktrees/audit-g1-g15-fixes
ST=$S/run/status2.txt; : > $ST
say(){ echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) $*" | tee -a $ST; }
( while :; do echo "$(date -u +%s) $(tail -1 $ST)" > $S/run/heartbeat2; sleep 15; done ) &
HB=$!; trap 'kill $HB 2>/dev/null' EXIT
rm -f $S/run/host2.stop; bash $S/host_sampler.sh $S/run/host2.log $S/run/host2.stop & HS=$!
say BEGIN spillmount; bash $S/spill_mount.sh > $S/run/spillmount.console.txt 2>&1; say END spillmount
RUN=/home/louis/bench-runs/audit-g-close/swap; mkdir -p $RUN
say BEGIN m1swap
env WT=$WT SP=$S M1M4=$S/m1m4 BIN_BASE=$S/bin/kds_server-B BIN_CLOSE=$S/bin/kds_server-A RUN_ROOT=$RUN OUT=$S/run/m1swap CELLS=m1 M1_REPS=3 M1_OPS=40000 PORT_BASE=15620 PORT_BASE_DBG=15621 PORT_CLOSE=15622 PORT_CLOSE_DBG=15623 bash $S/m1m4/run_m1m4.sh > $S/run/m1swap.console.txt 2>&1
say END m1swap
touch $S/run/host2.stop; say DONE
