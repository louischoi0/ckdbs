#!/bin/sh
# BT-S1: Release kds_server for A and each ablation, sequentially, into bin/.
R=/home/cdkbs/bench-runs/bt-s1
rm -f $R/build.flag
{
  date -u '+start %F %T'
  cmake -S $R/src -B $R/rel -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > $R/cmake.log 2>&1 || echo CMAKE_FAIL
  for v in A D1 D2 D3 all; do
    python3 $R/variant.py $v
    if cmake --build $R/rel -j8 --target kds_server > $R/build-$v.log 2>&1; then
      cp $R/rel/kds_server $R/bin/kds_server-$v && sha256sum $R/bin/kds_server-$v
      date -u "+built $v %F %T"
    else
      echo "BUILD_FAIL $v"
    fi
  done
  python3 $R/variant.py A
  date -u '+end %F %T'
} > $R/build.out 2>&1
echo FIN > $R/build.flag
