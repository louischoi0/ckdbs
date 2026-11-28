#!/bin/bash
# EF-S2, run c: scenario 1's pairs again, its KDS statements through ServerConnection.rendered (EF-Q8 (b)).
# Otherwise run.sh of EF-S2 unchanged:
# Three rounds; in a round each KDS cell is followed (odd rounds) or preceded (even rounds) by its PostgreSQL
# twin. A fresh KDS server and data file a cell; one PostgreSQL cluster for the run, fresh table names a cell.
# Both servers on CPUs 0-7, every driver on CPUs 8-23. Idempotent: a cell with done/<tag> is skipped.
set -uo pipefail
R=/home/ukjin/bench-runs/ef-v3.5.1-6-g72580aa04; O=$R/s2c; T=$R/tools-s2c
PGPORT=15434; PGNAME=kds-pg-ef; PGIMG=docker.io/library/postgres:18
st() { echo "$(date -u +%FT%TZ) $*" > "$O/status"; }
tl() { echo "$(date -u +%FT%TZ) $*" >> "$O/timeline"; }
busy() { local u n s i w q sq st i1 t1 i2 t2; read -r _ u n s i w q sq st _ < /proc/stat; i1=$((i+w)); t1=$((u+n+s+i+w+q+sq+st)); sleep 3
  read -r _ u n s i w q sq st _ < /proc/stat; i2=$((i+w)); t2=$((u+n+s+i+w+q+sq+st)); echo $((100*((t2-t1)-(i2-i1))/(t2-t1))); }
gate() { local w=0 b p; while :; do p=$(pgrep -a -f "cc1plus|cmake --build|ctest|kds_tests" | grep -v pgrep); b=$(busy)
    [ -z "$p" ] && [ "$b" -le 8 ] && break; st "$1 waiting for a quiet host: busy=${b}%"; w=$((w+10)); [ $w -ge 900 ] && { touch "$O/logs/$1.gate-timeout"; break; }; sleep 7; done
  { date -u +%FT%TZ; cat /proc/loadavg; echo "gate wait s: $w"; pgrep -a -f "cc1plus|cmake --build|ctest|kds_tests" | grep -v pgrep || echo "no build or test process"; } > "$O/logs/$1.precheck"; }
kstart() { # name port durability [conf lines]: sets KPID
  local n=$1 p=$2 d=$3; shift 3; rm -rf "$O/data/$n"; mkdir -p "$O/data/$n"
  { echo "data_file = $O/data/$n/kds.db"; echo "cores = 1"; echo "port = $p"; echo "durability = $d"; echo "log_dir = $O/data/$n"
    echo "log_file = kds.log"; echo "log_level = warn"; echo "buffer_pool_frames = 65536"; for l in "$@"; do echo "$l"; done; } > "$O/logs/$n.conf"
  taskset -c 0-7 nohup "$R/kds_server" --config "$O/logs/$n.conf" > "$O/data/$n/stdout.log" 2>&1 &
  KPID=$!; local i; for i in $(seq 1 200); do (exec 3<>/dev/tcp/127.0.0.1/$p) 2>/dev/null && return 0; kill -0 $KPID 2>/dev/null || return 1; sleep 0.1; done; return 1; }
kstop() { kill $2 2>/dev/null; local i; for i in $(seq 1 200); do kill -0 $2 2>/dev/null || break; sleep 0.1; done; kill -KILL $2 2>/dev/null; rm -rf "$O/data/$1"; }
pg_ready() { podman exec $PGNAME pg_isready -q -h 127.0.0.1 -p $PGPORT -d bench 2>/dev/null; }
pg_up() {
  if ! podman container exists $PGNAME; then
    ss -ltn | grep -q "127.0.0.1:$PGPORT " && { tl "port $PGPORT held"; return 1; }
    podman run -d --name $PGNAME --network host -e POSTGRES_HOST_AUTH_METHOD=trust -e POSTGRES_DB=bench \
      -e POSTGRES_INITDB_ARGS="--encoding=UTF8 --locale=C" $PGIMG -c port=$PGPORT -c listen_addresses=127.0.0.1 >> "$O/logs/pg.container" 2>&1 || return 1
  elif [ "$(podman inspect -f '{{.State.Running}}' $PGNAME)" != true ]; then podman start $PGNAME >> "$O/logs/pg.container" 2>&1 || return 1; fi
  local k; for k in $(seq 1 60); do pg_ready && break; sleep 1; done; pg_ready || return 1; sleep 1
  local pm c; pm=$(podman inspect -f '{{.State.Pid}}' $PGNAME); taskset -a -cp 0-7 $pm >/dev/null; for c in $(pgrep -P $pm); do taskset -a -cp 0-7 $c >/dev/null; done
  for c in $pm $(pgrep -P $pm); do echo "$(taskset -cp $c) $(tr '\0' ' ' < /proc/$c/cmdline)"; done > "$O/logs/pg-affinity.txt"; }
kcell() { # tag durability driver args...   (KDS, one server, port 15791)
  local tag=$1 dur=$2 drv=$3; shift 3; [ -e "$O/done/$tag" ] && return
  gate $tag; st "$tag"; tl "== $tag start"
  kstart $tag 15791 $dur || { tl "== $tag SERVER FAILED"; return; }
  PYTHONPATH=$T taskset -c 8-23 python3 "$T/$drv" --port 15791 --suffix ${tag//-/_} "$@" --json "$O/json/$tag.json" > "$O/logs/$tag.stdout" 2>&1
  echo $? > "$O/logs/$tag.rc"; kstop $tag $KPID; tl "== $tag end rc=$(cat $O/logs/$tag.rc)"; touch "$O/done/$tag"; }
k4cell() { # tag   (scenario 4: three servers)
  local tag=$1 a; [ -e "$O/done/$tag" ] && return
  gate $tag; st "$tag"; tl "== $tag start"
  local C1="decay_half_life = 5" C2="cabin_optimizer_snapshot_interval_ms = 500" PO PN PD
  kstart $tag-off 15651 group "$C1" "$C2" || { tl "== $tag SERVER FAILED"; return; }; PO=$KPID
  kstart $tag-on 15652 group "$C1" "$C2" || { tl "== $tag SERVER FAILED"; return; }; PN=$KPID
  kstart $tag-declared 15653 group "$C1" "$C2" || { tl "== $tag SERVER FAILED"; return; }; PD=$KPID
  PYTHONPATH=$T taskset -c 8-23 python3 "$T/scenario4_cabinopt_days.py" --port-off 15651 --port-on 15652 --port-declared 15653 \
    --pid-off $PO --pid-on $PN --pid-declared $PD --suffix a --json "$O/json/$tag.json" > "$O/logs/$tag.stdout" 2>&1
  echo $? > "$O/logs/$tag.rc"; kstop $tag-off $PO; kstop $tag-on $PN; kstop $tag-declared $PD
  tl "== $tag end rc=$(cat $O/logs/$tag.rc)"; touch "$O/done/$tag"; }
pcell() { # tag driver args...   (PostgreSQL twin)
  local tag=$1 drv=$2; shift 2; [ -e "$O/done/$tag" ] && return
  gate $tag; st "$tag"; tl "== $tag start"
  pg_ready || pg_up || { tl "== $tag PG NOT READY"; return; }
  PYTHONPATH=$T PGUSER=postgres taskset -c 8-23 python3 "$T/$drv" --port $PGPORT --database bench --suffix ${tag//-/_} "$@" --json "$O/json/$tag.json" > "$O/logs/$tag.stdout" 2>&1
  echo $? > "$O/logs/$tag.rc"; tl "== $tag end rc=$(cat $O/logs/$tag.rc)"; touch "$O/done/$tag"; }
pair() { # round "kds cell command" "pg cell command": KDS first on odd rounds
  if [ $(( $1 % 2 )) = 1 ]; then eval "$2"; eval "$3"; else eval "$3"; eval "$2"; fi; }

echo $$ > "$O/master.pid"; [ -e "$O/start-time.txt" ] || date -u +%FT%TZ > "$O/start-time.txt"
df -T "$R" > "$O/device.txt"; sha256sum "$R/kds_server" > "$O/binary.sha256"
st "pg up"; pg_up || { tl "PG FAILED TO START"; touch "$O/failed"; exit 1; }
[ -e "$O/logs/pg-version.txt" ] || podman exec $PGNAME psql -h 127.0.0.1 -p $PGPORT -U postgres -d bench -Atc "select version()" > "$O/logs/pg-version.txt" 2>&1
S2="--organizations 300 --ships 30 --operations 300 --cargos 4000 --bookings 3000 --verify 100 --seed 1"
S1="--seed 1 --bars-clustered btree --verify --sync --analyze"
for r in 1 2 3; do
  pair $r "kcell r$r-s1-c1-g group scenario1_backtest.py $S1; kcell r$r-s1-c1-s strict scenario1_backtest.py $S1" \
          "pcell r$r-pg-s1 pg_scenario1_backtest.py --seed 1 --verify"
  pair $r "kcell r$r-s1s-n252-c1-g group scenario1_backtest.py $S1 --years 1 --symbols 1 --exchanges 1" \
          "pcell r$r-pg-s1s-n252 pg_scenario1_backtest.py --seed 1 --verify --years 1 --symbols 1 --exchanges 1"
  pair $r "kcell r$r-s1s-n1008-c1-g group scenario1_backtest.py $S1 --years 1 --symbols 4 --exchanges 2" \
          "pcell r$r-pg-s1s-n1008 pg_scenario1_backtest.py --seed 1 --verify --years 1 --symbols 4 --exchanges 2"
  pair $r "kcell r$r-s1s-n10080-c1-g group scenario1_backtest.py $S1 --years 5 --symbols 8 --exchanges 2" \
          "pcell r$r-pg-s1s-n10080 pg_scenario1_backtest.py --seed 1 --verify --years 5 --symbols 8 --exchanges 2"
done
podman logs $PGNAME > "$O/logs/pg.serverlog" 2>&1; podman rm -f $PGNAME >/dev/null 2>&1
date -u +%FT%TZ > "$O/end-time.txt"; st "ALL DONE"; touch "$O/done/ALL"
