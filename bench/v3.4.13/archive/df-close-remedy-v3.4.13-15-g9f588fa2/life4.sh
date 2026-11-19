#!/bin/bash
# usage: life4.sh <tag> <first> <last> <ops> <blocks> <only> <rows> <port> <drv> <pin>
# Four arms A, A2 (A's binary again), B, C per lifetime; arm order rotates with the lifetime number.
cd /home/louis/dfclose/run
tag=$1; first=$2; last=$3; ops=$4; blocks=$5; only=$6; rows=$7; port=$8; drv=$9; pin=${10}
TOOL=/home/louis/ckdbs/.claude/worktrees/da-q8-memory-bound-key/tools/bn_census.py
for i in $(seq $first $last); do
  out=out_${tag}_$i.json
  if [ -s $out ] && python3 -c "import json,sys; d=json.load(open('$out')); sys.exit(0 if len(d['runs'])==1 else 1)" 2>/dev/null; then
    continue
  fi
  echo "$(date -u +%FT%TZ) $tag lifetime $i start" >> status4.txt
  while pgrep -f "cc1plus|cmake --build|ctest" > /dev/null; do
    echo "$(date -u +%FT%TZ) $tag lifetime $i HELD" >> status4.txt; sleep 10
  done
  A="--arm A=$PWD/kds_server_A"; A2="--arm A2=$PWD/kds_server_A"; B="--arm B=$PWD/kds_server_B"; C="--arm C=$PWD/kds_server_C"
  case $((i % 4)) in
    0) arms=($A $A2 $B $C);;
    1) arms=($A2 $B $C $A);;
    2) arms=($B $C $A $A2);;
    3) arms=($C $A $A2 $B);;
  esac
  python3 $TOOL "${arms[@]}" --workdir $PWD/wd_${tag}_$i --port $port --pin-server $pin --pin-driver $drv \
    --cores 1 --rows $rows --scan-rows 1000 --ops $ops --blocks $blocks --runs 1 --only $only \
    --cell-quiet-pct 15 --cell-quiet-wait 60 --cell-retries 2 --cell-clean-pct 25 \
    --out $out > log_${tag}_$i.txt 2>&1
  echo "$(date -u +%FT%TZ) $tag lifetime $i exit $?" >> status4.txt
  rm -rf wd_${tag}_$i
done
echo "$(date -u +%FT%TZ) $tag ALLDONE" >> status4.txt
