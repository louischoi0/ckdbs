#!/bin/bash
cd /home/cdkbs/ckdbs/.claude/worktrees/ce-drain-variant || exit 1
R=/home/cdkbs/bench-runs
date -u +%FT%TZ > $R/ce-drain-build.t0
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release > $R/ce-drain-build.log 2>&1 && cmake --build build-release --target kds_server -j4 >> $R/ce-drain-build.log 2>&1
echo $? > $R/ce-drain-build.rc
date -u +%FT%TZ > $R/ce-drain-build.t1
