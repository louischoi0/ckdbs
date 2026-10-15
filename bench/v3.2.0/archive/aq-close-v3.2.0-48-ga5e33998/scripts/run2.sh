#!/bin/bash
# AQ close part 2: more every-row UPDATE / point runs (u3), and the row-count sweep 200 / 10,000.
R=/home/cdkbs/bench-runs/aq-close
W=/home/cdkbs/ckdbs/.claude/worktrees/aq-expression-cabins
S=$W/bench/v3.2.0/archive/aq-close-v3.2.0-48-ga5e33998/scripts
B=$R/bin
GATE="--cell-quiet-pct 10 --cell-quiet-wait 30 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1"
COMMON="--pin-server 2 --pin-driver 4 --scan-rows 1000 --blocks 6 --quiet-load 2 --quiet-wait 30 $GATE"
stamp() { echo "$(date -u +%T) $1" >> $R/progress.txt; }
phase() { local name=$1; shift
  [ -f $R/flags/$name.done ] && { stamp "skip $name"; return 0; }
  stamp "phase start $name"; rm -rf $R/wd-$name; mkdir -p $R/wd-$name
  python3 "$@" > $R/out/$name.log 2>&1 & echo $! > $R/flags/$name.pid; echo $name > $R/flags/current
  wait $!; local rc=$?; stamp "phase end $name rc=$rc"; [ $rc -eq 0 ] && touch $R/flags/$name.done; }
BN="$W/tools/bn_census.py"
AQ="$S/aq_cabin.py"
phase u3 $BN --arm A2=$B/kds_server-A2 --arm B=$B/kds_server-B --arm A=$B/kds_server-A \
  --workdir $R/wd-u3 --port 32881 --runs 6 --rows 1000 --ops 2000 --range-ops 120 \
  --only select-pk,update-range $COMMON --out $R/out/u3.json
phase r200 $BN --arm A=$B/kds_server-A --arm A2=$B/kds_server-A2 --arm B=$B/kds_server-B \
  --workdir $R/wd-r200 --port 32891 --runs 3 --rows 200 --ops 2000 --range-ops 240 \
  --only select-pk,insert,update-range $COMMON --out $R/out/r200.json
phase r10k $BN --arm B=$B/kds_server-B --arm A2=$B/kds_server-A2 --arm A=$B/kds_server-A \
  --workdir $R/wd-r10k --port 32901 --runs 3 --rows 10000 --ops 2000 --range-ops 24 \
  --only select-pk,insert,update-range $COMMON --out $R/out/r10k.json
phase c200 $AQ --arm A=$B/kds_server-A --arm A2=$B/kds_server-A2 --arm B=$B/kds_server-B \
  --workdir $R/wd-c200 --port 32911 --runs 3 --rows 200 --ops 2000 \
  --only plain-insert,cab-insert,plain-select,cab-select $COMMON --out $R/out/c200.json
phase c10k $AQ --arm B=$B/kds_server-B --arm A2=$B/kds_server-A2 --arm A=$B/kds_server-A \
  --workdir $R/wd-c10k --port 32921 --runs 3 --rows 10000 --ops 1200 \
  --only plain-insert,cab-insert,plain-select,cab-select $COMMON --out $R/out/c10k.json
touch $R/run2.done; stamp "ALL DONE 2"
