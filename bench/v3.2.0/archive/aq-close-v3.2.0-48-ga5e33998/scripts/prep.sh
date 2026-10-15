#!/bin/bash
cd /home/cdkbs/bench-runs/aq-close
cp build-A/kds_server bin/kds_server-A
cp build-A/kds_server bin/kds_server-A2
cp build-B/kds_server bin/kds_server-B
{ sha256sum bin/*; stat -c '%n %y' build-A/kds_server build-B/kds_server; } > binaries.txt
cat binaries.txt
grep CMAKE_BUILD_TYPE build-A/CMakeCache.txt build-B/CMakeCache.txt
cat /proc/loadavg
pgrep cc1plus
g++ --version | head -1
