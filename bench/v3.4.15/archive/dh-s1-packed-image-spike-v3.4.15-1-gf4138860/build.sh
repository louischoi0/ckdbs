#!/bin/bash
set -e
cd /home/louis/bench-runs/dh-s1/src
grep -q '<cstdlib>' src/server/expeditor.cpp || sed -i '0,/^#include </s//#include <cstdlib>\n#include </' src/server/expeditor.cpp
for h in '<cstdlib>' '<chrono>'; do grep -q "#include $h" include/kds/wal/manager.hpp || sed -i "0,/^#include </s//#include $h\n#include </" include/kds/wal/manager.hpp; done
date -u +%T > /home/louis/bench-runs/dh-s1/build.start
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > /home/louis/bench-runs/dh-s1/cmake.log 2>&1
cmake --build build --target kds_server -j8 > /home/louis/bench-runs/dh-s1/build.log 2>&1
date -u +%T > /home/louis/bench-runs/dh-s1/build.done
