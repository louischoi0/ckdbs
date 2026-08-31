#!/usr/bin/env bash
# XF3 — the confirming repeat of XE §4.3's `group, pl, b=8` pair.
#
# Differences from XE's own harness (`run_xe4_matrix2.sh`), each deliberate
# and each stated in the results addendum:
#   - arms are **interleaved** (base r1, xe1 r1, base r2, ...) rather than
#     run as two per-arm blocks, per CLAUDE.md's interleaved-A/B rule;
#   - three repeats rather than one, because this host is not XE's host and
#     a delta with no floor of its own is not readable.
set -uo pipefail
S=/tmp/claude-1000/-home-cdkbs-ckdbs/4e50e866-8005-43e4-b524-1d53355b0c93/scratchpad
WT=/home/cdkbs/ckdbs/.claude/worktrees/xf
ROOT=$S/bench-xf
XOWNER=$ROOT/xowner
OUT=$ROOT/out
DRIVER_XO=$WT/bench/xe4_crossowner_commit_probe.py
WAITQ=$WT/bench/wait_quiet.sh
mkdir -p "$XOWNER" "$OUT"

PORT=17100

declare -A SERVER
SERVER[base]=$S/arms/kds_server-base
SERVER[xe1]=$S/arms/kds_server-xe1

run_xo_cell() {
    local label=$1; shift
    local server=$1; shift
    PORT=$((PORT+1))
    rm -rf "${XOWNER:?}/$label"
    "$WAITQ"
    echo "=== $label (port $PORT) $(date -u +%FT%TZ) ===" | tee -a "$OUT/run.log"
    python3 "$DRIVER_XO" --server "$server" --workdir "$XOWNER" --label "$label" --port "$PORT" \
        --json "$OUT/$label.json" "$@" > "$OUT/$label.log" 2>&1
    rc=$?
    if [[ $rc -eq 0 ]]; then
        echo "OK $label rc=0" | tee -a "$OUT/run.log"
        return 0
    else
        echo "FAILED $label rc=$rc" | tee -a "$OUT/run.log"
        tail -40 "$OUT/$label.log" | tee -a "$OUT/run.log"
        return 1
    fi
}

for r in 1 2 3; do
    for arm in base xe1; do
        run_xo_cell xf3-group-b8-${arm}-r${r} "${SERVER[$arm]}" \
            --cores 2 --durability group --concurrency 8 --txns 4000 || exit 1
    done
done

echo "ALL DONE $(date -u +%FT%TZ)" | tee -a "$OUT/run.log"
