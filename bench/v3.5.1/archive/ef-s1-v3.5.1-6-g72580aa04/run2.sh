#!/bin/bash
# EF-S1, pass D2: pass D again after the values path cached its row descriptions (kwp.Connection.fetch),
# and one overlap round of each scenario cell on the final driver. Same servers, pinning and gate as run.sh.
set -uo pipefail
R=/home/ukjin/bench-runs/ef-v3.5.1-6-g72580aa04; O=$R/s1; NEW=$R/tools-new; OLD=$R/tools-old-72580aa04
KPORT=15791; PGPORT=15434; PGNAME=kds-pg-ef; PGIMG=docker.io/library/postgres:18
st() { echo "$(date -u +%FT%TZ) $*" > "$O/status"; echo "$(date -u +%FT%TZ) $*" >> "$O/timeline"; }
busy() { local u n s i w q sq st i1 t1 i2 t2; read -r _ u n s i w q sq st _ < /proc/stat; i1=$((i+w)); t1=$((u+n+s+i+w+q+sq+st)); sleep 3
  read -r _ u n s i w q sq st _ < /proc/stat; i2=$((i+w)); t2=$((u+n+s+i+w+q+sq+st)); echo $((100*((t2-t1)-(i2-i1))/(t2-t1))); }
gate() { local w=0 b p; while :; do p=$(pgrep -a -f "cc1plus|cmake --build|ctest|kds_tests" | grep -v pgrep); b=$(busy)
    [ -z "$p" ] && [ "$b" -le 8 ] && break; st "$1 waiting: busy=${b}%"; w=$((w+10)); [ $w -ge 600 ] && { touch "$O/logs/$1.gate-timeout"; break; }; sleep 7; done
  { date -u +%FT%TZ; cat /proc/loadavg; echo "gate wait s: $w"; pgrep -a -f "cc1plus|cmake --build|ctest|kds_tests" | grep -v pgrep || echo "no build or test process"; } > "$O/logs/$1.precheck"; }
kstart() { # tag durability
  rm -rf "$O/data/$1"; mkdir -p "$O/data/$1"
  { echo "data_file = $O/data/$1/kds.db"; echo "cores = 1"; echo "port = $KPORT"; echo "durability = $2"
    echo "log_dir = $O/data/$1"; echo "log_file = kds.log"; echo "log_level = warn"; echo "buffer_pool_frames = 65536"; } > "$O/logs/$1.conf"
  taskset -c 0-7 nohup "$R/kds_server" --config "$O/logs/$1.conf" > "$O/data/$1/stdout.log" 2>&1 &
  KPID=$!; for i in $(seq 1 150); do (exec 3<>/dev/tcp/127.0.0.1/$KPORT) 2>/dev/null && return 0; sleep 0.1; done; return 1; }
kstop() { kill $KPID 2>/dev/null; wait $KPID 2>/dev/null; rm -rf "$O/data/$1"; }
echo $$ > "$O/master.pid"; date -u +%FT%TZ > "$O/start-time2.txt"
for p in $KPORT $PGPORT; do ss -ltn | grep -q "127.0.0.1:$p " && { st "port $p held"; touch "$O/failed2"; exit 1; }; done
st "pg up"; podman rm -f $PGNAME >/dev/null 2>&1
podman run -d --name $PGNAME --network host -e POSTGRES_HOST_AUTH_METHOD=trust -e POSTGRES_DB=bench \
  -e POSTGRES_INITDB_ARGS="--encoding=UTF8 --locale=C" $PGIMG -c port=$PGPORT -c listen_addresses=127.0.0.1 > "$O/pg.container2" 2>&1
for k in $(seq 1 60); do podman exec $PGNAME pg_isready -q -h 127.0.0.1 -p $PGPORT -d bench 2>/dev/null && break; sleep 1; done; sleep 2
pm=$(podman inspect -f '{{.State.Pid}}' $PGNAME); taskset -a -cp 0-7 $pm >/dev/null; for c in $(pgrep -P $pm); do taskset -a -cp 0-7 $c >/dev/null; done
export PGUSER=postgres
gate passD2; kstart passD2 group || { st "kds did not start"; touch "$O/failed2"; exit 1; }
st "pass D2"
PYTHONPATH=$NEW taskset -c 8-23 python3 "$NEW/parity_bench.py" --kds-port $KPORT --kds-pid $KPID --pg-port $PGPORT \
  --kds-client rows --pg-client fetch --shapes pk-lookup,bar-range,symbol-history,day-slice,agg-by-session \
  --reps 3 --json "$O/json/passD2.json" --status "$O/status" > "$O/logs/passD2.stdout" 2> "$O/logs/passD2.stderr"; echo $? > "$O/logs/passD2.rc"
kstop passD2
S2="--organizations 300 --ships 30 --operations 300 --cargos 4000 --bookers 1 --bookings 3000 --verify 100 --seed 1 --sync"
for r in 4 5 6; do
  if [ $((r % 2)) = 1 ]; then order="old new"; else order="new old"; fi
  for v in $order; do
    [ $v = old ] && T=$OLD || T=$NEW
    tag=s3-n10000-none-$v-r$r; gate $tag; kstart $tag group && { st "$tag"
      PYTHONPATH=$T taskset -c 8-23 python3 "$T/scenario3_library.py" --port $KPORT --suffix ov --loans 10000 --index-mode none \
        --server-indexes on --seed 1 --json "$O/json/$tag.json" > "$O/logs/$tag.stdout" 2>&1; echo $? > "$O/logs/$tag.rc"; kstop $tag; }
    tag=s2-b1-c1-s-$v-r$r; gate $tag; kstart $tag strict && { st "$tag"
      PYTHONPATH=$T taskset -c 8-23 python3 "$T/scenario2_freight.py" --port $KPORT --suffix ov $S2 \
        --json "$O/json/$tag.json" > "$O/logs/$tag.stdout" 2>&1; echo $? > "$O/logs/$tag.rc"; kstop $tag; }
  done
done
podman rm -f $PGNAME >/dev/null 2>&1
date -u +%FT%TZ > "$O/end-time2.txt"; st "ALL DONE 2"; touch "$O/done2"
