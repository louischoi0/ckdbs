#!/bin/bash
# Stage 2: the DF-S1 knob spike, its tags' server lifetimes one after another (a fresh
# server and data file per arm and lifetime), so an arm's delta is not one process's.
# usage: run2.sh <rounds> <ops> <tags...>
cd /home/louis/cxbisect/run
rounds=$1; ops=$2; shift 2
for tag in "$@"; do
  taskset -c 4 python3 arms_bench.py --arm A=$PWD/kds_server_A --arm KALL=$PWD/kds_server_KALL --arm M=$PWD/kds_server_11f72ce9 \
    --arm X=$PWD/kds_server_X --arm KS=$PWD/kds_server_KS --arm KC=$PWD/kds_server_KC \
    --wd $PWD/wd$tag --json out$tag.json --status status2.txt --rounds $rounds --ops $ops --base-port 26751 > driver$tag.log 2>&1
  echo "$(date -u +%FT%TZ) lifetime $tag exit $?" >> run2.log
done
echo "$(date -u +%FT%TZ) ALLDONE" >> run2.log
echo "$(date -u +%FT%TZ) ALLDONE" > status2.txt
