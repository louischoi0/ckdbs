#!/bin/bash
# BQ-S2's measurement builds: one release kds_server per arm from an export
# of 4cf30270, patched by variants.py. Skips an arm whose binary exists.
set -u
S2=/home/cdkbs/bench-runs/bq/s2
mkdir -p $S2/bin
for arm in opt3 s16 s256 s1024; do
  [ -x $S2/bin/kds_server.$arm ] && continue
  rm -rf $S2/src-$arm && mkdir -p $S2/src-$arm
  tar -xf $S2/4cf30270.tar -C $S2/src-$arm
  python3 $S2/variants.py $S2/src-$arm $arm || exit 1
  cmake -S $S2/src-$arm -B $S2/build-$arm -DCMAKE_BUILD_TYPE=Release > $S2/build-$arm.cfg.log 2>&1 || exit 1
  cmake --build $S2/build-$arm -j8 --target kds_server > $S2/build-$arm.log 2>&1 || exit 1
  cp $S2/build-$arm/kds_server $S2/bin/kds_server.$arm
  echo "built $arm"
done
