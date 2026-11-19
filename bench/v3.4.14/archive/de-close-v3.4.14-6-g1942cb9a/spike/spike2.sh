#!/bin/bash
# The spike's second round: K5 = the tree as it stands (K1 + three always-on
# trims). Part V: vscan, arms A, K3, K5 rotated over two rounds. Part P: the
# point SELECTs, arms A, A2, B, K5, four lifetimes.
S=/tmp/claude-1000/-home-louis-ckdbs/783802aa-0ec8-4cfd-866b-862e378009ca/scratchpad
W=/home/louis/ckdbs/.claude/worktrees/de-page-read-off-the-reactor
BIN=$HOME/bench-runs/de/close
OUT=$HOME/bench-runs/de/spike2
cd $W
mkdir -p $OUT
echo "spike2 start $(date -u +%FT%TZ)" > $S/spike2.status
B=cmake
$B --build build-release -j8 --target kds_server > $S/build-K5.log 2>&1
echo "K5 build exit $? $(date -u +%T)" >> $S/spike2.status
cp build-release/kds_server $BIN/kds_server_K5
sha256sum $BIN/kds_server_K5 >> $S/spike2.status
sleep 45  # the build's load leaves the 1-minute average
orders=("A K3 K5" "K5 A K3")
for round in 0 1; do
  for arm in ${orders[$round]}; do
    echo "$(date -u +%T) V round $round arm $arm" >> $S/spike2.status
    python3 tools/de_premise.py --bin $BIN/kds_server_$arm --arm $arm --port 32750 \
      --workdir $HOME/bench-runs/de/work --out $OUT --keep --runs 1 --run-base $round \
      --cells vscan --cold-rate 650 --probe-reads 200 >> $S/spike2.log 2>&1 \
      || echo "$(date -u +%T) FAILED V round $round arm $arm" >> $S/spike2.status
  done
done
echo "$(date -u +%T) P start" >> $S/spike2.status
P2=$W/bench/v3.4.14/archive/de-close-v3.4.14-6-g1942cb9a/part2-and-runners/p2.py
python3 $P2 --arm A=$BIN/kds_server_A --arm A2=$HOME/bench-runs/de/p2k/kds_server_A2 \
  --arm B=$BIN/kds_server_B --arm K5=$BIN/kds_server_K5 \
  --workdir $HOME/bench-runs/de/p2k/wd --port 32760 \
  --rows 1000 --ops 12000 --blocks 24 --runs 4 --pin-server 3 --pin-driver 5 \
  --cell-quiet-pct 15 --cell-quiet-wait 60 --cell-retries 2 --cell-clean-pct 25 \
  --only select-pk,select-index --out $OUT/p2k5.json > $S/spike2-p2.log 2>&1
echo "spike2 exit $? $(date -u +%FT%TZ)" >> $S/spike2.status
