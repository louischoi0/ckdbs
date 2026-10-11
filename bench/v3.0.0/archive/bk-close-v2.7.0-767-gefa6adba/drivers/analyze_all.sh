#!/bin/bash
# Run every analysis the document needs. Output: run/analysis/*.md and *.json
SP=/tmp/claude-1000/-home-cdkbs-ckdbs/00c1c148-cc8f-4b5f-ac05-d5dc4f399e1d/scratchpad
cd $SP/run
mkdir -p analysis
A="python3 $SP/analyze_bk.py"
$A null-c1 1 --new-is-old --label "null pair, " --json analysis/null.json --no-tables > analysis/null.md
$A overhead-c1 1 --runs 0:16 --label "pass 1, " --json analysis/c1p1.json --null-json analysis/null.json --no-tables > analysis/c1p1.md
$A overhead-c1 1 --runs 16:32 --label "pass 2, " --json analysis/c1p2.json --null-json analysis/null.json --no-tables > analysis/c1p2.md
$A overhead-c1 1 --label "" --json analysis/c1.json --null-json analysis/null.json > analysis/c1.md
$A null-c2 2 --new-is-old --label "null pair, " --json analysis/null2.json --no-tables > analysis/null2.md
$A overhead-c2 2 --label "" --json analysis/c2.json --null-json analysis/null2.json > analysis/c2.md
if [ -d strict-c1 ]; then $A strict-c1 1 --label "strict, " --json analysis/strict.json > analysis/strict.md; fi
if [ -d flip-c1 ]; then $A flip-c1 1 --flipped --rows 1000,10000 --label "flipped pair, " --json analysis/flip.json --no-tables > analysis/flip.md; fi
if [ -d fresh-c1 ]; then $A fresh-c1 1 --rows 1000,10000 --label "fresh copies (A copy 3 against B copy 2), " --json analysis/fresh.json > analysis/fresh.md; fi
if [ -d nullB-c1 ]; then $A nullB-c1 1 --null-new --rows 10000 --label "copy control, B bytes in both slots: " --json analysis/nullB.json > analysis/nullB.md; fi
if [ -d nullA-c1 ]; then $A nullA-c1 1 --new-is-old --rows 10000 --label "copy control, A bytes in both slots: " --json analysis/nullA.json > analysis/nullA.md; fi
echo analysis done
