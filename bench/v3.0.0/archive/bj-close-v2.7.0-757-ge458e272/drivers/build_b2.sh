#!/bin/bash
W=/home/cdkbs/ckdbs/.claude/worktrees/bj-measure-b2
B=/home/cdkbs/bench-runs/bj-close
cd $W || exit 1
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > $B/b2-cmake.log 2>&1 || { echo "cmake failed" > $B/b2.done; exit 1; }
cmake --build build-release --target kds_server -j6 > $B/b2-build.log 2>&1 || { echo "build failed" > $B/b2.done; exit 1; }
cp build-release/kds_server $B/bin/kds_server-B2
stat -c "%y" build-release/kds_server > $B/bin/B2-mtime.txt
grep CMAKE_BUILD_TYPE build-release/CMakeCache.txt >> $B/bin/B2-mtime.txt
cd $B/bin && sha256sum kds_server-A kds_server-B kds_server-B2 > binaries2.txt
echo ok > $B/b2.done
