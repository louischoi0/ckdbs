#!/usr/bin/env bash
# XF4's rerun: the same `group, pl, b=8` cell, with the per-leg timers in.
# Three repeats, so the leg means have a floor of their own.
set -uo pipefail
S=/tmp/claude-1000/-home-cdkbs-ckdbs/4e50e866-8005-43e4-b524-1d53355b0c93/scratchpad
WT=/home/cdkbs/ckdbs/.claude/worktrees/xf
ROOT=$S/bench-xf4b
XOWNER=$ROOT/xowner
OUT=$ROOT/out
DRIVER=$WT/bench/xe4_crossowner_commit_probe.py
WAITQ=$WT/bench/wait_quiet.sh
mkdir -p "$XOWNER" "$OUT"
PORT=17300
SERVER=$S/arms/kds_server-xf4base

for r in 1 2 3; do
    label=xf4-group-b8-ackdurable-r${r}
    PORT=$((PORT+1))
    rm -rf "${XOWNER:?}/$label"
    "$WAITQ"
    echo "=== $label (port $PORT) $(date -u +%FT%TZ) ===" | tee -a "$OUT/run.log"
    python3 "$DRIVER" --server "$SERVER" --workdir "$XOWNER" --label "$label" --port "$PORT" \
        --json "$OUT/$label.json" --cores 2 --durability group --concurrency 8 --txns 4000 \
        > "$OUT/$label.log" 2>&1
    rc=$?
    if [[ $rc -eq 0 ]]; then
        echo "OK $label rc=0" | tee -a "$OUT/run.log"
    else
        echo "FAILED $label rc=$rc" | tee -a "$OUT/run.log"
        tail -40 "$OUT/$label.log" | tee -a "$OUT/run.log"
        exit 1
    fi
done
echo "ALL DONE $(date -u +%FT%TZ)" | tee -a "$OUT/run.log"
