#!/bin/sh
# Renders every table of the results file into $T/ (pinned away from the measurement's CPUs).
WT=/home/cdkbs/ckdbs/.claude/worktrees/bm-scan-kernel-open
R=/home/cdkbs/bench-runs/bm-s6
T=/tmp/claude-1000/-home-cdkbs-ckdbs/cf0ad77a-b48f-47e8-b714-576faa5dc6c9/scratchpad/bm-s6/tables
BASE=$WT/bench/v3.0.0/archive/bm-s1-scan-row-v2.7.0-942-gf5a7efb2/census.json.gz
mkdir -p $T
C="taskset -c 0,1 python3 $WT/tools/bm_close_table.py"
for s in net delta rate guard point hosts digest lookups pct; do
    $C census $R/census/census.json --base $BASE --section $s > $T/census-$s.md 2>&1
done
PGS=""
if [ -f $R/pg/pg-b.json ]; then PGS="--pg $R/pg/pg-run0.json,$R/pg/pg-b.json"; else PGS="--pg $R/pg/pg-run0.json"; fi
$C census $R/census/census.json $PGS --section pg > $T/census-pg.md 2>&1
$C bn 1000=$R/bn1000/bn.json --section guard > $T/bn1000-guard.md 2>&1
$C bn 1000=$R/bn1000/bn.json --section hosts > $T/bn1000-hosts.md 2>&1
$C bn 1000=$R/bn1000/bn.json --section pct > $T/bn1000-pct.md 2>&1
if [ -f $R/bnpad2/bn.json ]; then
    $C bn 1000=$R/bnpad2/bn.json --section guard > $T/bnpad2-guard.md 2>&1
    $C bn 1000=$R/bnpad2/bn.json --section hosts > $T/bnpad2-hosts.md 2>&1
fi
echo tables done
