#!/bin/bash
# usage: lifek.sh <tag> <first> <last> <ops> <blocks> <only> <rows> <port> <drv> <pin> <labels...>
# One lifetime per index: every label is an arm (A2 is A's binary again); the arm order rotates by the lifetime number.
cd /home/louis/dfclose/run
tag=$1; first=$2; last=$3; ops=$4; blocks=$5; only=$6; rows=$7; port=$8; drv=$9; pin=${10}
shift 10
labels=("$@")
n=${#labels[@]}
TOOL=/home/louis/ckdbs/.claude/worktrees/da-q8-memory-bound-key/tools/bn_census.py
for i in $(seq $first $last); do
  out=out_${tag}_$i.json
  if [ -s $out ] && python3 -c "import json,sys; d=json.load(open('$out')); sys.exit(0 if len(d['runs'])==1 else 1)" 2>/dev/null; then
    continue
  fi
  echo "$(date -u +%FT%TZ) $tag lifetime $i start" >> statusk.txt
  while pgrep -f "cc1plus|cmake --build|ctest" > /dev/null; do
    echo "$(date -u +%FT%TZ) $tag lifetime $i HELD" >> statusk.txt; sleep 10
  done
  arms=()
  for j in $(seq 0 $((n-1))); do
    l=${labels[$(( (j + i) % n ))]}
    b=$l; [ $l = A2 ] && b=A
    arms+=(--arm $l=$PWD/kds_server_$b)
  done
  python3 $TOOL "${arms[@]}" --workdir $PWD/wd_${tag}_$i --port $port --pin-server $pin --pin-driver $drv \
    --cores 1 --rows $rows --scan-rows 1000 --ops $ops --blocks $blocks --runs 1 --only $only \
    --cell-quiet-pct 15 --cell-quiet-wait 60 --cell-retries 2 --cell-clean-pct 25 \
    --out $out > log_${tag}_$i.txt 2>&1
  echo "$(date -u +%FT%TZ) $tag lifetime $i exit $?" >> statusk.txt
  rm -rf wd_${tag}_$i
done
echo "$(date -u +%FT%TZ) $tag ALLDONE" >> statusk.txt
