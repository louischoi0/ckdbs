#!/bin/bash
# M1b control: cores = 1, the server's non-reactor threads kept off the reactor's CPU (mask 0,2,6; reactor_cpus = 4).
R=/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05
P=/tmp/claude-1000/-home-cdkbs-ckdbs/f6bce74e-e48d-4d48-817a-4e239db00c8e/scratchpad/s10-progress.txt
cd $R
rm -f $R/m1b-off.done
python3 -I $R/drivers/sampler.py $R/logs/host-samples-m1b-off.jsonl &
SAMPLER=$!
echo "$(date -u +%H:%M) M1b cores 1 offcpu start load $(cut -d' ' -f1-3 /proc/loadavg)" >> $P
taskset -c 7 python3 $R/drivers/br_cost.py backup --cores-one 1 --taskset 0,2,6 --tag offcpu --ballast-rows 540000 --reps 5 --window 6 --port 31821 --out $R/out/m1b --progress $P > $R/logs/m1b-c1-offcpu.log 2>&1
echo "$(date -u +%H:%M) M1b cores 1 offcpu end rc=$? load $(cut -d' ' -f1-3 /proc/loadavg)" >> $P
kill $SAMPLER
touch $R/m1b-off.done
