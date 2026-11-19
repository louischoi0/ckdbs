#!/bin/bash
# The DF-S1 knob spike: main (11f72ce9) + spike.diff, one binary per knob set.
cd /home/louis/cxbisect
b() { # name flags
  cmake -S srcK -B buildK$1 -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF "-DCMAKE_CXX_FLAGS=$2" > cmakeK$1.log 2>&1 \
   && cmake --build buildK$1 --target kds_server -j8 > buildK$1.log 2>&1 \
   && cp buildK$1/kds_server run/kds_server_K$1 && echo "K$1 ok $(sha256sum run/kds_server_K$1 | cut -c1-64) $(date -u +%FT%TZ)" || echo "K$1 FAILED"
}
b S "-DKDS_SPIKE_NO_SAMPLE"
b C "-DKDS_SPIKE_NO_COMPILE_NS"
b ALL "-DKDS_SPIKE_NO_SAMPLE -DKDS_SPIKE_NO_COMPILE_NS -DKDS_SPIKE_NO_COST"
echo ALLDONE
