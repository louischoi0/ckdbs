#!/bin/bash
cd /home/cdkbs/bench-runs/az-s7b
S=/home/cdkbs/ckdbs/.claude/worktrees/az-s7-close/bench/v3.0.0/archive/az-s7-overhead-v2.7.0-566-g273416c
for f in t1.py t1b.py t2b.py t3.py t3b.py gen.py; do
  sed -e 's#bench-runs/az-s7#bench-runs/az-s7b#; s/(1,2,3)/(1,2,3,4,5)/g; s/(1, 2, 3)/(1, 2, 3, 4, 5)/g; s/273416c/9f170b1/g' $S/$f > $f
done
grep -n "for r in\|(1, 2\|(1,2" t3.py t3b.py gen.py
for d in c3-run*; do gzip -k $d/result.json; done
cat all.out
tail -3 c3-run5/driver.txt
grep -h "driver exit" */driver.txt | sort | uniq -c
cat */host.txt | grep -v precell | head
