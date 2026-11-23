#!/bin/bash
# CW-S0 M3: a sampled profile (tools/kds_sampler.c) of the tree at the opening
# commit built with frame pointers, at the clean cores = 2 cell. Run after the
# matrix so the build does not overlap a timed run.
set -u
W=/home/louis/bench-runs/cw
WT=/home/louis/ckdbs/.claude/worktrees/sync-stale-records
PROGRESS=$W/progress
echo "$(date -u +%T) start M3-build" >> "$PROGRESS"
mkdir -p $W/r-src
[ -f $W/r-src/CMakeLists.txt ] || tar -xf $W/spike-src.tar -C $W/r-src
s=$(date +%s)
cmake -S $W/r-src -B $W/rfp-build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS=-fno-omit-frame-pointer > $W/rfp-cmake.log 2>&1 &&
  make -C $W/rfp-build -j8 kds_server > $W/rfp-build.log 2>&1
echo "$(date -u +%T) end M3-build rc=$? wall=$(( $(date +%s) - s ))s" >> "$PROGRESS"
cp $W/rfp-build/kds_server $W/bin/Rfp
gcc -O2 -shared -fPIC -o $W/kds_sampler.so $WT/tools/kds_sampler.c
mkdir -p $W/samples
for sh in point pointN insert1; do
  echo "$(date -u +%T) start M3s-$sh-c2" >> "$PROGRESS"
  python3 $WT/tools/ba_census.py --engine kds --durability relaxed --frames 65536 --rows 10000 \
    --warmup 2 --seconds 8 --runs 1 --workdir $W/work --max-load 2.5 --max-wait 180 \
    --bin $W/bin/Rfp --port 17411 --shape $sh --cores 2 --server-cpus 0,1 \
    --client-cpus 2,3,6,7 --sessions 4 --out $W/out/M3s-$sh-c2 --sample \
    --server-env LD_PRELOAD=$W/kds_sampler.so --server-env KDS_SAMPLER_OUT=$W/samples/$sh-c2 \
    --server-env KDS_SAMPLER_HZ=1000 >> $W/driver.log 2>&1
  echo "$(date -u +%T) end M3s-$sh-c2 rc=$?" >> "$PROGRESS"
done
echo "$(date -u +%T) M3 DONE" >> "$PROGRESS"
