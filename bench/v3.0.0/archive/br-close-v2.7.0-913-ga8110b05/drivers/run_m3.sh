#!/bin/bash
# BR close, M3: restore time. Backups kept by M1b (a backup taken under load, log window of 100s of MiB) and by M2 (an idle
# backup whose window is the 3.6 GiB the ballast load wrote; then PITR to `latest` through the 157-segment archive).
R=/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05
P=/tmp/claude-1000/-home-cdkbs-ckdbs/f6bce74e-e48d-4d48-817a-4e239db00c8e/scratchpad/s10-progress.txt
cd $R
mkdir -p $R/out/m3
rm -f $R/m3.done
python3 -I $R/drivers/sampler.py $R/logs/host-samples-m3.jsonl &
SAMPLER=$!
run() {  # label cores backup [extra...]
  local label=$1 cores=$2 bk=$3; shift 3
  echo "$(date -u +%H:%M) M3 $label start load $(cut -d' ' -f1-3 /proc/loadavg)" >> $P
  taskset -c 7 python3 $R/drivers/br_restore.py --label $label --cores $cores --backup $bk --out $R/out/m3/$label.json "$@" > $R/logs/m3-$label.log 2>&1
  echo "$(date -u +%H:%M) M3 $label end rc=$? load $(cut -d' ' -f1-3 /proc/loadavg)" >> $P
}
run m1b-c1 1 $R/work/m1b-c1/bk-4
run m1b-c4 4 $R/work/m1b-c4/bk-4
run m2-stop 4 $R/work/m2/bk
run m2-pitr-latest 4 $R/work/m2/bk --archive $R/work/m2/archive --target latest --reps 2
kill $SAMPLER
touch $R/m3.done
