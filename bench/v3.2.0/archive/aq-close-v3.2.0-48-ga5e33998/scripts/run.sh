#!/bin/bash
# AQ close: A=7c234766, B=a5e33998, A2=A's bytes as second server. Phases resumable (flag files).
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
phase u1 $BN --arm B=$B/kds_server-B --arm A2=$B/kds_server-A2 --arm A=$B/kds_server-A \
  --workdir $R/wd-u1 --port 32821 --runs 3 --rows 1000 --ops 2000 --range-ops 120 \
  --only select-pk,insert,update-range $COMMON --out $R/out/u1.json
phase c1 $AQ --arm B=$B/kds_server-B --arm A2=$B/kds_server-A2 --arm A=$B/kds_server-A \
  --workdir $R/wd-c1 --port 32811 --runs 3 --rows 1000 --ops 2000 \
  --only plain-insert,cab-insert,plain-select,cab-select $COMMON --out $R/out/c1.json
phase u2 $BN --arm A=$B/kds_server-A --arm A2=$B/kds_server-A2 --arm B=$B/kds_server-B \
  --workdir $R/wd-u2 --port 32861 --runs 3 --rows 1000 --ops 2000 --range-ops 120 \
  --only select-pk,insert,update-range $COMMON --out $R/out/u2.json
phase c2 $AQ --arm A=$B/kds_server-A --arm A2=$B/kds_server-A2 --arm B=$B/kds_server-B \
  --workdir $R/wd-c2 --port 32841 --runs 3 --rows 1000 --ops 2000 \
  --only plain-insert,cab-insert,plain-select,cab-select $COMMON --out $R/out/c2.json
export AQ_EXPR=1
phase x $AQ --arm B=$B/kds_server-B --arm B2=$B/kds_server-B \
  --workdir $R/wd-x --port 32871 --runs 4 --rows 1000 --ops 2000 \
  --only plain-insert,cab-insert,x-insert-plain,x-insert,x-walk,x-select $COMMON --out $R/out/x.json
touch $R/run.done; stamp "ALL DONE"
