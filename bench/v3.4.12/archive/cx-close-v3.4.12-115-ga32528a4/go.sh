#!/bin/bash
# usage: go.sh <tag> <mode> <cores> <cpus> <port> [extra driver args]
cd /home/louis/cxclose/run
tag=$1; mode=$2; cores=$3; cpus=$4; port=$5; shift 5
nohup taskset -c 4 python3 cx_close.py --mode $mode --bin-a $PWD/kds_server_A --bin-b $PWD/kds_server_B --bin-x $PWD/kds_server_X --bin-y $PWD/kds_server_Y \
  --wd $PWD/wd$tag --json out$tag.json --status status$tag.txt --plans plans$tag.json \
  --cores $cores --server-cpu $cpus --base-port $port "$@" > driver$tag.log 2>&1 &
echo $! > pid$tag
nohup ./wd.sh $(cat pid$tag) watch$tag.txt status$tag.txt > /dev/null 2>&1 &
