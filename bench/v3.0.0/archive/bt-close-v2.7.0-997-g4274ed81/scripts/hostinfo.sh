#!/bin/bash
R=/home/cdkbs/bench-runs/bt-close
W=/home/cdkbs/ckdbs/.claude/worktrees/bt-accept-tuple-row
{
  echo "date: $(date -u '+%F %H:%M UTC')"
  echo "kernel: $(uname -r)"
  echo "cpu: $(lscpu | grep -m1 'Model name' | sed 's/  */ /g')"; echo "topology: $(lscpu | grep -E '^(CPU\(s\)|Thread|Core)' | tr '\n' ';')"
  echo "gcc: $(g++ --version | head -1)"
  echo "data device: $(df -T $R | tail -1)"
  echo "build cache (B):"
  grep -E 'CMAKE_BUILD_TYPE|CMAKE_CXX_FLAGS:|KDS_BUILD_(SIM|TESTS)' $R/rel/CMakeCache.txt
  echo "driver sha256:"
  (cd $W && sha256sum tools/bm_scan.py tools/bn_census.py tools/kwp.py tools/bb_overhead_benchmark.py)
  echo "worktree HEAD: $(git -C $W rev-parse --short HEAD) ($(git -C $W describe --tags))"
  echo "worktree status:"
  git -C $W status --short
  echo "diff --stat eeb88f47 4274ed81 -- src include:"
  git -C $W diff --stat eeb88f47 4274ed81 -- src include
  echo "tools/ unchanged since: $(git -C $W log -1 --format='%h %cI' -- tools/bm_scan.py tools/bn_census.py tools/kwp.py tools/bb_overhead_benchmark.py)"
  echo "free:"; free -g | head -2
} > $R/out/host.txt 2>&1
