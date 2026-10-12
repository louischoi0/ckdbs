#!/bin/sh
# BI-S5's measurement (workorder-bi-checkpoint-off-the-reactor.md §5.2): B,
# the build-release of ec827a19, over §5.1's twelve cells with a 30 s
# idle-core tail each; then A (55ee8370) on the two cores = 4 shapes with the
# same tail, for the idle-core reading. A frozen copy of the driver.
DIR=/home/cdkbs/bench-runs/bi-s5-v2.7.0-784-gec827a19
mkdir -p "$DIR/work" "$DIR/cells"
cell() {  # bin tag cores durability
  python3 "$DIR/driver.py" --server "$DIR/kds_server_$1" --workdir "$DIR/work" \
    --cores "$3" --durability "$4" --seconds 600 --idle-seconds 30 \
    --tag "$2" --out "$DIR/cells/$2.json" >> "$DIR/run.log" 2>&1
  echo "done $2 rc=$?" >> "$DIR/run.log"
  rm -rf "$DIR/work/$2.db" "$DIR/work/$2.db.wal"
}
for run in 1 2 3; do
  cell B "B-c1-relaxed-r$run" 1 relaxed
  cell B "B-c4-relaxed-r$run" 4 relaxed
  cell B "B-c1-group-r$run" 1 group
  cell B "B-c4-group-r$run" 4 group
done
cell A "A-c4-relaxed-idle" 4 relaxed
cell A "A-c4-group-idle" 4 group
echo ALLDONE >> "$DIR/run.log"
