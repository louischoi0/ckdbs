#!/bin/sh
# BL-S1 skew cells, CPU-bound variant: a read-only point shape with Waystone
# recording off, so no statement appends to the WAL and a core's load is its
# CPU alone - the premise placement by load acts on, without the log coupling
# the pkmix cells carry. Waits for WAITFOR. Usage: skew_cpu.sh OUTDIR RUNS WAITFOR
OUT=$1; RUNS=${2:-3}; WAITFOR=$3
TOOLS=/home/cdkbs/ckdbs/.claude/worktrees/bl-session-balancer/tools
BIN=/home/cdkbs/bench-runs/bl/bin/kds_server.3552a03c
if [ -n "$WAITFOR" ]; then while [ ! -f "$WAITFOR" ]; do sleep 10; done; fi
mkdir -p "$OUT"
for r in $(seq 1 "$RUNS"); do
  for cell in "2 4 0,2 4,5,6,7" "4 8 0,2,4,6 1,3,5,7" "2 8 0,2 4,5,6,7"; do
    set -- $cell
    for arm in kernel balanced skew; do
      /home/cdkbs/bench-runs/bl/quiet_gate.sh; python3 $TOOLS/ba_census.py --bin $BIN --port 17443 --shape point --durability relaxed \
        --cores $1 --sessions $2 --server-cpus $3 --client-cpus $4 --placement $arm \
        --extra-conf "waystone_recording = off" \
        --runs 1 --seconds 8 --warmup 2 --rows 16000 --max-load 3.0 --max-wait 15 \
        --workdir /home/cdkbs/bench-runs/bl/work-skewcpu --out "$OUT/r$r" >> "$OUT/log.txt" 2>&1
    done
  done
done
touch "$OUT/done"
