#!/bin/bash
S=/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-fix-ddl-build-fault-net/2fe39e3f-40fc-42ed-910b-fde09fa4cb64/scratchpad
R=/home/cdkbs/ckdbs/.claude/worktrees/ddl-build-yield
O=$S/out; mkdir -p $O
L=$S/a1-close-progress.log
env_snap() { echo "$1 loadavg=$(cat /proc/loadavg) procs=[$(pgrep -a -f 'cc1plus|cmake --build|ctest' | cut -c1-60 | tr '\n' ';')]" >> $O/env.log; }
cell() { # mode label binary rep extra...
  mode=$1; lab=$2; bin=$3; rep=$4; shift 4
  tag=$mode-$lab-$rep
  env_snap "before $tag"
  echo "$(date -u +%T) cell $tag start" >> $L
  if [ $mode = stmt ]; then cores=1; port=15811; else cores=1; port=15812; fi
  /home/cdkbs/a1-run/srv.sh start $tag $bin $port $cores || { echo "$(date -u +%T) ABORTED server start $tag" >> $L; exit 1; }
  (cd $R; python3 -I tools/a1_close_benchmark.py --port $port --mode $mode --json $O/$tag.json "$@" > $O/$tag.out 2>&1)
  rc=$?
  /home/cdkbs/a1-run/srv.sh stop $tag
  env_snap "after $tag rc=$rc"
  echo "$(date -u +%T) cell $tag done rc=$rc" >> $L
}
MODE=$1
if [ $MODE = stmt ]; then
  for x in A:1 B:1 B:2 A:2 A:3 B:3 B:4 A:4 A:5 B:5 B:6 A:6 A:7 B:7 B:8 A:8 A:9 B:9 B:10 A:10; do
    l=${x%:*}; r=${x#*:}
    cell stmt $l /home/cdkbs/a1-run/kds_$l $r --sizes 200,1000,10000 --ops 2000 --scan-ops 20
  done
else
  for x in A:1 B:1 B:2 A:2 A:3 B:3; do
    l=${x%:*}; r=${x#*:}
    cell build $l /home/cdkbs/a1-run/kds_$l $r --rows 1000000 --note-len 200 --idle 1.0
  done
fi
echo "$(date -u +%T) phase $MODE finished" >> $L
