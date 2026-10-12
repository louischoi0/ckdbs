#!/bin/bash
# BR close, M0: the milestone's OLTP overhead A/B. A = bin/kds_server-A (fb953f07, v2.7.0-908-gfb953f07, the commit BR opened from),
# B = bin/kds_server-B (a8110b05, v2.7.0-913-ga8110b05, BR closed). Backup off, archive off (the defaults) on both.
# Server pinned (cores = 1: CPU 4; cores = 4: CPUs 0,2,4,6 with reactor_cpus = 4,2,6,0), driver on CPU 7 (taskset).
# Cells run one after another, never overlapped; the sampler writes a host sample per second.
R=/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05
P=/tmp/claude-1000/-home-cdkbs-ckdbs/f6bce74e-e48d-4d48-817a-4e239db00c8e/scratchpad/s10-progress.txt
cd $R
rm -f $R/m0.done
python3 -I $R/drivers/sampler.py $R/logs/host-samples-m0.jsonl &
SAMPLER=$!
( while true; do sleep 240; echo "$(date -u +%H:%M) M0 alive load $(cut -d' ' -f1-3 /proc/loadavg)" >> $P; done ) &
HB=$!
echo "started $(date -u '+%F %T') UTC" > $R/logs/cells-m0.log

DRV="python3 -I $R/drivers/br_overhead_ab.py --port-a 31811 --port-b 31812 --max-load ${MAXLOAD:-3} --max-wait ${MAXWAIT:-45}"

cell() {  # name binA binB cores runs extra...
  local name=$1 ba=$2 bb=$3 c=$4 runs=$5; shift 5
  local pin=4 rcpu=""
  if [ $c = 4 ]; then pin=0,2,4,6; rcpu="--reactor-cpus 4,2,6,0"; fi
  echo "cell $name start $(date -u '+%T') load $(cut -d' ' -f1-3 /proc/loadavg)" >> $R/logs/cells-m0.log
  taskset -c 7 $DRV --bin-a $R/bin/$ba --bin-b $R/bin/$bb --runs $runs --cores $c --pin-server $pin $rcpu --workdir $R/work/w-$name --out $R/out/m0/$name "$@" > $R/logs/m0-$name.log 2>&1
  local rc=$?
  echo "cell $name end $(date -u '+%T') rc=$rc load $(cut -d' ' -f1-3 /proc/loadavg)" >> $R/logs/cells-m0.log
  echo "$(date -u +%H:%M) M0 cell $name done rc=$rc load $(cut -d' ' -f1-3 /proc/loadavg)" >> $P
}

mkdir -p $R/out/m0 $R/logs
for spec in "$@"; do
  case $spec in
    relaxed-c1) cell relaxed-c1 kds_server-A kds_server-B 1 ${RUNS:-12} --rows 200,1000,10000 ;;
    relaxed-c4) cell relaxed-c4 kds_server-A kds_server-B 4 ${RUNS:-12} --rows 200,1000,10000 ;;
    aa-c1) cell aa-c1 kds_server-A kds_server-A 1 ${AARUNS:-6} --rows 200,1000,10000 ;;
    aa-c4) cell aa-c4 kds_server-A kds_server-A 4 ${AARUNS:-6} --rows 200,1000,10000 ;;
    group-c1) cell group-c1 kds_server-A kds_server-B 1 ${GRUNS:-8} --rows 200,1000,10000 --durability group --arms iso,insert,insert-again --ops 1500 ;;
    group-c4) cell group-c4 kds_server-A kds_server-B 4 ${GRUNS:-8} --rows 200,1000,10000 --durability group --arms iso,insert,insert-again --ops 1500 ;;
    aag-c1) cell aag-c1 kds_server-A kds_server-A 1 ${AARUNS:-6} --rows 200,1000,10000 --durability group --arms iso,insert,insert-again --ops 1500 ;;
    aag-c4) cell aag-c4 kds_server-A kds_server-A 4 ${AARUNS:-6} --rows 200,1000,10000 --durability group --arms iso,insert,insert-again --ops 1500 ;;
  esac
done

kill $SAMPLER $HB
echo "all done $(date -u '+%F %T') UTC" >> $R/logs/cells-m0.log
touch $R/m0.done
