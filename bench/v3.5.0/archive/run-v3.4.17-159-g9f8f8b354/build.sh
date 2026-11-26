#!/bin/bash
# Release build of kds_server at the measured commit (src) and the comparator (src-old).
R=/home/ukjin/bench-runs/scenarios-v3.4.17-159-g9f8f8b354
cd $R
for s in src src-old; do
  echo "$s start $(date -u +%H:%M:%S)" >> $R/build.status
  cmake -S $R/$s -B $R/$s/build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > $R/build-$s.log 2>&1 \
   && cmake --build $R/$s/build-release --target kds_server -j24 >> $R/build-$s.log 2>&1
  echo "$s rc=$? end $(date -u +%H:%M:%S)" >> $R/build.status
done
echo DONE >> $R/build.status
