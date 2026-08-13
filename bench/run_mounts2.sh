#!/usr/bin/env bash
# Intermediate cardinalities for the mount matrix: the assert-minus-noassert
# delta was non-monotonic between 200 and 10000 entries, and two more points
# say whether that is a curve or a pair of outliers. Head side only - the
# 18-cell matrix already showed head and base indistinguishable at every cell.
set -euo pipefail
W=/home/cdkbs/ckdbs/.claude/worktrees/assert-orphan-flag-format-v2
OUT=/home/cdkbs/abbench/mounts2
mkdir -p "$OUT"

for rows in 2000 5000; do
    for shape in noassert assert; do
        label="head-r$rows-$shape"
        extra=""
        [ "$shape" = assert ] && extra="--assertion"
        "$W/bench/wait_quiet.sh" > /dev/null
        # shellcheck disable=SC2086
        python3 "$W/tools/mount_cost_benchmark.py" \
            --binary "$W/build-rel-head/kds_server" --label "$label" \
            --mounts 9 --rows "$rows" --port 15491 --json "$OUT/$label.json" $extra \
            > "$OUT/$label.txt" 2>&1 || echo "FAILED $label"
        echo "  done $label"
    done
done
echo ALLDONE
