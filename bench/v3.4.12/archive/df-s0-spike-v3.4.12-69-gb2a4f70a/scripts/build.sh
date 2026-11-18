#!/bin/bash
R=/home/louis/bench-runs/df-s0
tar -x -f $R/src.tar -C $R/src
echo "$(date -u '+%F %T UTC') build start" >> $R/progress.txt
cmake -S $R/src -B $R/build -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > $R/out/cmake.log 2>&1 && cmake --build $R/build --target kds_server -j6 > $R/out/build.log 2>&1
echo "rc=$?" > $R/flags/build
echo "$(date -u '+%F %T UTC') build done $(cat $R/flags/build)" >> $R/progress.txt
