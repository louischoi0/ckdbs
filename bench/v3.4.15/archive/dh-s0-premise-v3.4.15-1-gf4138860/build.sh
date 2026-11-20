#!/bin/bash
set -e
cd /home/louis/bench-runs/dh-s0/src
grep -q '<cstdlib>' src/server/expeditor.cpp || sed -i '0,/^#include </s//#include <cstdlib>\n#include </' src/server/expeditor.cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > /home/louis/bench-runs/dh-s0/cmake.log 2>&1
date -u +%T > /home/louis/bench-runs/dh-s0/build.start
cmake --build build --target kds_server -j8 > /home/louis/bench-runs/dh-s0/build.log 2>&1
date -u +%T > /home/louis/bench-runs/dh-s0/build.done
