#!/bin/bash
S=/tmp/claude-1000/-home-cdkbs-ckdbs/305b0005-d7d2-422c-b0ed-247e8d0b8e88/scratchpad
date -u '+%F %T' > $S/at/build.start
nice -n 19 taskset -c 0-5 $S/bs/build-release.sh /home/cdkbs/ckdbs/.claude/worktrees/fix-assertion-on-invisible-relation kds_server > $S/at/build-B.log 2>&1
echo "exit $?" >> $S/at/build-B.log
date -u '+%F %T' > $S/at/build.done
