#!/bin/bash
cd /home/cdkbs/bench-runs/bw-close
cp build-A/kds_server bin/kds_server-A; cp build-A/kds_server bin/kds_server-A2; cp build-B/kds_server bin/kds_server-B
{ sha256sum bin/*; stat -c '%n %y' build-A/kds_server build-B/kds_server; } > binaries.txt; cat binaries.txt
grep CMAKE_BUILD_TYPE build-A/CMakeCache.txt build-B/CMakeCache.txt
cat /proc/loadavg; pgrep -c cc1plus
nohup ./scripts/run.sh > out/run.sh.out 2>&1 &
echo $! > run.pid
nohup ./scripts/watch.sh > /dev/null 2>&1 &
sleep 2; tail -n 3 progress.txt
