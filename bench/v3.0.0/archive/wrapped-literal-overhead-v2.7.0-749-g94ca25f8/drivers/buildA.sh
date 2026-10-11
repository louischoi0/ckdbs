#!/bin/bash
S=/tmp/claude-1000/-home-cdkbs-ckdbs/305b0005-d7d2-422c-b0ed-247e8d0b8e88/scratchpad
export CMAKE_BUILD_PARALLEL_LEVEL=4
nice -n 19 taskset -c 0,1,6,7 $S/bs/build-release.sh $S/base-6b0d8412 kds_server > $S/lw/build-A2.log 2>&1
echo "exit $?" >> $S/lw/build-A2.log
nice -n 19 taskset -c 0,1,6,7 $S/bs/build-release.sh $S/lw/base-ctl kds_server > $S/lw/build-C.log 2>&1
echo "exit $?" >> $S/lw/build-C.log
date -u '+%F %T' > $S/lw/buildA.done
