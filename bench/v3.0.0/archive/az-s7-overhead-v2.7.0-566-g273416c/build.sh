#!/bin/bash
cd /home/cdkbs/bench-runs/az-s7
for X in A B; do
 ( cd src-$X && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > ../cfg-$X.log 2>&1 && cmake --build build --target kds_server -j4 > ../build-$X.log 2>&1; echo "exit $?" >> ../build-$X.log ) &
done
wait
echo done > build.done
