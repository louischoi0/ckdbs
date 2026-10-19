#!/bin/bash
cd /home/cdkbs/bench-runs/cd-close
cp build-A/kds_server bin/kds_server-A
cp build-A/kds_server bin/kds_server-A2
cp build-B/kds_server bin/kds_server-B
sha256sum bin/* > binaries.txt
stat -c '%n %y' build-A/kds_server build-B/kds_server >> binaries.txt
cat binaries.txt
grep CMAKE_BUILD_TYPE build-A/CMakeCache.txt build-B/CMakeCache.txt
cat /proc/loadavg
pgrep -c cc1plus
df -T /home/cdkbs/bench-runs/cd-close | tail -1
