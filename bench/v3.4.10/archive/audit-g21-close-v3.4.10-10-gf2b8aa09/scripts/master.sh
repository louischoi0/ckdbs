#!/bin/bash
# G21/G23-G26 close: A=61917b0f (kds_server-A), B=f2b8aa09 (kds_server-B). interleaved.
S=/tmp/claude-1000/-home-louis-ckdbs/27596f25-ceea-4694-a7ec-1a2566519ac2/scratchpad
G=$S/g21
WT=/home/louis/ckdbs/.claude/worktrees/audit-g1-g15-fixes
T=$WT/tools
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
RUN=/home/louis/bench-runs/audit-g21-close/$STAMP
OUT=$G/out/$STAMP
mkdir -p $RUN $OUT
ST=$G/status.txt; : > $ST
say(){ echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) $*" | tee -a $ST; }
( while :; do echo "$(date -u +%s) $(tail -1 $ST)" > $G/heartbeat; sleep 15; done ) &
HB=$!
rm -f $G/host.stop; bash $S/host_sampler.sh $OUT/host.log $G/host.stop &
trap 'kill $HB 2>/dev/null; touch $G/host.stop' EXIT
T0=$(date +%s)
step(){ n=$1; shift; say "BEGIN $n"; "$@" > $OUT/$n.console.txt 2>&1; say "END $n rc=$? t=$(( $(date +%s)-T0 ))s"; }
BA=$G/bin/kds_server-A; BB=$G/bin/kds_server-B
COMMON="WT=$WT SP=$S M1M4=$S/m1m4"
step m1 env $COMMON BIN_BASE=$BA BIN_CLOSE=$BB RUN_ROOT=$RUN/m1 OUT=$OUT/m1 CELLS=m1 M1_REPS=3 M1_OPS=40000 bash $S/m1m4/run_m1m4.sh
step m1swap env $COMMON BIN_BASE=$BB BIN_CLOSE=$BA RUN_ROOT=$RUN/m1s OUT=$OUT/m1swap CELLS=m1 M1_REPS=3 M1_OPS=40000 PORT_BASE=15620 PORT_BASE_DBG=15621 PORT_CLOSE=15622 PORT_CLOSE_DBG=15623 bash $S/m1m4/run_m1m4.sh
step m4 env $COMMON BIN_BASE=$BA BIN_CLOSE=$BB RUN_ROOT=$RUN/m4 OUT=$OUT/m4 CELLS=m4 M4_OPS=4000 bash $S/m1m4/run_m1m4.sh
SRV=$S/m1m4/srv.py
as(){
 for pre in 200 1000 10000; do
  for l in A B; do
    python3 $SRV start --binary $G/bin/kds_server-$l --workdir $RUN/as-$pre-$l --port $([ $l = A ] && echo 15701 || echo 15702) --debug-text-port 0 --cores 4 --taskset 2-5 --durability relaxed --buffer-pool-frames 65536 --ash on --label as-$pre-$l > $OUT/as-$pre-$l.start.txt 2>&1
  done
  say "assertion preload=$pre"
  taskset -c 6-7 python3 -u $T/assertion_overhead_benchmark.py --port 15702 --label B --ab-port 15701 --ab-label A --preload $pre --ops 8000 --blocks 16 --json $OUT/assert-$pre.json > $OUT/assert-$pre.txt 2>&1
  for l in A B; do python3 $SRV stop --workdir $RUN/as-$pre-$l > $OUT/as-$pre-$l.stop.txt 2>&1; done
 done; }
step assert as
m3(){ step m3-$1 env $3=1 TREE=$WT BIN=$2 WORKROOT=$RUN/m3-$1 OUT=$OUT/m3-$1 HOST_SAMPLER=/nonexistent bash $S/m3/run_m3.sh; }
m3 s2-A-a $BA SKIP_S0
m3 s2-B-a $BB SKIP_S0
m3 s2-B-b $BB SKIP_S0
m3 s2-A-b $BA SKIP_S0
m3 s0-A-a $BA SKIP_S2
m3 s0-B-a $BB SKIP_S2
m3 s0-B-b $BB SKIP_S2
m3 s0-A-b $BA SKIP_S2
mt(){
  python3 -u $T/mount_cost_benchmark.py --binary $G/bin/wrap-$1.sh --label $1 --rows $2 --mounts 7 --durability relaxed $5 \
    --port 15481 --scratch $RUN/mt-$1-$2-$3-$4 --json $OUT/mt-$4-$2-$1-r$3.json --force > $OUT/mt-$4-$2-$1-r$3.txt 2>&1
  rm -rf $RUN/mt-$1-$2-$3-$4; }
mount_cells(){
 orders=("A B" "B A" "A B")
 for cell in "0 mount" "4000 mount" "4000 mountcrash" "4000 mountassert" "4000 mountassertcrash" "10000 mountassert"; do
  set -- $cell; rows=$1; name=$2; flag=""
  case $name in mountcrash) flag="--crash";; mountassert) flag="--assertion";; mountassertcrash) flag="--assertion --crash";; esac
  for rep in 1 2 3; do for l in ${orders[$((rep-1))]}; do say "$name $l rows=$rows rep=$rep"; mt $l $rows $rep $name "$flag"; done; done
 done; }
step mount mount_cells
step m2 env M2_BASE=$BA M2_NOSLICE=$BB M2_OUT=$OUT/m2 M2_WORKDIR=$RUN/m2work M2_RUNS=5 M2_CENSUS_TIMEOUT=360 M2_HOST_WAIT=5 bash $S/m2/m2_run_m2.sh
say "DONE t=$(( $(date +%s)-T0 ))s"
