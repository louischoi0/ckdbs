#!/bin/bash
S=/tmp/claude-1000/-home-cdkbs-ckdbs/00ec4c4c-8445-418e-92d3-c235d2b282e7/scratchpad/cc-s0
R=/home/cdkbs/bench-runs/cc-s0/src
cd $R
rm -f $S/build.done
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > $S/cfg.log 2>&1 && cmake --build build-release --target kds_server -j8 > $S/build.log 2>&1
echo $? > $S/build.done
date -u > $S/build.end
