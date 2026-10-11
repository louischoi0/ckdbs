#!/bin/bash
# Builds kds_server at each exported commit, one after another.
D=/home/cdkbs/bench-runs/carve-diag
for c in 66bb11b2 91a14db0 7fa2bd96; do
  rm -rf $D/src-$c && mkdir -p $D/src-$c
  tar -xf $D/src-$c.tar -C $D/src-$c
  cmake -S $D/src-$c -B $D/src-$c/build -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > $D/build-$c.log 2>&1
  cmake --build $D/src-$c/build -j4 --target kds_server >> $D/build-$c.log 2>&1 && cp $D/src-$c/build/kds_server $D/bin/kds_server-$c && echo "built $c" >> $D/bisect.log
done
touch $D/built
