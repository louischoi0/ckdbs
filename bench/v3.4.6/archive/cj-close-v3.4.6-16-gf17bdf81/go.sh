#!/bin/bash
# usage: go.sh outname cell rows runs ops blocks
S=/tmp/claude-1000/-home-louis-ckdbs/c0ca10d8-b854-48ed-840b-6fe2f0acb530/scratchpad
R=/home/louis/bench-runs/cj-close
N=$1
exec taskset -c 6 python3 -I $S/cj_overhead.py --cell $2 --rows $3 --runs $4 --ops $5 --blocks $6 \
  --bin-a $R/bin/kds_server-a --bin-b $R/bin/kds_server-b --bin-c $R/bin/kds_server-c \
  --workdir $R/work-$N --out $S/out-$N --status $S/status-$N.txt > $S/log-$N.txt 2>&1
