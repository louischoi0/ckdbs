#!/bin/bash
# DZ-S3's premise: the relaxed carve's longest stall, BA's shape.
# usage: carve.sh <tools dir> <binary> <outdir> <port>
tools=$1; bin=$2; out=$3; port=$4
mkdir -p "$out"
status="$out/status"
echo "$(date -u +%H:%M:%S) start load=$(cut -d' ' -f1 /proc/loadavg) compilers=$(pgrep -c -f 'cc1plus|cmake --build|ctest')" >> "$status"
python3 -I "$tools/ba_census.py" --engine kds --shape insert1 --durability relaxed --cores 1 \
  --sessions 16 --seconds 8 --warmup 2 --runs 3 --bin "$bin" --port "$port" \
  --server-cpus 0 --client-cpus 4,5,6,7 --workdir /var/tmp/dz-bench/carve/work \
  --out "$out/json" --max-load 4 --max-wait 300 > "$out/driver.log" 2>&1
echo "$(date -u +%H:%M:%S) end rc=$? load=$(cut -d' ' -f1 /proc/loadavg)" >> "$status"
echo "$(date -u +%H:%M:%S) DONE" >> "$status"
