#!/bin/bash
# G21/G23-G26 close: A=61917b0f (kds_server-A), B=f2b8aa09 (kds_server-B). interleaved.
S=/tmp/claude-1000/-home-louis-ckdbs/27596f25-ceea-4694-a7ec-1a2566519ac2/scratchpad
G=$S/g21
WT=/home/louis/ckdbs/.claude/worktrees/audit-g1-g15-fixes
T=$WT/tools
STAMP=20261115T074119Z
RUN=/home/louis/bench-runs/audit-g21-close/$STAMP
OUT=$G/out/$STAMP
mkdir -p $RUN $OUT $OUT/tainted
ST=$G/status5.txt; : > $ST
say(){ echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) $*" | tee -a $ST; }
( while :; do echo "$(date -u +%s) $(tail -1 $ST)" > $G/heartbeat5; sleep 15; done ) &
HB=$!
rm -f $G/host5.stop; bash $S/host_sampler.sh $OUT/host5.log $G/host5.stop &
trap 'kill $HB 2>/dev/null; touch $G/host5.stop' EXIT
T0=$(date +%s); DEADLINE=$(date -u -d "2026-11-15 08:30:00" +%s)
busy(){ n=$(( $(pgrep -x cc1plus | wc -l) + $(pgrep -x ctest | wc -l) + $(pgrep -x kds_tests | wc -l) + $(pgrep -x ld | wc -l) )); echo $n; }
( while [ ! -e $G/host5.stop ]; do echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) load=$(cut -d' ' -f1-3 /proc/loadavg) busy=$(busy)" >> $OUT/quiet5.log; sleep 2; done ) &
waitquiet(){ local ok=0 t=0; while [ $ok -lt 3 ]; do if [ $(busy) -eq 0 ]; then ok=$((ok+1)); else ok=0; fi; sleep 5; t=$((t+5)); [ $t -gt 45 ] && { say "waitquiet gave up"; return; }; done; }
step(){ n=$1; shift; [ $(date +%s) -gt $DEADLINE ] && { say "SKIP $n (deadline)"; return; }; local try=1; while :; do waitquiet; local b=$(date -u +%Y-%m-%dT%H:%M:%SZ); say "BEGIN $n try$try"; "$@" > $OUT/$n.console.txt 2>&1; local rc=$?; local e=$(date -u +%Y-%m-%dT%H:%M:%SZ); local nb=$(awk -v b=$b -v e=$e '$1>=b && $1<=e && $3!="busy=0"' $OUT/quiet5.log | wc -l); say "END $n rc=$rc busy_samples=$nb t=$(( $(date +%s)-T0 ))s"; if [ $nb -gt 0 ] && [ $try -lt 1 ] && [ $(date +%s) -lt $DEADLINE ]; then say "TAINTED $n: rerun"; mkdir -p $OUT/tainted/$n.try$try; mv $OUT/$n $OUT/tainted/$n.try$try/ 2>/dev/null; mv $OUT/$n.console.txt $OUT/tainted/$n.try$try/; try=$((try+1)); continue; fi; [ $nb -gt 0 ] && say "TAINTED $n: kept, flagged"; break; done; }
BA=$G/bin/kds_server-A; BB=$G/bin/kds_server-B
COMMON="WT=$WT SP=$S M1M4=$S/m1m4"
mt(){
  python3 -u $T/mount_cost_benchmark.py --binary $G/bin/wrap-$1.sh --label $1 --rows $2 --mounts 7 --durability relaxed $5 \
    --port 15481 --scratch $RUN/mt-$1-$2-$3-$4 --json $OUT/mt-$4-$2-$1-r$3.json --force > $OUT/mt-$4-$2-$1-r$3.txt 2>&1
  rm -rf $RUN/mt-$1-$2-$3-$4; }
mount_cells(){
 orders=("A B" "B A" "A B")
 for cell in "0 mount" "4000 mountassert" "4000 mountassertcrash" "10000 mountassert"; do
  set -- $cell; rows=$1; name=$2; flag=""
  case $name in mountcrash) flag="--crash";; mountassert) flag="--assertion";; mountassertcrash) flag="--assertion --crash";; esac
  for rep in 1 2 3; do for l in ${orders[$((rep-1))]}; do say "$name $l rows=$rows rep=$rep"; mt $l $rows $rep $name "$flag"; done; done
 done; }
as1(){
 for cfg in "200 2 3" "1000 2 3" "10000 2 3" "1000 3 2"; do set -- $cfg; pre=$1; ca=$2; cb=$3
  python3 $SRV start --binary $G/bin/kds_server-A --workdir $RUN/a1-$pre-$ca-A --port 15711 --debug-text-port 0 --cores 1 --taskset $ca --durability relaxed --buffer-pool-frames 65536 --ash on --label a1-$pre-A > $OUT/a1-$pre-$ca-A.start.txt 2>&1
  python3 $SRV start --binary $G/bin/kds_server-B --workdir $RUN/a1-$pre-$ca-B --port 15712 --debug-text-port 0 --cores 1 --taskset $cb --durability relaxed --buffer-pool-frames 65536 --ash on --label a1-$pre-B > $OUT/a1-$pre-$ca-B.start.txt 2>&1
  say "assert1 preload=$pre A@$ca B@$cb"
  taskset -c 0,1 python3 -u $T/assertion_overhead_benchmark.py --port 15712 --label B --ab-port 15711 --ab-label A --preload $pre --ops 8000 --blocks 16 --json $OUT/assert1-$pre-A$ca.json > $OUT/assert1-$pre-A$ca.txt 2>&1
  for l in A B; do python3 $SRV stop --workdir $RUN/a1-$pre-$ca-$l > $OUT/a1-$pre-$ca-$l.stop.txt 2>&1; done
 done; }
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
step mount mount_cells
step m2 env M2_BASE=$BA M2_NOSLICE=$BB M2_OUT=$OUT/m2 M2_WORKDIR=$RUN/m2work M2_RUNS=5 M2_CENSUS_TIMEOUT=360 M2_HOST_WAIT=5 bash $S/m2/m2_run_m2.sh
m3 s0-A-a $BA SKIP_S2
m3 s0-B-a $BB SKIP_S2
step assert1 as1
m3 s0-B-b $BB SKIP_S2
m3 s0-A-b $BA SKIP_S2
step m1x env $COMMON BIN_BASE=$BA BIN_CLOSE=$BB RUN_ROOT=$RUN/m1x OUT=$OUT/m1x CELLS=m1 M1_REPS=3 M1_OPS=40000 PORT_BASE=15640 PORT_BASE_DBG=15641 PORT_CLOSE=15642 PORT_CLOSE_DBG=15643 bash $S/m1m4/run_m1m4.sh
say "DONE t=$(( $(date +%s)-T0 ))s"
