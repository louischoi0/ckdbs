#!/bin/bash
# AP-S5 (PINNED servers cpu2, client cpu4): one fresh A/B server pair (or B alone with SOLO=1), one driver run, teardown.
# usage: pair.sh <tag> <driver args after the port flags...>
# B (c4b82e4) on 15610, A (4012617) on 15611. cores=1, relaxed, BTREE, warn.
R=/home/cdkbs/bench-runs/ap-s5
W=/home/cdkbs/ckdbs/.claude/worktrees/ap-s5-close
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
XS="A B"
if [ "$SOLO" = 1 ]; then XS="B"; fi
for X in $XS; do
  if [ $X = B ]; then PORT=15610; else PORT=15611; fi
  cat > "$D/$X.conf" <<EOC
data_file = $D/$X/kds.db
cores = 1
port = $PORT
durability = relaxed
log_dir = $D/$X
log_file = kds.log
log_level = warn
EOC
  nohup taskset -c 2 "$R/kds_server-$X" --config "$D/$X.conf" > "$D/$X/stdout.log" 2>&1 &
  echo $! > "$D/$X/pid"
done
PORTS="15610 15611"
if [ "$SOLO" = 1 ]; then PORTS="15610"; fi
for PORT in $PORTS; do
  for i in $(seq 1 100); do
    (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break
    sleep 0.2
  done
done
sleep 1
cd "$W/tools"
AB="--ab-port 15611 --ab-label A"
if [ "$SOLO" = 1 ]; then AB=""; fi
taskset -c 4 python3 "$@" --port 15610 --label B $AB --json "$D/result.json" > "$D/driver.txt" 2>&1
echo "driver exit $?" >> "$D/driver.txt"
for X in $XS; do kill -TERM $(cat "$D/$X/pid") 2>/dev/null; done
sleep 1
for X in $XS; do kill -KILL $(cat "$D/$X/pid") 2>/dev/null; done
rm -rf "$D"/A/kds.db* "$D"/B/kds.db*
