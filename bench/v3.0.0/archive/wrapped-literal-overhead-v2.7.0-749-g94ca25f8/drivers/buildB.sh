#!/bin/bash
S=/tmp/claude-1000/-home-cdkbs-ckdbs/305b0005-d7d2-422c-b0ed-247e8d0b8e88/scratchpad
date -u '+%F %T' > $S/lw/build.start
nice -n 19 taskset -c 0,1,6,7 $S/bs/build-release.sh /home/cdkbs/ckdbs/.claude/worktrees/fix-int-literal-wrap-in-where kds_server > $S/lw/build-B.log 2>&1
echo "exit $?" >> $S/lw/build-B.log
date -u '+%F %T' > $S/lw/build.done
