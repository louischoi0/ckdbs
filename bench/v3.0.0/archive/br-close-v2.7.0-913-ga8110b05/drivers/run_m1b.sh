#!/bin/bash
# BR close, M1b: a base backup running (binary B, archive off). Cells: cores = 1 with the server on CPUs 0,2,4,6 (reactor on 4),
# cores = 1 with the server's other threads kept off the reactor's CPU (server mask 0,2,6, reactor_cpus = 4), cores = 4 on
# CPUs 0,2,4,6 (reactors 4,2,6,0). Driver on CPU 7.
R=/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05
P=/tmp/claude-1000/-home-cdkbs-ckdbs/f6bce74e-e48d-4d48-817a-4e239db00c8e/scratchpad/s10-progress.txt
cd $R
mkdir -p $R/out/m1b $R/logs
rm -f $R/m1b.done
python3 -I $R/drivers/sampler.py $R/logs/host-samples-m1b.jsonl &
SAMPLER=$!
cell() {  # cores taskset tag
  echo "$(date -u +%H:%M) M1b cores $1 taskset $2 tag '$3' start load $(cut -d' ' -f1-3 /proc/loadavg)" >> $P
  taskset -c 7 python3 $R/drivers/br_cost.py backup --cores-one $1 --taskset $2 --tag "$3" --ballast-rows ${BALLAST:-540000} --reps ${REPS:-5} --window ${WINDOW:-6} --port 31821 --out $R/out/m1b --progress $P > $R/logs/m1b-c$1$3.log 2>&1
  echo "$(date -u +%H:%M) M1b cores $1 tag '$3' end rc=$? load $(cut -d' ' -f1-3 /proc/loadavg)" >> $P
}
cell 1 0,2,4,6 ""
cell 1 0,2,6 "-offcpu"
cell 4 0,2,4,6 ""
kill $SAMPLER
touch $R/m1b.done
