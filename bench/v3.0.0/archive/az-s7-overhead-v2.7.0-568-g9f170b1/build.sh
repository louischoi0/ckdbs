#!/bin/bash
cd /home/cdkbs/bench-runs/az-s7b/src-B
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > ../cfg-B.log 2>&1 && cmake --build build --target kds_server -j6 > ../build-B.log 2>&1
echo "exit $?" >> ../build-B.log
echo done > ../build.done
