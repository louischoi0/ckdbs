#!/bin/sh
R=/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05
S=/tmp/claude-1000/-home-cdkbs-ckdbs/f6bce74e-e48d-4d48-817a-4e239db00c8e/scratchpad
rm -f $S/s10-build-C.done
cmake -S $R/src-C -B $R/src-C/build-release -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF -DKDS_BUILD_BENCH=OFF > $S/s10-build-C.log 2>&1 && nice -n 10 cmake --build $R/src-C/build-release --target kds_server -j6 >> $S/s10-build-C.log 2>&1
echo $? > $S/s10-build-C.done
