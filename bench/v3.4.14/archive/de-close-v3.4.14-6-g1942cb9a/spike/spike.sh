#!/bin/bash
# The close's knob spike. Part V: the resident scan cell (vscan), arms A, B
# and K1 rotated over two rounds. Part P: the resident point SELECTs through
# the close's own wrapper, arms A, A2, B, K1, K2, K3, four server lifetimes.
S=/tmp/claude-1000/-home-louis-ckdbs/783802aa-0ec8-4cfd-866b-862e378009ca/scratchpad
W=/home/louis/ckdbs/.claude/worktrees/de-page-read-off-the-reactor
BIN=$HOME/bench-runs/de/close
OUT=$HOME/bench-runs/de/spike
cd $W
mkdir -p $OUT
echo "spike start $(date -u +%FT%TZ)" > $S/spike.status
orders=("A B K1" "K1 A B" "B K1 A")
for round in 0 1 2; do
  for arm in ${orders[$round]}; do
    echo "$(date -u +%T) V round $round arm $arm" >> $S/spike.status
    python3 tools/de_premise.py --bin $BIN/kds_server_$arm --arm $arm --port 32750 \
      --workdir $HOME/bench-runs/de/work --out $OUT --keep --runs 1 --run-base $round \
      --cells vscan --cold-rate 650 --probe-reads 200 >> $S/spike.log 2>&1 \
      || echo "$(date -u +%T) FAILED V round $round arm $arm" >> $S/spike.status
  done
done
echo "$(date -u +%T) P start" >> $S/spike.status
P2=$W/bench/v3.4.14/archive/de-close-v3.4.14-6-g1942cb9a/part2-and-runners/p2.py
mkdir -p $HOME/bench-runs/de/p2k
cp $BIN/kds_server_A $HOME/bench-runs/de/p2k/kds_server_A2
python3 $P2 --arm A=$BIN/kds_server_A --arm A2=$HOME/bench-runs/de/p2k/kds_server_A2 \
  --arm B=$BIN/kds_server_B --arm K1=$BIN/kds_server_K1 --arm K2=$BIN/kds_server_K2 \
  --arm K3=$BIN/kds_server_K3 --workdir $HOME/bench-runs/de/p2k/wd --port 32760 \
  --rows 1000 --ops 12000 --blocks 24 --runs 4 --pin-server 3 --pin-driver 5 \
  --cell-quiet-pct 15 --cell-quiet-wait 60 --cell-retries 2 --cell-clean-pct 25 \
  --only select-pk,select-index --out $OUT/p2k.json > $S/spike-p2.log 2>&1
echo "spike exit $? $(date -u +%FT%TZ)" >> $S/spike.status
