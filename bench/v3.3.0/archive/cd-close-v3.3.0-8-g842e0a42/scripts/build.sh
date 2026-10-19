#!/bin/bash
cd /home/cdkbs/bench-runs/cd-close
for L in A B; do
 ( cmake -S src-$L -B build-$L -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > out/cmake-$L.log 2>&1 && cmake --build build-$L --target kds_server -j4 > out/build-$L.log 2>&1; echo "$(date -u +%T) build $L done rc=$?" >> progress.txt ) &
done
wait
