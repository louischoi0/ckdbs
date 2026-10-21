#!/bin/bash
S=/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-fix-ddl-build-fault-net/2fe39e3f-40fc-42ed-910b-fde09fa4cb64/scratchpad
R=/home/cdkbs/ckdbs/.claude/worktrees/ddl-build-yield
D=/home/cdkbs/a1-run/diag2
O=$S/out; L=$S/a1-close-progress.log
echo "$(date -u +%T) diag2 cell start; load $(cat /proc/loadavg) procs=[$(pgrep -a -f 'cc1plus|cmake --build|ctest' | cut -c1-50 | tr '\n' ';')]" >> $L
/home/cdkbs/a1-run/srv.sh start diag2 /home/cdkbs/a1-run/kds_B 15816 1 || exit 1
# poller: every 50 ms, log any change of the wal dir listing with epoch time
(
  prev=""
  while [ ! -f $D/stop ]; do
    cur=$(stat -c '%n %y %s' $D/wal/* 2>/dev/null)
    if [ "$cur" != "$prev" ]; then
      echo "T $(date +%s.%N)" >> $O/diag2-poll.log
      echo "$cur" >> $O/diag2-poll.log
      prev="$cur"
    fi
    sleep 0.05
  done
) &
PP=$!
echo "T0 $(date +%s.%N)" > $O/diag2-poll.log
(cd $R; python3 -I tools/a1_close_benchmark.py --port 15816 --mode build --rows 1000000 --note-len 200 --idle 1.0 --json $O/diag2.json > $O/diag2.out 2>&1)
echo "rc=$?" >> $O/diag2.out
stat -c '%n %y %z %s' $D/wal/* > $O/diag2-wal-final.txt
touch $D/stop; wait $PP
/home/cdkbs/a1-run/srv.sh stop diag2
echo "$(date -u +%T) diag2 cell done; load $(cat /proc/loadavg)" >> $L
