#!/bin/bash
# G1-G15 close: interleaved A/B. A=2789a457, B=713a84d0.
S=/tmp/claude-1000/-home-louis-ckdbs/27596f25-ceea-4694-a7ec-1a2566519ac2/scratchpad
WT=/home/louis/ckdbs/.claude/worktrees/audit-g1-g15-fixes
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
RUN=/home/louis/bench-runs/audit-g-close/$STAMP
OUT=$S/run/$STAMP
mkdir -p $RUN $OUT
echo $STAMP > $S/run/current
ST=$S/run/status.txt; : > $ST
say(){ echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) $*" | tee -a $ST; }
( while :; do echo "$(date -u +%s) $(tail -1 $ST)" > $S/run/heartbeat; sleep 15; done ) &
HB=$!
rm -f $S/run/host.stop; bash $S/host_sampler.sh $OUT/host.log $S/run/host.stop &
trap 'kill $HB 2>/dev/null; touch $S/run/host.stop' EXIT
T0=$(date +%s)
step(){ n=$1; shift; say "BEGIN $n"; "$@" > $OUT/$n.console.txt 2>&1; say "END $n rc=$? t=$(( $(date +%s)-T0 ))s"; }

COMMON="WT=$WT SP=$S M1M4=$S/m1m4 BIN_BASE=$S/bin/kds_server-A BIN_CLOSE=$S/bin/kds_server-B"
step m1 env $COMMON RUN_ROOT=$RUN/m1 OUT=$OUT/m1 CELLS=m1 M1_REPS=3 M1_OPS=40000 bash $S/m1m4/run_m1m4.sh
step m4 env $COMMON RUN_ROOT=$RUN/m4 OUT=$OUT/m4 CELLS=m4 M4_OPS=4000 bash $S/m1m4/run_m1m4.sh
m3(){ # label bin skip
  step m3-$1 env $3=1 TREE=$WT BIN=$2 WORKROOT=$RUN/m3-$1 OUT=$OUT/m3-$1 HOST_SAMPLER=/nonexistent bash $S/m3/run_m3.sh; }
m3 s2-A-a $S/bin/kds_server-A SKIP_S0
m3 s2-B-a $S/bin/kds_server-B SKIP_S0
m3 s2-B-b $S/bin/kds_server-B SKIP_S0
m3 s2-A-b $S/bin/kds_server-A SKIP_S0
step spillmount bash $S/spill_mount.sh
if [ $(( $(date +%s)-T0 )) -lt 2400 ]; then
 m3 s0-A-a $S/bin/kds_server-A SKIP_S2
 m3 s0-B-a $S/bin/kds_server-B SKIP_S2
else say "CUT scenario0 (budget)"; fi
say "DONE t=$(( $(date +%s)-T0 ))s"
