#!/bin/bash
cd /home/cdkbs/ckdbs/.claude/worktrees/ce-streaming-reply
R=/home/cdkbs/bench-runs/ce-close-v3.3.0-52-g9e568b44
date -u +%FT%TZ > $R/build.t0
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release > $R/build.log 2>&1 && cmake --build build-release --target kds_server -j4 >> $R/build.log 2>&1
echo $? > $R/build.rc
date -u +%FT%TZ > $R/build.t1
touch $R/build.done
