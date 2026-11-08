#!/bin/bash
# A few-second smoke of the launcher and every S0 driver against one base
# server. Not a measurement: tmpfs is fine here, nothing is recorded.
set -u
SP=/tmp/claude-1000/-home-louis-ckdbs/1c794fd2-a838-4425-a96a-e3b26a73757b/scratchpad/cp-s0
WT=/home/louis/ckdbs/.claude/worktrees/cp-lock-model-exploration
SM=/home/louis/bench-runs/cp-s0/smoke-$(date -u +%H%M%S)
mkdir -p "$SM"
cd "$WT"
python3 "$SP/m1m4/srv.py" start --binary "$SP/bin/kds_server-base" --workdir "$SM/srv" --port 15590 \
  --debug-text-port 15591 --cores 4 --taskset 2-5 --durability group --label smoke 2>&1 | tail -2
echo "--- release_order"
timeout 60 python3 tools/cp_s0_release_order.py --port 15591 --protocol text --arm group --iterations 8 --warmup 2 --json "$SM/ro.json" 2>&1 | tail -25
echo "--- fence"
timeout 60 python3 tools/cp_s0_fence.py --port 15590 --protocol kwp --rows 256 --writers 1 --ops 30 --blocks 1 --json "$SM/fence.json" 2>&1 | tail -15
echo "--- read_views"
timeout 60 python3 "$SP/m3/read_views.py" --port 15590 --out "$SM/views" --label smoke --tools "$WT/tools" 2>&1 | tail -15
echo "--- lock_contention"
timeout 60 python3 tools/lock_contention_benchmark.py --port 15590 --sessions 2 --rows 16 --ops 20 --blocks 2 --json "$SM/lc.json" 2>&1 | tail -12
echo "--- stop"
python3 "$SP/m1m4/srv.py" stop --workdir "$SM/srv" 2>&1 | tail -2
