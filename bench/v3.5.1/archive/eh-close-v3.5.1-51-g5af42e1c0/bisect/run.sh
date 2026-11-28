#!/bin/bash
# EH close, localising group-60480: the S2 binary, the close binary and the close binary without the pool's
# size bound, rotated over nine rounds
# (old, new, new, old, old, new), PostgreSQL 18 beside both; counting clients; servers on CPUs 0-7, driver on 8-23.
set -uo pipefail
R=/home/ukjin/bench-runs/eh-v3.5.1-35-g01c6c741c; O=$R/bisect; T=$R/tools
KPORT=15791; PGPORT=15434; PGNAME=kds-pg-ef; PGIMG=docker.io/library/postgres:18
st() { echo "$(date -u +%FT%TZ) $*" > "$O/status"; echo "$(date -u +%FT%TZ) $*" >> "$O/timeline"; }
busy() { local u n s i w q sq st i1 t1 i2 t2; read -r _ u n s i w q sq st _ < /proc/stat; i1=$((i+w)); t1=$((u+n+s+i+w+q+sq+st)); sleep 3
  read -r _ u n s i w q sq st _ < /proc/stat; i2=$((i+w)); t2=$((u+n+s+i+w+q+sq+st)); echo $((100*((t2-t1)-(i2-i1))/(t2-t1))); }
gate() { local w=0 b p; while :; do p=$(pgrep -a -f "cc1plus|cmake --build|ctest|kds_tests" | grep -v pgrep); b=$(busy)
    [ -z "$p" ] && [ "$b" -le 8 ] && break; st "$1 waiting: busy=${b}%"; w=$((w+10)); [ $w -ge 900 ] && { touch "$O/$1.gate-timeout"; break; }; sleep 7; done
  { date -u +%FT%TZ; cat /proc/loadavg; echo "gate wait s: $w"; pgrep -a -f "cc1plus|cmake --build|ctest|kds_tests" | grep -v pgrep || echo "no build or test process"; } > "$O/$1.precheck"; }
echo $$ > "$O/master.pid"; date -u +%FT%TZ > "$O/start-time.txt"; df -T "$R" > "$O/device.txt"
for p in $KPORT $PGPORT; do ss -ltn | grep -q "127.0.0.1:$p " && { st "port $p held"; touch "$O/failed"; exit 1; }; done
st "pg up"; podman rm -f $PGNAME >/dev/null 2>&1
podman run -d --name $PGNAME --network host -e POSTGRES_HOST_AUTH_METHOD=trust -e POSTGRES_DB=bench \
  -e POSTGRES_INITDB_ARGS="--encoding=UTF8 --locale=C" $PGIMG -c port=$PGPORT -c listen_addresses=127.0.0.1 > "$O/pg.container" 2>&1
for k in $(seq 1 60); do podman exec $PGNAME pg_isready -q -h 127.0.0.1 -p $PGPORT -d bench 2>/dev/null && break; sleep 1; done; sleep 2
pm=$(podman inspect -f '{{.State.Pid}}' $PGNAME); taskset -a -cp 0-7 $pm >/dev/null; for c in $(pgrep -P $pm); do taskset -a -cp 0-7 $c >/dev/null; done
podman exec $PGNAME psql -h 127.0.0.1 -p $PGPORT -U postgres -d bench -Atc "select version()" > "$O/pg-version.txt" 2>&1
export PGUSER=postgres PYTHONPATH=$T
for r in 1 2 3 4 5 6 7 8 9; do
  case $((r % 3)) in 1) v=s2; B="$R/kds_server.s2";; 2) v=close; B="$R/kds_server.close";; 0) v=nobound; B="$R/kds_server.nobound";; esac
  rm -rf "$R/data"; mkdir -p "$R/data"
  { echo "data_file = $R/data/kds.db"; echo "cores = 1"; echo "port = $KPORT"; echo "durability = group"
    echo "log_dir = $R/data"; echo "log_file = kds.log"; echo "log_level = warn"; echo "buffer_pool_frames = 65536"; } > "$O/kds.conf"
  taskset -c 0-7 nohup "$B" --config "$O/kds.conf" > "$R/data/stdout.log" 2>&1 &
  KPID=$!; for i in $(seq 1 150); do (exec 3<>/dev/tcp/127.0.0.1/$KPORT) 2>/dev/null && break; sleep 0.2; done
  kill -0 $KPID 2>/dev/null || { st "kds did not start"; touch "$O/failed"; exit 1; }
  gate r$r-$v; st "r$r $v"
  taskset -c 8-23 python3 "$T/eh_bisect.py" --kds-port $KPORT --kds-pid $KPID --pg-port $PGPORT --reps 2 \
    --json "$O/r$r-$v.json" --status "$O/status" > "$O/r$r-$v.stdout" 2> "$O/r$r-$v.stderr"; echo $? > "$O/r$r-$v.rc"
  kill $KPID; wait $KPID 2>/dev/null
done
podman rm -f $PGNAME >/dev/null 2>&1
date -u +%FT%TZ > "$O/end-time.txt"; st "ALL DONE"; touch "$O/done"
