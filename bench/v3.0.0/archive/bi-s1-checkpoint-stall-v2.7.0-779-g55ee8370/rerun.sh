#!/bin/sh
# The twelfth cell again: c4-relaxed-r1 lost a load worker to a refused
# retry (docs/inflight/bugs/a-rollback-whose-pk-descent-is-refused-leaves-its-keys-bound.md)
# before the driver counted such duplicates rather than dying.
DIR=/home/cdkbs/bench-runs/bi-s1-v2.7.0-779-g55ee8370
DRIVER=/home/cdkbs/ckdbs/.claude/worktrees/bi-checkpoint-off-the-reactor/tools/bi_checkpoint_stall.py
rm -rf "$DIR/work/c4-relaxed-r1.db" "$DIR/work/c4-relaxed-r1.db.wal"
python3 "$DRIVER" --server "$DIR/kds_server" --workdir "$DIR/work" --cores 4 --durability relaxed \
  --seconds 600 --tag c4-relaxed-r1 --out "$DIR/cells/c4-relaxed-r1.json" >> "$DIR/run.log" 2>&1
echo "done c4-relaxed-r1 (rerun) rc=$?" >> "$DIR/run.log"
rm -rf "$DIR/work/c4-relaxed-r1.db" "$DIR/work/c4-relaxed-r1.db.wal"
echo RERUNDONE >> "$DIR/run.log"
