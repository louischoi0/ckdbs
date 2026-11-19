#!/bin/bash
# usage: go.sh <tag> <rounds> <ops> <port> <arm specs...>   (arm: LABEL=binary-name-in-this-directory[:conf])
cd /home/louis/cxbisect/run
tag=$1; rounds=$2; ops=$3; port=$4; shift 4
arms=()
for s in "$@"; do l=${s%%=*}; r=${s#*=}; arms+=(--arm "$l=$PWD/$r"); done
nohup taskset -c 4 python3 arms_bench.py "${arms[@]}" --wd $PWD/wd$tag --json out$tag.json --status status$tag.txt \
  --rounds $rounds --ops $ops --base-port $port > driver$tag.log 2>&1 &
echo $! > pid$tag
nohup /home/louis/cxclose/run/wd.sh $(cat pid$tag) watch$tag.txt status$tag.txt > /dev/null 2>&1 &
