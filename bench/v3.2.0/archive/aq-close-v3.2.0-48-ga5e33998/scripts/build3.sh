#!/bin/bash
R=/home/cdkbs/bench-runs/aq-close
mkdir -p $R/build-Bp && cd $R/build-Bp && cmake -S $R/src-Bp -B . -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > $R/out/cmake-Bp.log 2>&1 && cmake --build . --target kds_server -j8 > $R/out/build-Bp.log 2>&1
echo "rc=$?" > $R/out/build-Bp.rc
