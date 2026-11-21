#!/bin/bash
# DG-S0 smoke: one short cell per spike arm, to check each switch acts.
WT=/home/louis/ckdbs/.claude/worktrees/dg-wal-segment-body
BIN=/home/louis/bench-runs/dg-s0/kds_server.dg-spike
OUT=/home/louis/bench-runs/dg-s0/smoke
rm -rf "$OUT"; mkdir -p "$OUT"
for SP in "" nofill "pool=32,cold" "pace=8000"; do
  D="$OUT/${SP//[=,]/_}x"; mkdir -p "$D"
  KDS_DG_SPIKE="$SP" KDS_DG_FILL_LOG="$D/fill.log" python3 "$WT/tools/bl_mixed.py" \
    --bin $BIN --port 15611 --cores 4 --readers 6 --rate 400 --seconds 4 --warmup 1 \
    --frames 65536 --log-level info --server-cpus 0,2,4,6 --client-cpus 1,3,5,7 \
    --placement kernel --runs 1 --workdir /home/louis/bench-runs/dg-s0/work \
    --durability relaxed --arm mixed --levels reader=transactional,walker=analytic,inserter=background \
    --out "$D" 2>&1 | tail -1 | cut -c1-200
  echo "[$SP] fill lines: $(grep -c '^fill' "$D/fill.log") pooled: $(grep -c '^pooled' "$D/fill.log") asks: $(grep -c '^ask' "$D/fill.log") rolls: $(grep -c 'wal] roll' "$D"/*.s.log)"
  grep -m3 '^fill\|^pool' "$D/fill.log" | tail -3
done
