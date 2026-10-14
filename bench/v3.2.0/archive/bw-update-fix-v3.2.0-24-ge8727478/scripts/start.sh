#!/bin/bash
cd /home/cdkbs/bench-runs/bw-update-fix
cp build-C/kds_server bin/kds_server-C
{ sha256sum bin/*; stat -c '%n %y' build-C/kds_server; grep CMAKE_BUILD_TYPE build-C/CMakeCache.txt; } | tee binaries.txt
cat /proc/loadavg; pgrep -c cc1plus
nohup ./scripts/run.sh > out/run.sh.out 2>&1 &
echo $! > run.pid
nohup ./scripts/watch.sh > /dev/null 2>&1 &
sleep 3; tail -n 3 progress.txt
