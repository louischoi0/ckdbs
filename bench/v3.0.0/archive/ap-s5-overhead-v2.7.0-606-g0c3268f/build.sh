#!/bin/bash
R=/home/cdkbs/bench-runs/ap-s5
for X in A B; do
  cd $R/src-$X
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > ../cfg-$X.log 2>&1 && cmake --build build --target kds_server -j6 > ../build-$X.log 2>&1
  echo "exit $?" >> ../build-$X.log
  stat -c '%y' build/kds_server > ../mtime-$X.txt
done
echo done > $R/build.done
