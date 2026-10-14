#!/bin/bash
cd /home/cdkbs/bench-runs/bw-update-prof
for n in A B; do
 cmake -S ../bw-close/src-$n -B build-$n -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-g -fno-omit-frame-pointer" -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF >build-$n.log 2>&1 && cmake --build build-$n --target kds_server -j8 >>build-$n.log 2>&1
 echo "$(date -u +%T) build $n rc=$?" >> progress.txt
done
echo "$(date -u +%T) builds done" >> progress.txt
