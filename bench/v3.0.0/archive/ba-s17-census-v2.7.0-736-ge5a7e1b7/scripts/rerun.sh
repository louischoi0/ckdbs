#!/bin/bash
# usage: rerun.sh <round>   moves every run of a cell holding a flagged run into quarantine/round<N>, then resumes census.sh
R=/home/cdkbs/bench-runs/ba-s17
cd $R
python3 -I screen.py kds ../ba-close/host-samples.jsonl > contaminated-round$1.txt 2> screen-round$1.err
mkdir -p quarantine/round$1
for f in $(cat contaminated-round$1.txt); do
  cell=${f%-r[0-9].json}
  for g in kds/$cell-r*.json; do
    test -f "$g" && mv "$g" quarantine/round$1/
  done
done
rm -f census-done.flag
./census.sh
echo "round $1 done $(date -u +%T)" >> rerun.log
