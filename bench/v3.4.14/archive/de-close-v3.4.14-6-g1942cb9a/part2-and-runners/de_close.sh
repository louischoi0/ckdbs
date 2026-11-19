#!/bin/bash
# DE's close: DE-S0's cells, A (35086008, the commit DE opened from) against
# B (the closing code commit), interleaved: one cell, both arms back to back,
# the order of the arms swapped each round. One relation, loaded at DE-S0 and
# kept (`--keep`); the cold sessions' rate fixed for both arms.
# usage: de_close.sh <out-dir> <rounds> [cells]
S=/tmp/claude-1000/-home-louis-ckdbs/783802aa-0ec8-4cfd-866b-862e378009ca/scratchpad
W=/home/louis/ckdbs/.claude/worktrees/de-page-read-off-the-reactor
BIN=$HOME/bench-runs/de/close
OUT=$1
ROUNDS=${2:-2}
CELLS=${3:-"i ii iii iv v vscan iii4"}
cd $W
mkdir -p $OUT
echo "close start $(date -u +%FT%TZ) rounds=$ROUNDS cells=$CELLS" > $S/close.status
for round in $(seq 0 $((ROUNDS - 1))); do
  for cell in $CELLS; do
    if [ $((round % 2)) -eq 0 ]; then arms="A B"; else arms="B A"; fi
    for arm in $arms; do
      echo "$(date -u +%T) round $round cell $cell arm $arm" >> $S/close.status
      python3 tools/de_premise.py --bin $BIN/kds_server_$arm --arm $arm --port 32750 \
        --workdir $HOME/bench-runs/de/work --out $OUT --keep --runs 1 --run-base $round \
        --cells $cell --cold-rate 650 --probe-reads 200 >> $S/close.log 2>&1
      rc=$?
      [ $rc -ne 0 ] && echo "$(date -u +%T) FAILED rc=$rc round $round cell $cell arm $arm" >> $S/close.status
    done
  done
done
echo "close exit $(date -u +%FT%TZ) files=$(ls $OUT/de-*.json 2>/dev/null | wc -l)" >> $S/close.status
