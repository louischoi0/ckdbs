#!/bin/bash
# usage: run_all.sh BIN_A BIN_B WORKDIR OUTDIR PROGRESS
A=$1; B=$2; W=$3; O=$4; P=$5
D=$(dirname "$0")
for w in $(seq 1 ${GATE:-0}); do [ "$(pgrep -c ctest)" = 0 ] && break; sleep 1; done
mkdir -p "$W" "$O"
for run in ${RUNS:-0 1}; do
  for n in 200 1000 10000 100000; do
    python3 -I "$D/r100_ab.py" --arm A=$A --arm Ap=$A --arm B=$B --pg-port 15434 \
      --rows $n --workdir "$W" --ops 400 --blocks 20 --run $run \
      --progress "$P" --out "$O/r100-n$n-run$run.json" > "$O/r100-n$n-run$run.log" 2>&1
    echo "n=$n run=$run rc=$?" >> "$O/status.txt"
  done
done
echo ALLDONE >> "$O/status.txt"; rm -f "$P.gate"
