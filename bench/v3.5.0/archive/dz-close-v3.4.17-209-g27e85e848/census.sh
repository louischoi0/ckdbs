#!/bin/bash
# DZ-S0 census driver: sizes x readers x rounds, one process a cell, the
# binaries interleaved within a cell. usage:
#   census.sh <outdir> <rounds> "<label>=<binary> ..." <sizes...>
out=$1; rounds=$2; bins=$3; shift 3
mkdir -p "$out"
status="$out/status"
for round in $(seq 1 "$rounds"); do
  for frames in "$@"; do
    for readers in 0 3; do
      for lb in $bins; do
        label=${lb%%=*}; bin=${lb#*=}
        cell="${label}-r${round}-f${frames}-rd${readers}"
        echo "$(date -u +%H:%M:%S) start $cell load=$(cut -d' ' -f1 /proc/loadavg) compilers=$(pgrep -c -f "cc1plus|cmake --build|ctest")" >> "$status"
        rm -rf /var/tmp/dz-bench/d.kds /var/tmp/dz-bench/d.kds.wal
        timeout 600 taskset -c 0-7 "$bin" --frames "$frames" --readers "$readers" \
          --file /var/tmp/dz-bench/d.kds > "$out/$cell.txt" 2>&1
        echo "$(date -u +%H:%M:%S) end $cell rc=$? load=$(cut -d' ' -f1 /proc/loadavg)" >> "$status"
      done
    done
  done
done
rm -rf /var/tmp/dz-bench/d.kds /var/tmp/dz-bench/d.kds.wal
echo "$(date -u +%H:%M:%S) DONE" >> "$status"
