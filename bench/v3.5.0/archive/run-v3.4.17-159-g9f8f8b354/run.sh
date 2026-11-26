#!/bin/bash
# Scenario 1-4 runner at v3.4.17-159-g9f8f8b354 (comparator binary 07f4552a = v3.5.0).
# The cells, driver arguments and suffixes are the v3.5.0 run's (bench/v3.5.0/archive/run-v3.5.0/
# run_all.sh, ab.sh, extras.sh, ab4.sh), rewritten as one sequential, resumable script:
# a fresh server and data file per cell, a quiet gate before each, a cell whose .rc is 0 skipped.
R=/home/ukjin/bench-runs/scenarios-v3.4.17-159-g9f8f8b354
T=$R/src/tools
OUT=$R/out
NEW=$R/bin/kds_server
OLD=$R/bin/kds_server_old
# Measurement budget: fired 20:58:08 UTC, about one hour. Extras start only before this.
EXTRA_DEADLINE=$(date -u -d "2026-11-26 21:50:00" +%s)
mkdir -p $OUT/conf $OUT/json $OUT/logs $R/data $R/run
STATUS=$R/run/status
export PYTHONPATH=$T
echo $$ > $R/run/runner.pid

st() { echo "$(date -u +%FT%TZ) $*" >> $STATUS; }

cpu_busy() { # percent of all CPUs busy over $1 seconds
  local a b c d e f g h t1 i1 t2 i2
  read -r _ a b c d e f g h _ < /proc/stat; t1=$((a+b+c+d+e+f+g+h)); i1=$((d+e))
  sleep "$1"
  read -r _ a b c d e f g h _ < /proc/stat; t2=$((a+b+c+d+e+f+g+h)); i2=$((d+e))
  echo $(( 100 - 100*(i2-i1)/(t2-t1) ))
}

GATE_BUSY_PCT=4   # 24 CPUs: 4 % is about one core
gate() { # tag: wait for no build or test process (by comm, never by argv) and a quiet CPU
  local tag=$1 waited=0 procs busy t0
  t0=$(date +%s)
  # Rule changed at 21:38 UTC (gate v2): a build or test process holds the gate only while it uses
  # CPU - 5 % or more of one CPU over the 1 s sample, or it exits inside the sample. Until then its
  # mere existence held the gate, and another session's `kds_tests --gtest_repeat=20`, asleep at
  # 0.9 % of one CPU, held one gate for 315 s with no end in sight.
  local pids p j1
  local -A j0
  while :; do
    pids=$(ps -eo pid=,comm= | awk '$2 ~ /^(cc1plus|ctest|kds_tests|cmake|ninja|lto1)$/ {print $1}')
    j0=()
    for p in $pids; do j0[$p]=$(awk '{print $14+$15}' /proc/$p/stat 2>/dev/null); done
    busy=$(cpu_busy 1)
    procs=""
    for p in $pids; do
      j1=$(awk '{print $14+$15}' /proc/$p/stat 2>/dev/null)
      if [ -z "$j1" ] || [ -z "${j0[$p]}" ]; then procs="$procs $p exited-in-sample
"
      elif [ $((j1 - ${j0[$p]})) -ge 5 ]; then procs="$procs $p $(cat /proc/$p/comm 2>/dev/null) $((j1 - ${j0[$p]}))%
"
      fi
    done
    if [ -z "$procs" ] && [ "$busy" -le $GATE_BUSY_PCT ]; then break; fi
    sleep 2
    waited=$(( $(date +%s) - t0 ))
    st "gate $tag holding ${waited}s busy=${busy}% procs=$(echo "$procs" | tr -s ' \n' ' ')"
    if [ $waited -ge 1800 ]; then st "gate $tag gave up at ${waited}s"; break; fi
  done
  echo $waited > $OUT/logs/$tag.gatewait
  echo "$tag $waited" >> $R/run/hold
  {
    echo "time: $(date -u +%FT%TZ)"
    echo "loadavg: $(cat /proc/loadavg)"
    echo "gate wait s: $waited"
    echo "busy pct over 1 s at the gate: $busy"
    echo "comm cc1plus|ctest|kds_tests|cmake|ninja|lto1 using 5 % or more of one CPU (gate v2):"
    if [ -n "$procs" ]; then echo "$procs"; else echo "  none"; fi
    echo "such processes present, idle or not: $(echo $pids | wc -w)"
  } > $OUT/logs/$tag.precheck
}

start_server() { # tag bin cores durability port [extra conf lines]
  local tag=$1 bin=$2 cores=$3 dur=$4 port=$5; shift 5
  local d=$R/data/$tag i
  rm -rf $d; mkdir -p $d
  {
    echo "data_file = $d/kds.db"
    echo "cores = $cores"
    echo "port = $port"
    echo "durability = $dur"
    echo "log_dir = $d"
    echo "log_file = kds.log"
    echo "log_level = warn"
    echo "buffer_pool_frames = 65536"
    for l in "$@"; do [ -n "$l" ] && echo "$l"; done
  } > $OUT/conf/$tag.conf
  $bin --config $OUT/conf/$tag.conf > $OUT/logs/$tag.startup 2>&1 &
  SPID=$!
  echo $SPID >> $R/run/server.pids
  for i in $(seq 1 300); do
    if (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null; then return 0; fi
    kill -0 $SPID 2>/dev/null || { st "server for $tag exited at start"; return 1; }
    sleep 0.1
  done
  st "server for $tag did not listen in 30 s"
  return 1
}

stop_server() { # tag pid
  local i
  kill -TERM $2 2>/dev/null
  for i in $(seq 1 300); do kill -0 $2 2>/dev/null || break; sleep 0.1; done
  kill -KILL $2 2>/dev/null
  wait $2 2>/dev/null
  cp $R/data/$1/kds.log $OUT/logs/$1.serverlog 2>/dev/null
  rm -rf $R/data/$1
}

cell() { # tag bin cores durability port driver-command...
  local tag=$1 bin=$2 cores=$3 dur=$4 port=$5; shift 5
  [ "$(cat $OUT/logs/$tag.rc 2>/dev/null)" = "0" ] && return 0
  gate $tag
  st "cell $tag start ($(basename $bin) cores=$cores $dur)"
  if ! start_server $tag $bin $cores $dur $port; then
    echo 99 > $OUT/logs/$tag.rc; stop_server $tag $SPID; st "cell $tag server-start-failed"; return 1
  fi
  echo $OUT/logs/$tag.stdout > $R/run/current
  date -u +%FT%TZ > $OUT/logs/$tag.t0
  "$@" > $OUT/logs/$tag.stdout 2>&1
  echo $? > $OUT/logs/$tag.rc
  date -u +%FT%TZ > $OUT/logs/$tag.t1
  stop_server $tag $SPID
  st "cell $tag done rc=$(cat $OUT/logs/$tag.rc)"
}

s1cell() { # tag bin cores durability [extra driver args]   (suffix = the tag, as the v3.5.0 run)
  local tag=$1 bin=$2 cores=$3 dur=$4; shift 4
  cell $tag $bin $cores $dur 15710 python3 $T/scenario1_backtest.py --port 15710 --suffix ${tag//-/_} \
    --seed 1 --bars-clustered btree --verify --sync --analyze "$@" --json $OUT/json/$tag.json
}

S2_ARGS="--organizations 300 --ships 30 --operations 300 --cargos 4000 --bookers 8 --bookings 3000 --verify 100 --seed 1 --sync"
drv_s2() { # tag port suffix [extra driver args]: three invocations on one server, as the v3.5.0 run's cell2
  local tag=$1 port=$2 suf=$3; shift 3
  python3 $T/scenario2_freight.py --port $port --suffix $suf $S2_ARGS "$@" --schema-only > $OUT/logs/$tag.schema 2>&1
  echo "schema rc=$?" > $OUT/logs/$tag.prep
  python3 $T/scenario2_freight.py --port $port --suffix $suf $S2_ARGS "$@" --load-only > $OUT/logs/$tag.load 2>&1
  echo "load rc=$?" >> $OUT/logs/$tag.prep
  python3 $T/scenario2_freight.py --port $port --suffix $suf $S2_ARGS "$@" --json $OUT/json/$tag.json
}
s2cell() { # tag bin cores durability port suffix [extra driver args]
  local tag=$1 bin=$2 cores=$3 dur=$4 port=$5 suf=$6; shift 6
  cell $tag $bin $cores $dur $port drv_s2 $tag $port $suf "$@"
}

s3cell() { # tag bin cores loans index-mode
  local tag=$1 bin=$2 cores=$3 n=$4 mode=$5 extra=""
  [ $mode != none ] && extra="--assert-index-reads"
  cell $tag $bin $cores group 15720 python3 $T/scenario3_library.py --port 15720 \
    --suffix ${tag//-/_} --loans $n --index-mode $mode --server-indexes on --seed 1 \
    $extra --json $OUT/json/$tag.json
}

s4run() { # tag bin [extra conf line]
  local tag=$1 bin=$2 p1 p2 p3 ok=1; shift 2
  [ "$(cat $OUT/logs/$tag.rc 2>/dev/null)" = "0" ] && return 0
  gate $tag
  st "cell $tag start ($(basename $bin), three servers)"
  start_server $tag-off $bin 1 group 15751 "decay_half_life = 5" "cabin_optimizer_snapshot_interval_ms = 500" "$@" || ok=0; p1=$SPID
  start_server $tag-on $bin 1 group 15752 "decay_half_life = 5" "cabin_optimizer_snapshot_interval_ms = 500" "$@" || ok=0; p2=$SPID
  start_server $tag-declared $bin 1 group 15753 "decay_half_life = 5" "cabin_optimizer_snapshot_interval_ms = 500" "$@" || ok=0; p3=$SPID
  if [ $ok = 1 ]; then
    echo $OUT/logs/$tag.stdout > $R/run/current
    date -u +%FT%TZ > $OUT/logs/$tag.t0
    python3 -u $T/scenario4_cabinopt_days.py --port-off 15751 --port-on 15752 --port-declared 15753 \
      --pid-off $p1 --pid-on $p2 --pid-declared $p3 --suffix a --json $OUT/json/$tag.json > $OUT/logs/$tag.stdout 2>&1
    echo $? > $OUT/logs/$tag.rc
    date -u +%FT%TZ > $OUT/logs/$tag.t1
  else
    echo 99 > $OUT/logs/$tag.rc
  fi
  stop_server $tag-off $p1; stop_server $tag-on $p2; stop_server $tag-declared $p3
  st "cell $tag done rc=$(cat $OUT/logs/$tag.rc)"
}

sat2() { # tag cores durability port suffix bookers: a scenario 2 cell under a 1 s host CPU sampler
  local tag=$1 spid
  [ "$(cat $OUT/logs/$tag.rc 2>/dev/null)" = "0" ] && return 0
  ( while :; do echo "$(date -u +%FT%TZ) busy=$(cpu_busy 1)"; done >> $OUT/logs/$tag.cpu ) &
  spid=$!
  s2cell $tag $NEW $2 $3 $4 $5 --bookers $6
  kill $spid 2>/dev/null; wait $spid 2>/dev/null
}

phase_s1() {
  local r
  for r in "" r2- r3-; do
    s1cell ${r}s1-c1-g $NEW 1 group
    s1cell ${r}s1-c8-g $NEW 8 group
    s1cell ${r}s1-c1-s $NEW 1 strict
  done
  for r in "" r2- r3-; do
    s1cell ${r}s1s-n252-c1-g $NEW 1 group --years 1 --symbols 1 --exchanges 1
    s1cell ${r}s1s-n1008-c1-g $NEW 1 group --years 1 --symbols 4 --exchanges 2
    s1cell ${r}s1s-n10080-c1-g $NEW 1 group --years 5 --symbols 8 --exchanges 2
  done
}

phase_s2() {
  local r b
  for r in "" r2- r3-; do
    s2cell ${r}s2-c8-s $NEW 8 strict 15707 s2c8s
    s2cell ${r}s2-c1-s $NEW 1 strict 15706 s2c1s
    s2cell ${r}s2-c1-g $NEW 1 group 15704 s2c1g
    s2cell ${r}s2-c8-g $NEW 8 group 15705 s2c8g
  done
  for b in 1 2 4 16; do
    s2cell x-b$b-s2-c8-g $NEW 8 group 15705 xb${b}c8g --bookers $b
    s2cell x-b$b-s2-c8-s $NEW 8 strict 15707 xb${b}c8s --bookers $b
  done
  for r in "" r2- r3-; do
    s2cell ${r}b1-s2-c1-g $NEW 1 group 15704 b1c1g --bookers 1
    s2cell ${r}b1-s2-c1-s $NEW 1 strict 15706 b1c1s --bookers 1
  done
  sat2 sat-b32-s2-c8-g 8 group 15705 satb32c8g 32
  sat2 sat-b32-s2-c8-s 8 strict 15707 satb32c8s 32
}

phase_s3() {
  local r c n m
  for r in "" r2- r3-; do
    for c in 1 8; do for n in 200 1000 10000; do for m in none all; do
      s3cell ${r}s3-c$c-n$n-$m $NEW $c $n $m
    done; done; done
  done
}

phase_ab() { # old, new, old, new, old, new; one driver (identical in both commits)
  local i v b
  for i in 1 2 3; do
    for v in old new; do
      b=$NEW; [ $v = old ] && b=$OLD
      s1cell ab-$v$i-s1-c1-g $b 1 group
      s2cell ab-$v$i-s2-c1-g $b 1 group 15704 abs2c1g
      s2cell ab-$v$i-b1-s2-c1-s $b 1 strict 15706 abb1c1s --bookers 1
      s3cell ab-$v$i-s3-c1-n10000-none $b 1 10000 none
      # added to the v3.5.0 run's A/B: the eight-core booking cell and the indexed library cell
      s2cell ab-$v$i-s2-c8-g $b 8 group 15705 abs2c8g
      s3cell ab-$v$i-s3-c1-n10000-all $b 1 10000 all
    done
  done
}

phase_extras() {
  # Cheap extras first (seconds each), then scenario 4 run B (about 5 minutes) if the budget holds.
  # Gate holds are outside the budget (CLAUDE.md, 2026-11-18): the deadline moves by their sum.
  EXTRA_DEADLINE=$(( EXTRA_DEADLINE + $(awk '{s+=$2} END{print s+0}' $R/run/hold) ))
  st "extras deadline $(date -u -d @$EXTRA_DEADLINE +%H:%M:%S) (21:50:00 plus the gate holds)"
  if [ $(date +%s) -lt $EXTRA_DEADLINE ]; then
    s2cell rr-s2-c1-g $NEW 1 group 15704 s2rrc1g --isolation repeatable-read
    s2cell rr-s2-c8-g $NEW 8 group 15705 s2rrc8g --isolation repeatable-read
    sat2 sat-b16-s2-c8-g 8 group 15705 satb16c8g 16
    sat2 sat-b16-s2-c8-s 8 strict 15707 satb16c8s 16
  else
    st "extras rr and sat-b16 cut: past the budget deadline"
  fi
  if [ $(date +%s) -lt $EXTRA_DEADLINE ]; then
    s4run cd2-s4 $NEW "cabin_optimizer_cooldown_half_lives = 2"
  else
    st "extra cd2-s4 (scenario 4 run B) cut: past the budget deadline"
  fi
}

st "runner $$ start"
phase_s1; st "phase s1 end"
phase_s2; st "phase s2 end"
phase_s3; st "phase s3 end"
s4run s4 $NEW; st "phase s4 run A end"
phase_ab; st "phase ab end"
s4run ab-old1-s4 $OLD; st "phase s4 old-binary control end"
phase_extras; st "phase extras end"
st "runner $$ DONE"
touch $R/run/DONE
