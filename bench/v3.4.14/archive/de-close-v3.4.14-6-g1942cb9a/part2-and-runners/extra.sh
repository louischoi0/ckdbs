#!/bin/bash
S=/tmp/claude-1000/-home-louis-ckdbs/783802aa-0ec8-4cfd-866b-862e378009ca/scratchpad
W=/home/louis/ckdbs/.claude/worktrees/de-page-read-off-the-reactor
BIN=$HOME/bench-runs/de/close
OUT=$W/bench/v3.4.14/archive/de-close-v3.4.14-6-g1942cb9a
cd $W
echo "extra start $(date -u +%FT%TZ)" > $S/extra.status
for round in 2 3; do
  for cell in iii iv; do
    if [ $((round % 2)) -eq 0 ]; then arms="A B"; else arms="B A"; fi
    for arm in $arms; do
      echo "$(date -u +%T) round $round cell $cell arm $arm" >> $S/extra.status
      python3 tools/de_premise.py --bin $BIN/kds_server_$arm --arm $arm --port 32750 --workdir $HOME/bench-runs/de/work --out $OUT --keep --runs 1 --run-base $round --cells $cell --cold-rate 650 --probe-reads 200 >> $S/close.log 2>&1
      rc=$?; [ $rc -ne 0 ] && echo "FAILED rc=$rc $round $cell $arm" >> $S/extra.status
    done
  done
done
echo "extra exit $(date -u +%FT%TZ)" >> $S/extra.status
