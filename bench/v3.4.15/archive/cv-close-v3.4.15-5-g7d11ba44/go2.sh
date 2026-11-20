#!/bin/bash
cd /home/louis/ckdbs/.claude/worktrees/cv-ordered-index-walk
R=/home/louis/ck-tmp/cv-close
quiet() { # need 3 consecutive quiet polls 5 s apart
  n=0; t0=$(date +%s)
  while [ $n -lt 3 ]; do
    c=$(pgrep -c cc1plus); l=$(cut -d' ' -f1 /proc/loadavg)
    if [ "$c" = 0 ] && awk -v l=$l 'BEGIN{exit !(l<3)}'; then n=$((n+1)); else n=0; fi
    sleep 5
  done
  echo "gate $1 held $(( $(date +%s) - t0 )) s" >> $R/go.log
}
: > $R/go.log
quiet q1
rm -rf $R/w1; mkdir -p $R/w1
echo "q1 start $(date -u +%T) load=$(cut -d' ' -f1-3 /proc/loadavg) cc1=$(pgrep -c cc1plus)" >> $R/go.log
python3 tools/cu_close_overhead.py --arm A=$R/run/kds_A --arm B=$R/run/kds_B --arm A2=$R/run/kds_A2 --workdir $R/w1 --port 31980 --runs 5 --blocks 6 --scale 1.0 --out $R/q1.json --progress $R/q1.progress > $R/q1.log 2>&1
echo "q1 done $(date -u +%T) load=$(cut -d' ' -f1-3 /proc/loadavg) cc1=$(pgrep -c cc1plus)" >> $R/go.log
quiet s4
rm -rf $R/w2; mkdir -p $R/w2
echo "s4 start $(date -u +%T) load=$(cut -d' ' -f1-3 /proc/loadavg) cc1=$(pgrep -c cc1plus)" >> $R/go.log
python3 tools/cv_s0_premise.py --bin $R/run/kds_B --workdir $R/w2 --out $R/s4.json --port 15741 --reps 2 --seconds 2.0 --heartbeat $R/s4.hb --status $R/s4.status > $R/s4.log 2>&1
echo "s4 done $(date -u +%T) load=$(cut -d' ' -f1-3 /proc/loadavg) cc1=$(pgrep -c cc1plus)" >> $R/go.log
