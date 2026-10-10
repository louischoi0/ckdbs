#!/bin/bash
S=/tmp/claude-1000/-home-cdkbs-ckdbs/305b0005-d7d2-422c-b0ed-247e8d0b8e88/scratchpad
export CMAKE_BUILD_PARALLEL_LEVEL=4
nice -n 19 taskset -c 4-7 $S/bs/build-release.sh /home/cdkbs/ckdbs/.claude/worktrees/fix-nested-uncorrelated-subquery kds_server > $S/nhf/build-Bp.log 2>&1
echo "exit $?" >> $S/nhf/build-Bp.log
echo done > $S/nhf/build.done
