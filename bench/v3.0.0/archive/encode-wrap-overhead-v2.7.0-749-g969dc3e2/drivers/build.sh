#!/bin/bash
S=/tmp/claude-1000/-home-cdkbs-ckdbs/305b0005-d7d2-422c-b0ed-247e8d0b8e88/scratchpad
date -u '+%F %T' > $S/ew/build.start
nice -n 19 taskset -c 6,7 $S/bs/build-release.sh /home/cdkbs/ckdbs/.claude/worktrees/fix-date-wrapped-literal-stored kds_server > $S/ew/build-B.log 2>&1
echo "exit $?" >> $S/ew/build-B.log
nice -n 19 taskset -c 6,7 $S/bs/build-release.sh $S/ew/base-ctl kds_server > $S/ew/build-C.log 2>&1
echo "exit $?" >> $S/ew/build-C.log
date -u '+%F %T' > $S/ew/build.done
