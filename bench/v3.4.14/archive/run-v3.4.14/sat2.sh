#!/bin/bash
# Scenario 2 at the extreme (rule 4b): 20,000 committed bookings (not 3,000) and, in arm B, two client processes.
set -uo pipefail
source /home/louis/bench-runs/scenarios-v3.4.14/lib.sh
echo $$ > "$RUN/sat2.pid"
export PYTHONPATH=$TOOLS
while [ ! -e "$RUN/pg.done" ]; do sleep 5; done
BIN=$RUN/kds_server
D="--organizations 3000 --ships 30 --operations 3000 --cargos 40000 --seed 1 --verify 100 --sync --seconds 120"
run1() { # tag port dur bookers
  local TAG=$1 PORT=$2 DUR=$3 B=$4
  [ -e "$RUN/done/$TAG" ] && return
  status "$TAG start"; tl "== $TAG start $(date -u +%FT%TZ) =="
  precheck "$TAG"
  start_server "$TAG" "$PORT" 8 "$DUR" >> "$RUN/logs/$TAG.startup" 2>&1 || return
  rm -f "$RUN/logs/$TAG.cpustop"
  python3 "$TOOLS/scenario2_freight.py" --port "$PORT" --suffix sa $D --bookers $B --bookings 20000 --schema-only > "$RUN/logs/$TAG.schema" 2>&1
  python3 "$TOOLS/scenario2_freight.py" --port "$PORT" --suffix sa $D --bookers $B --bookings 20000 --load-only > "$RUN/logs/$TAG.load" 2>&1
  bash "$RUN/cpusample.sh" "$RUN/logs/$TAG.cpu" "$RUN/logs/$TAG.cpustop" &
  local SP=$!
  python3 "$TOOLS/scenario2_freight.py" --port "$PORT" --suffix sa $D --bookers $B --bookings 20000 --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
  echo $? > "$RUN/logs/$TAG.rc"
  touch "$RUN/logs/$TAG.cpustop"; wait $SP 2>/dev/null
  stop_server "$TAG" >> "$RUN/logs/$TAG.startup" 2>&1
  tl "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) =="
  touch "$RUN/done/$TAG"
}
run2() { # tag port dur : two client processes of 16 bookers, 10,000 bookings each
  local TAG=$1 PORT=$2 DUR=$3
  [ -e "$RUN/done/$TAG" ] && return
  status "$TAG start"; tl "== $TAG start $(date -u +%FT%TZ) =="
  precheck "$TAG"
  start_server "$TAG" "$PORT" 8 "$DUR" >> "$RUN/logs/$TAG.startup" 2>&1 || return
  rm -f "$RUN/logs/$TAG.cpustop"
  local S
  for S in sa sb; do
    python3 "$TOOLS/scenario2_freight.py" --port "$PORT" --suffix $S $D --bookers 16 --bookings 10000 --schema-only > "$RUN/logs/$TAG-$S.schema" 2>&1
    python3 "$TOOLS/scenario2_freight.py" --port "$PORT" --suffix $S $D --bookers 16 --bookings 10000 --load-only > "$RUN/logs/$TAG-$S.load" 2>&1
  done
  bash "$RUN/cpusample.sh" "$RUN/logs/$TAG.cpu" "$RUN/logs/$TAG.cpustop" &
  local SP=$!
  local PA PB
  python3 "$TOOLS/scenario2_freight.py" --port "$PORT" --suffix sa $D --bookers 16 --bookings 10000 --json "$RUN/json/$TAG-sa.json" > "$RUN/logs/$TAG-sa.stdout" 2>&1 &
  PA=$!
  python3 "$TOOLS/scenario2_freight.py" --port "$PORT" --suffix sb $D --bookers 16 --bookings 10000 --json "$RUN/json/$TAG-sb.json" > "$RUN/logs/$TAG-sb.stdout" 2>&1 &
  PB=$!
  wait $PA; wait $PB
  touch "$RUN/logs/$TAG.cpustop"; wait $SP 2>/dev/null
  stop_server "$TAG" >> "$RUN/logs/$TAG.startup" 2>&1
  tl "== $TAG end $(date -u +%FT%TZ) =="
  touch "$RUN/done/$TAG"
}
run1 big-b32-c8-g 15605 group 32
run1 big-b32-c8-s 15607 strict 32
run2 big2p-c8-g 15605 group
run2 big2p-c8-s 15607 strict
status "SAT2 DONE"
touch "$RUN/sat2.done"
