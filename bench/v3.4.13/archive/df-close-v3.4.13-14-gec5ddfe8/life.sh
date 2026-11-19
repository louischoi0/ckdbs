#!/bin/bash
# usage: life.sh <tag> <first> <last> <cores> <pin-server> <ops> <blocks> <only> <rows> <port>
# One bn_census invocation (--runs 1: one server lifetime of every arm) per index, skipped if its
# output is complete, so a re-run of this script resumes. Status line per lifetime in status.txt.
cd /home/louis/dfclose/run
tag=$1; first=$2; last=$3; cores=$4; pin=$5; ops=$6; blocks=$7; only=$8; rows=$9; port=${10}; drv=${11}
TOOL=/home/louis/ckdbs/.claude/worktrees/da-q8-memory-bound-key/tools/bn_census.py
for i in $(seq $first $last); do
  out=out_${tag}_$i.json
  if [ -s $out ] && python3 -c "import json,sys; d=json.load(open('$out')); sys.exit(0 if len(d['runs'])==1 else 1)" 2>/dev/null; then
    continue
  fi
  echo "$(date -u +%FT%TZ) $tag lifetime $i start" >> status.txt
  while pgrep -f "cc1plus|cmake --build|ctest" > /dev/null; do
    echo "$(date -u +%FT%TZ) $tag lifetime $i HELD by build/test" >> status.txt; sleep 10
  done
  # A rotates by lifetime: --runs 1 always starts rotation at run 0, so the arm order is
  # rotated here by the order of the --arm flags.
  case $((i % 3)) in
    0) arms=(--arm A=$PWD/kds_server_A --arm A2=$PWD/kds_server_A --arm B=$PWD/kds_server_B);;
    1) arms=(--arm A2=$PWD/kds_server_A --arm B=$PWD/kds_server_B --arm A=$PWD/kds_server_A);;
    2) arms=(--arm B=$PWD/kds_server_B --arm A=$PWD/kds_server_A --arm A2=$PWD/kds_server_A);;
  esac
  python3 $TOOL "${arms[@]}" --workdir $PWD/wd_${tag}_$i --port $port --pin-server $pin --pin-driver $drv \
    --cores $cores --rows $rows --scan-rows 1000 --ops $ops --blocks $blocks --runs 1 --only $only \
    --cell-quiet-pct 15 --cell-quiet-wait 60 --cell-retries 2 --cell-clean-pct 25 \
    --out $out > log_${tag}_$i.txt 2>&1
  echo "$(date -u +%FT%TZ) $tag lifetime $i exit $?" >> status.txt
  rm -rf wd_${tag}_$i
done
echo "$(date -u +%FT%TZ) $tag ALLDONE" >> status.txt
