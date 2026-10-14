#!/bin/bash
cd /home/cdkbs/bench-runs/bw-update-fix
cmake -S src-C -B build-C -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF >build-C.log 2>&1 && cmake --build build-C --target kds_server -j4 >>build-C.log 2>&1
echo "$(date -u +%T) build C done rc=$?" >> progress.txt
