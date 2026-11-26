#!/bin/bash
# Added series: the engine A/B again with the server pinned to the P-cores (CPUs 0-7) and the
# driver to the E-cores (CPUs 8-23). This host is a hybrid CPU (8 P-cores, 16 E-cores, powersave
# governor) and the unpinned cells of the main run are bimodal across runs; this series removes
# the P-or-E placement from the reading. Same cells, driver arguments and binaries as the unpinned
# A/B; five interleaved pairs, old first. Waits for run.sh's DONE. Resumable per cell.
R=/home/ukjin/bench-runs/scenarios-v3.4.17-159-g9f8f8b354
T=$R/src/tools
OUT=$R/out
NEW=$R/bin/pin_new.sh
OLD=$R/bin/pin_old.sh
STATUS=$R/run/status
export PYTHONPATH=$T
PIN_DEADLINE=$(date -u -d "2026-11-26 21:52:00" +%s)
while [ ! -e $R/run/DONE ]; do sleep 2; done
echo $$ > $R/run/runner2.pid
# the helpers and the cell functions of run.sh, unchanged
eval "$(sed -n '/^st() {/,/^sat2() {/p' $R/run.sh | sed '$d')"
python3() { taskset -c 8-23 /usr/bin/python3 "$@"; }

( while [ ! -e $R/run/DONE2 ]; do
    out=$(top -b -c -n2 -d1 -w 400 -o %CPU 2>/dev/null | awk '/^top -/{blk++} blk==2 && $1 ~ /^[0-9]+$/ && $9+0 > 10 {print}' \
        | grep -v "$R/bin/kds_server" | grep -v "scenario[1-4]_" | grep -v " top -b" \
        | awk '{c=$12; for(i=13;i<=NF&&i<=14;i++) c=c" "$i; printf "%s:%s:%s | ", $1, $9, substr(c,1,90)}')
    echo "$(date -u +%FT%TZ) load=$(cut -d' ' -f1 /proc/loadavg) foreign=${out:-none}" >> $R/run/monitor.log
    sleep 1
  done ) &
MON=$!

# Gate holds are outside the budget (CLAUDE.md, 2026-11-18): the deadline moves by their sum.
# Since the restart at 21:59 UTC the deadline is read at every pair, so a hold inside this series
# moves it too (before, it was fixed at the series' start).
deadline() { echo $(( PIN_DEADLINE + $(awk '{s+=$2} END{print s+0}' $R/run/hold) )); }
st "runner2 $$ start (pinned A/B), deadline now $(date -u -d @$(deadline) +%H:%M:%S) (21:52:00 plus the gate holds, read at every pair)"
for i in 1 2 3 4 5; do
  if [ $(date +%s) -ge $(deadline) ] && [ ! -e $OUT/logs/pin-ab-old$i-s1-c1-g.rc ]; then
    st "pinned A/B pair $i cut: past the budget deadline"; continue
  fi
  for v in old new; do
    b=$NEW; [ $v = old ] && b=$OLD
    s1cell pin-ab-$v$i-s1-c1-g $b 1 group
    s2cell pin-ab-$v$i-s2-c1-g $b 1 group 15704 pabs2c1g
    s2cell pin-ab-$v$i-b1-s2-c1-s $b 1 strict 15706 pabb1c1s --bookers 1
    s2cell pin-ab-$v$i-s2-c8-g $b 8 group 15705 pabs2c8g
    s3cell pin-ab-$v$i-s3-c1-n10000-none $b 1 10000 none
    s3cell pin-ab-$v$i-s3-c1-n10000-all $b 1 10000 all
  done
  st "pinned A/B pair $i end"
done
st "runner2 $$ DONE"
touch $R/run/DONE2
kill $MON 2>/dev/null
