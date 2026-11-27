#!/bin/bash
cd /home/ukjin/workspaces/ckdbs/.claude/worktrees/dy-segment-roll-off-the-latch
R=/home/ukjin/bench-runs/dy-s0
CL=$(seq -s, 8 23)
cell() { # name durability arm
  n=$1; d=$2; arm=$3
  echo "$(date +%s) start $n" > $R/beat
  { echo "== $n $(date -u +%FT%TZ) load $(cat /proc/loadavg)"; pgrep -a -f "cc1plus|cmake --build|ctest|ninja" ; } >> $R/hoststamps.log
  taskset -c 0-7 python3 tools/bl_mixed.py --bin $R/bin/kds_server --port 17611 --workdir $R/work --out $R/out/$n \
    --cores 4 --seconds 30 --warmup 2 --runs 1 --max-wait 0 --max-load 4 --server-cpus 0,1,2,3 --client-cpus $CL \
    --frames 131072 --log-level info --ctl-core 0 --durability $d --arm $arm --readers 6 --rate 400 --placement spread \
    > $R/$n.console 2>&1
  rc=$?
  echo "$(date +%s) end $n rc=$rc" >> $R/beat; echo "end $n rc=$rc" >> $R/hoststamps.log
}
cell relaxed-mixed-r1 relaxed mixed
cell relaxed-iso-r1 relaxed isolated
cell relaxed-mixed-r2 relaxed mixed
cell group-mixed-r1 group mixed
cell group-iso-r1 group isolated
cell strict-mixed-r1 strict mixed
cell strict-iso-r1 strict isolated
cell relaxed-iso-r2 relaxed isolated
cell group-mixed-r2 group mixed
echo "$(date +%s) ALLDONE" >> $R/beat
