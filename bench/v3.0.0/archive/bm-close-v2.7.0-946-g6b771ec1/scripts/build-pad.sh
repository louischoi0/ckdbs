#!/bin/sh
# A layout probe: A's source (fa80da9d, the src-A export) with every function aligned to 64 bytes.
SP=/tmp/claude-1000/-home-cdkbs-ckdbs/cf0ad77a-b48f-47e8-b714-576faa5dc6c9/scratchpad/bm-s6
cd "$SP" || exit 1
rm -f pad.done pad.failed
echo "=== build Apad $(date -u +%H:%M:%S)"
cmake -S "$SP/src-A" -B "$SP/build-Apad" -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF "-DCMAKE_CXX_FLAGS=-falign-functions=64" > "$SP/cmake-Apad.log" 2>&1 || { touch pad.failed; exit 1; }
nice -n 5 cmake --build "$SP/build-Apad" --target kds_server -j8 > "$SP/make-Apad.log" 2>&1 || { touch pad.failed; exit 1; }
echo "=== done Apad $(date -u +%H:%M:%S)"
cp "$SP/build-Apad/kds_server" /home/cdkbs/bench-runs/bm-s6/bin/kds_server-Apad
sha256sum /home/cdkbs/bench-runs/bm-s6/bin/kds_server-Apad
stat -c '%y %s' "$SP/build-Apad/kds_server"
touch pad.done
