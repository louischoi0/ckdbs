#!/bin/bash
# AZ-S7 re-measure: one fresh A/B server pair, one driver run, teardown.
# usage: pair.sh <tag> <driver args after the port flags...>
# B (9f170b1) on 15600, A (a59da9c) on 15601. cores=1, relaxed, BTREE, warn.
R=/home/cdkbs/bench-runs/az-s7b
W=/home/cdkbs/ckdbs/.claude/worktrees/az-s7-close
TAG=$1; shift
D=$R/$TAG
rm -rf "$D"; mkdir -p "$D/A" "$D/B"
for i in $(seq 1 90); do
  L=$(cut -d" " -f1 /proc/loadavg)
  BUSY=$(pgrep -a -f "cc1plus|cmake --build|ctest" | grep -v pgrep | grep -v "pair.sh" | wc -l)
  if [ "$BUSY" = 0 ] && awk "BEGIN{exit !($L < 1.0)}"; then break; fi
  sleep 2
done
echo "precell loadavg: $(cat /proc/loadavg)" > "$D/host.txt"
pgrep -a -f "cc1plus|cmake --build|ctest" | grep -v pgrep >> "$D/host.txt"
for X in A B; do
  if [ $X = B ]; then PORT=15600; else PORT=15601; fi
  cat > "$D/$X.conf" <<EOC
data_file = $D/$X/kds.db
cores = 1
port = $PORT
durability = relaxed
log_dir = $D/$X
log_file = kds.log
log_level = warn
EOC
  nohup "$R/kds_server-$X" --config "$D/$X.conf" > "$D/$X/stdout.log" 2>&1 &
  echo $! > "$D/$X/pid"
done
for PORT in 15600 15601; do
  for i in $(seq 1 100); do
    (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break
    sleep 0.2
  done
done
sleep 1
cd "$W/tools"
python3 "$@" --port 15600 --label B --ab-port 15601 --ab-label A --json "$D/result.json" > "$D/driver.txt" 2>&1
echo "driver exit $?" >> "$D/driver.txt"
for X in A B; do kill -TERM $(cat "$D/$X/pid") 2>/dev/null; done
sleep 1
for X in A B; do kill -KILL $(cat "$D/$X/pid") 2>/dev/null; done
rm -rf "$D"/A/kds.db* "$D"/B/kds.db*
