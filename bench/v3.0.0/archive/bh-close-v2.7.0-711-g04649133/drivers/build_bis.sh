#!/bin/bash
# Release builds of BH's stage commits for the range100 bisection. No benchmark runs while this does.
D=/home/cdkbs/bench-runs/bh-close/bis
for s in S1 S2 S3; do
  cmake -S $D/src-$s -B $D/build-$s -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > $D/cfg-$s.log 2>&1
  cmake --build $D/build-$s --target kds_server -j8 > $D/build-$s.log 2>&1
  echo "$s exit=$?" >> $D/build.status
  cp $D/build-$s/kds_server /home/cdkbs/bench-runs/bh-close/bin/kds_server-$s
done
echo finished >> $D/build.status
