#!/bin/bash
# Release builds of the close A/B's binaries from exports, one per commit;
# a commit whose binary exists is skipped.
set -u
C=/home/cdkbs/bench-runs/close
WT=/home/cdkbs/ckdbs/.claude/worktrees/bp-per-core-residue
mkdir -p $C/bin
for sha in "$@"; do
  [ -x $C/bin/kds_server.$sha ] && continue
  [ -f $C/$sha.tar ] || { echo "missing $C/$sha.tar"; exit 1; }
  rm -rf $C/src-$sha && mkdir -p $C/src-$sha
  tar -xf $C/$sha.tar -C $C/src-$sha
  cmake -S $C/src-$sha -B $C/build-$sha -DCMAKE_BUILD_TYPE=Release > $C/build-$sha.cfg.log 2>&1 || exit 1
  cmake --build $C/build-$sha -j8 --target kds_server > $C/build-$sha.log 2>&1 || exit 1
  cp $C/build-$sha/kds_server $C/bin/kds_server.$sha
  echo "built $sha"
done
