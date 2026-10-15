#!/bin/bash
R=/home/cdkbs/bench-runs/aq-close
for n in A B; do
 ( mkdir -p $R/build-$n && cd $R/build-$n && cmake -S $R/src-$n -B . -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > $R/out/cmake-$n.log 2>&1 && cmake --build . --target kds_server -j4 > $R/out/build-$n.log 2>&1; echo "$n rc=$?" >> $R/out/build.rc ) &
done
wait
touch $R/build.done
