#!/bin/bash
# G16-G20 close: A=63fec9d6 (kds_server-A2), B=83375075 (kds_server-B2). interleaved.
S=/tmp/claude-1000/-home-louis-ckdbs/27596f25-ceea-4694-a7ec-1a2566519ac2/scratchpad
WT=/home/louis/ckdbs/.claude/worktrees/audit-g1-g15-fixes
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
RUN=/home/louis/bench-runs/audit-g16-close/$STAMP
OUT=$S/run4/$STAMP
mkdir -p $RUN $OUT
ST=$S/run4/status.txt; : > $ST
say(){ echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) $*" | tee -a $ST; }
( while :; do echo "$(date -u +%s) $(tail -1 $ST)" > $S/run4/heartbeat; sleep 15; done ) &
HB=$!
rm -f $S/run4/host.stop; bash $S/host_sampler.sh $OUT/host.log $S/run4/host.stop &
trap 'kill $HB 2>/dev/null; touch $S/run4/host.stop' EXIT
T0=$(date +%s)
step(){ n=$1; shift; say "BEGIN $n"; "$@" > $OUT/$n.console.txt 2>&1; say "END $n rc=$? t=$(( $(date +%s)-T0 ))s"; }
BA=$S/bin/kds_server-A2; BB=$S/bin/kds_server-B2

step m2 env M2_BASE=$BA M2_NOSLICE=$BB M2_OUT=$OUT/m2 M2_WORKDIR=$RUN/m2work M2_RUNS=5 M2_CENSUS_TIMEOUT=360 M2_HOST_WAIT=5 bash $S/m2/m2_run_m2.sh
COMMON="WT=$WT SP=$S M1M4=$S/m1m4"
step m1 env $COMMON BIN_BASE=$BA BIN_CLOSE=$BB RUN_ROOT=$RUN/m1 OUT=$OUT/m1 CELLS=m1 M1_REPS=3 M1_OPS=40000 bash $S/m1m4/run_m1m4.sh
step m1swap env $COMMON BIN_BASE=$BB BIN_CLOSE=$BA RUN_ROOT=$RUN/m1s OUT=$OUT/m1swap CELLS=m1 M1_REPS=3 M1_OPS=40000 PORT_BASE=15620 PORT_BASE_DBG=15621 PORT_CLOSE=15622 PORT_CLOSE_DBG=15623 bash $S/m1m4/run_m1m4.sh
step m4 env $COMMON BIN_BASE=$BA BIN_CLOSE=$BB RUN_ROOT=$RUN/m4 OUT=$OUT/m4 CELLS=m4 M4_OPS=4000 bash $S/m1m4/run_m1m4.sh
m3(){ step m3-$1 env $3=1 TREE=$WT BIN=$2 WORKROOT=$RUN/m3-$1 OUT=$OUT/m3-$1 HOST_SAMPLER=/nonexistent bash $S/m3/run_m3.sh; }
m3 s2-A-a $BA SKIP_S0
m3 s2-B-a $BB SKIP_S0
m3 s2-B-b $BB SKIP_S0
m3 s2-A-b $BA SKIP_S0
m3 s0-A-a $BA SKIP_S2
m3 s0-B-a $BB SKIP_S2
m3 s0-B-b $BB SKIP_S2
m3 s0-A-b $BA SKIP_S2
say "DONE t=$(( $(date +%s)-T0 ))s"
