#!/bin/bash
S=/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-bm-scan-kernel-open/382ae513-4d71-443e-8517-51d482efafba/scratchpad/s6r
cd $S
cmake -S $S/src -B $S/build -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > $S/build.log 2>&1 && cmake --build $S/build --target kds_server -j8 >> $S/build.log 2>&1
echo "EXIT $?" > $S/build.done
