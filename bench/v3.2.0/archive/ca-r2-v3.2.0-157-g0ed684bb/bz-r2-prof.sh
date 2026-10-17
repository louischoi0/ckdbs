#!/bin/bash
# CA round 2 Part B: sampled c13, s3 (ops 3000) and s4 (off + on arms sampled, session 20 s).
H=/home/cdkbs/ckdbs/.claude/worktrees/bz-background-advisory-optimizer/bench/v3.2.0/archive/ca-r2-v3.2.0-157-g0ed684bb
OUT=$1; B=/home/cdkbs/bench-runs/bz
export S3_ARGS="--loans 10000 --index-mode all --server-indexes on --seed 1 --assert-index-reads --ops 3000"
export S4_ARGS="--days 2 --blocks 4 --session-seconds 20 --overnight-seconds 6"
export SAMPLER=$B/bin/kds_sampler.so KDS_SAMPLER_HZ=500
mkdir -p "$OUT"
st() { echo "$(date -u +%FT%TZ) $*" > "$OUT/status"; echo "$(date -u +%FT%TZ) $*" >> "$OUT/timeline"; }
export SCEN="3"; st "sampled c13 s3"; "$H/quad-b.sh" $B/kds_server.c13 prof-c13-s3 "$OUT" 1
export SCEN="4"; st "sampled c13 s4"; "$H/quad-b.sh" $B/kds_server.c13 prof-c13-s4 "$OUT" 1
st "DONE"; touch "$OUT/done"
