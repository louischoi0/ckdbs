#!/bin/bash
# DZ-S4's server A/B: BA's census driver, A and B alternating run by run.
# usage: server_ab.sh <tools dir> <outdir> <rounds>
tools=$1; out=$2; rounds=$3
mkdir -p "$out"
status="$out/status"
cell() {  # label bin shape durability cores
  local label=$1 bin=$2 shape=$3 dur=$4 cores=$5 round=$6
  local cpus=0; [ "$cores" -ge 2 ] && cpus=0,1
  local name="$label-$shape-$dur-k$cores-r$round"
  echo "$(date -u +%H:%M:%S) start $name load=$(cut -d' ' -f1 /proc/loadavg) compilers=$(pgrep -c -f 'cc1plus|cmake --build|ctest')" >> "$status"
  python3 -I "$tools/ba_census.py" --engine kds --shape "$shape" --durability "$dur" --cores "$cores" \
    --sessions 16 --seconds 8 --warmup 2 --runs 1 --bin "$bin" --port 15761 \
    --server-cpus "$cpus" --client-cpus 4,5,6,7 --workdir /var/tmp/dz-bench/carve/work \
    --out "$out/json-$name" --max-load 6 --max-wait 300 >> "$out/driver.log" 2>&1
  echo "$(date -u +%H:%M:%S) end $name rc=$?" >> "$status"
}
for round in $(seq 1 "$rounds"); do
  for spec in "insert1 relaxed 1" "insert1 group 2" "point group 2"; do
    set -- $spec
    cell A /var/tmp/dz-bench/carve/kds_server-a "$1" "$2" "$3" "$round"
    cell B /var/tmp/dz-bench/carve/kds_server-b "$1" "$2" "$3" "$round"
  done
done
echo "$(date -u +%H:%M:%S) DONE" >> "$status"
