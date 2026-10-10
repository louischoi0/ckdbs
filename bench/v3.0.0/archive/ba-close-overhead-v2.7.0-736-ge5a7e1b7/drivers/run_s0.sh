#!/bin/bash
# After the census: (1) cores = 2 relaxed in a quiet window, (2) group cores = 2 repeat, (3) scenario 0's four cells x 3 passes.
B=/home/cdkbs/bench-runs/ba-close
gate() {
  n=0
  while pgrep -x ctest > /dev/null || pgrep -x cc1plus > /dev/null || pgrep -x kds_tests > /dev/null; do
    sleep 15; n=$((n + 1)); test $n -ge 120 && break
  done
  echo "gate waited $((n * 15)) s at $(date -u +%T)" >> $B/post.log
}
rm -f $B/s0.done
rm -rf $B/s0
# scenario 0, the arguments and cell order of bench/v3.0.0/archive/scenario0-v2.7.0-531-g9a0525d/run_s02.sh
S0=$B/s0
mkdir -p $S0/json $S0/logs $S0/data
S0_ARGS="--users 100 --accounts-per-user 3 --assets 30 --traders 8 --txn-per-user 50 --verify 200 --seed 1 --sync"
TOOLS=/home/cdkbs/ckdbs/.claude/worktrees/ba-open-marks/tools
cell0() {  # tag suffix port cores durability
  TAG=$1; SUF=$2; PORT=$3; CORES=$4; DUR=$5
  DIR=$S0/data/$TAG; rm -rf $DIR; mkdir -p $DIR
  gate
  echo "== $TAG start $(date -u +%FT%TZ) load $(cut -d' ' -f1-3 /proc/loadavg)" >> $S0/timeline
  pgrep -a -f "cc1plus|cmake --build|ctest" | grep -v pgrep >> $S0/logs/$TAG.competing 2>&1
  printf 'data_file = %s/kds.db\ncores = %s\nport = %s\ndurability = %s\nlog_dir = %s\nlog_file = kds.log\nlog_level = warn\nbuffer_pool_frames = 65536\n' $DIR $CORES $PORT $DUR $DIR > $DIR/conf
  $B/bin/kds_server-B --config $DIR/conf > $DIR/stdout.log 2>&1 &
  PID=$!
  for i in $(seq 1 150); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break; sleep 0.2; done
  python3 $TOOLS/scenario0_stockmarket.py --port $PORT --suffix $SUF $S0_ARGS --json $S0/json/$TAG.json > $S0/logs/$TAG.stdout 2>&1
  echo "$?" > $S0/logs/$TAG.rc
  kill -TERM $PID; for i in $(seq 1 50); do kill -0 $PID 2>/dev/null || break; sleep 0.2; done
  kill -0 $PID 2>/dev/null && kill -KILL $PID
  cp $DIR/kds.log $S0/logs/$TAG.serverlog 2>/dev/null
  rm -rf $DIR
  echo "== $TAG end $(date -u +%FT%TZ) rc=$(cat $S0/logs/$TAG.rc)" >> $S0/timeline
}
for P in "" r2- r3-; do
  cell0 ${P}s0-c1-g s0c1g 15700 1 group
  cell0 ${P}s0-c8-g s0c8g 15701 8 group
  cell0 ${P}s0-c1-s s0c1s 15702 1 strict
  cell0 ${P}s0-c8-s s0c8s 15703 8 strict
done
echo "s0 done $(date -u +%T)" >> $B/post.log
touch $B/s0.done
