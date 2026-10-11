#!/bin/bash
# Release builds of the stage commits for the literal-path bisection. Sequential, never during a measured cell.
W=/home/cdkbs/ckdbs/.claude/worktrees
B=/home/cdkbs/bench-runs/bj-close
rm -f $B/stages.done
for s in s0 s1 s2 s3 s4 s5 s6; do
  cd $W/bj-measure-$s || exit 1
  cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > $B/stage-cmake-$s.log 2>&1 || { echo "cmake $s failed" > $B/stages.done; exit 1; }
  cmake --build build-release --target kds_server -j6 > $B/stage-build-$s.log 2>&1 || { echo "build $s failed" > $B/stages.done; exit 1; }
  cp build-release/kds_server $B/bin/kds_server-$s
  stat -c "$s %y" build-release/kds_server >> $B/bin/stage-mtimes.txt
  echo "$s built $(date -u +%FT%TZ)" >> $B/stages.progress
done
cd $B/bin && sha256sum kds_server-s? > stage-hashes.txt
echo ok > $B/stages.done
