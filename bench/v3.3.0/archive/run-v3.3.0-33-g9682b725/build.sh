#!/bin/bash
cd /home/cdkbs/ckdbs/.claude/worktrees/bench-scenarios-1-4
rm -f /home/cdkbs/bench-run/build.flag
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release > /home/cdkbs/bench-run/build.log 2>&1
cmake --build build-release -j4 --target kds_server >> /home/cdkbs/bench-run/build.log 2>&1
echo done $? > /home/cdkbs/bench-run/build.flag
