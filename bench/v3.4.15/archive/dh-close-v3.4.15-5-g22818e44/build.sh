#!/bin/bash
cd /home/louis/bench-runs/dh-close
date -u +%T > build.start
for v in A B X; do
  cd /home/louis/bench-runs/dh-close/$v
  if [ $v = X ]; then
    grep -q '<cstdlib>' src/server/expeditor.cpp || sed -i '0,/^#include </s//#include <cstdlib>\n#include </' src/server/expeditor.cpp
    grep -q '<cstdlib>' src/wal/manager.cpp || sed -i '0,/^#include </s//#include <cstdlib>\n#include </' src/wal/manager.cpp
  fi
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > ../cmake-$v.log 2>&1
  cmake --build build --target kds_server -j8 > ../build-$v.log 2>&1
  date -u +%T > ../build-$v.done
done
date -u +%T > /home/louis/bench-runs/dh-close/build.done
