#!/bin/bash
# The layout check: release builds with function and loop starts aligned, so
# an unrelated change cannot shift a hot loop across a fetch boundary.
set -u
C=/home/cdkbs/bench-runs/close
mkdir -p $C/bin
for sha in "$@"; do
  [ -x $C/bin/kds_server.$sha.aligned ] && continue
  rm -rf $C/srcA-$sha && mkdir -p $C/srcA-$sha
  tar -xf $C/$sha.tar -C $C/srcA-$sha
  cmake -S $C/srcA-$sha -B $C/buildA-$sha -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_CXX_FLAGS=-falign-functions=64 -falign-loops=64 -falign-jumps=16" > $C/buildA-$sha.cfg.log 2>&1 || exit 1
  cmake --build $C/buildA-$sha -j8 --target kds_server > $C/buildA-$sha.log 2>&1 || exit 1
  cp $C/buildA-$sha/kds_server $C/bin/kds_server.$sha.aligned
  echo "built $sha aligned"
done
