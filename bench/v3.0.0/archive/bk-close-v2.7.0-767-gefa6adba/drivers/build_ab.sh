#!/bin/bash
# Builds A (5979b5e7) and B (efa6adba) in Release, each in its own build dir under the scratchpad.
# Run niced and confined to CPUs 0-5 so another session's benchmark on CPUs 6,7 is not disturbed.
SP=/tmp/claude-1000/-home-cdkbs-ckdbs/00c1c148-cc8f-4b5f-ac05-d5dc4f399e1d/scratchpad
for X in A B; do
  (
    date -u +"%F %T start $X"
    cmake -S $SP/bk-$X -B $SP/build-$X -DCMAKE_BUILD_TYPE=Release -DKDS_WITH_TLS=ON -DKDS_BUILD_SIM=ON \
          -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_BENCH=OFF > $SP/build-$X.configure.log 2>&1 || { echo "configure $X failed"; exit 1; }
    nice -n 19 taskset -c 0-5 cmake --build $SP/build-$X --target kds_server -j3 > $SP/build-$X.log 2>&1
    echo "build $X exit $?"
    date -u +"%F %T end $X"
  ) > $SP/build-$X.status 2>&1 &
done
wait
touch $SP/build.done
