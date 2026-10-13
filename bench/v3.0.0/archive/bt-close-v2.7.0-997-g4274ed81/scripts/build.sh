#!/bin/sh
# BT close: Release kds_server for B (4274ed81), built from git archive.
R=/home/cdkbs/bench-runs/bt-close
rm -f $R/build.flag
{
  date -u '+start %F %T'
  cmake -S $R/src -B $R/rel -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > $R/cmake.log 2>&1 || echo CMAKE_FAIL
  if cmake --build $R/rel -j8 --target kds_server > $R/build-B.log 2>&1; then
    cp $R/rel/kds_server $R/bin/kds_server-B && sha256sum $R/bin/kds_server-B
    date -u '+built B %F %T'
  else
    echo BUILD_FAIL
  fi
  date -u '+end %F %T'
} > $R/build.out 2>&1
echo FIN > $R/build.flag
