#!/bin/bash
# Scenario 3 (library) at 9a0525d. No v3 predecessor. Row-set axis 200/1000/10000
# (the driver's documented sweep), index-mode none vs all, cores 1 and 8,
# durability group (the load is the only write path; reads are durability-blind).
set -uo pipefail
P=${P:-}
source /home/cdkbs/bench-runs/rebaseline-9a0525d/lib.sh
PORT=15620
cell3() {  # tag cores durability loans mode
    TAG=$1; CORES=$2; DUR=$3; LOANS=$4; MODE=$5
    echo "== $TAG start $(date -u +%FT%TZ) ==" >> "$RUN/timeline"
    precheck "$TAG"
    if ! start_server "$TAG" "$PORT" "$CORES" "$DUR" >> "$RUN/logs/$TAG.startup" 2>&1; then
        echo "== $TAG SERVER FAILED ==" >> "$RUN/timeline"; return; fi
    EXTRA=""
    [ "$MODE" != none ] && EXTRA="--assert-index-reads"
    python3 "$TOOLS/scenario3_library.py" --port "$PORT" --suffix ${TAG//-/_} --loans "$LOANS" \
        --index-mode "$MODE" --server-indexes on --seed 1 $EXTRA \
        --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    stop_server "$TAG" >> "$RUN/logs/$TAG.startup" 2>&1
    echo "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) ==" >> "$RUN/timeline"
}
echo "S3 START $(date -u +%FT%TZ)" >> "$RUN/timeline"
for C in 1 8; do
  for N in 200 1000 10000; do
    for M in none all; do
      cell3 ${P}s3-c$C-n$N-$M $C group $N $M
    done
  done
done
echo "S3 END $(date -u +%FT%TZ)" >> "$RUN/timeline"
touch "$RUN/${P}s3.done"
