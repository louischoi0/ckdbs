#!/bin/bash
# EF-S0: the premise through equal clients, and the break between the two KDS clients.
# One KDS server (cores = 1, group) from a hashed copy of the Release binary built at 72580aa04,
# one fresh PostgreSQL 18 container at its defaults; both servers on CPUs 0-7, the driver on 8-23.
# run2.sh: pass C only, added after pass B (KDS send_command against PostgreSQL fetch, the pair the scenario 2 and 3 drivers time).
# Pass A: tools/parity_bench.py, KDS through kwp.Connection.query (equal clients), 5 reps, every shape.
# Pass B: the same with --kds-client send_command (the scenario drivers' timed path), 3 reps, four shapes.
set -uo pipefail
R=/home/ukjin/bench-runs/ef-v3.5.1-6-g72580aa04; O=$R/s0; TOOLS=$R/tools2
KPORT=15791; PGPORT=15434; PGNAME=kds-pg-ef; PGIMG=docker.io/library/postgres:18
st() { echo "$(date -u +%FT%TZ) $*" > "$O/status"; echo "$(date -u +%FT%TZ) $*" >> "$O/timeline"; }
echo $$ > "$O/master.pid"
date -u +%FT%TZ > "$O/start-time2.txt"
df -T "$R" > "$O/device.txt"
for p in $KPORT $PGPORT; do
  if ss -ltn | grep -q "127.0.0.1:$p "; then st "port $p held"; touch "$O/failed"; exit 1; fi
done
st "pg up"
podman rm -f $PGNAME >/dev/null 2>&1
podman run -d --name $PGNAME --network host -e POSTGRES_HOST_AUTH_METHOD=trust -e POSTGRES_DB=bench \
  -e POSTGRES_INITDB_ARGS="--encoding=UTF8 --locale=C" $PGIMG -c port=$PGPORT -c listen_addresses=127.0.0.1 > "$O/pg.container" 2>&1
for k in $(seq 1 60); do podman exec $PGNAME pg_isready -q -h 127.0.0.1 -p $PGPORT -d bench 2>/dev/null && break; sleep 1; done
sleep 2
pm=$(podman inspect -f '{{.State.Pid}}' $PGNAME); taskset -a -cp 0-7 $pm >/dev/null; for c in $(pgrep -P $pm); do taskset -a -cp 0-7 $c >/dev/null; done
for p in $pm $(pgrep -P $pm); do echo "$(taskset -cp $p) $(tr '\0' ' ' < /proc/$p/cmdline)"; done > "$O/pg-affinity.txt"
podman exec $PGNAME psql -h 127.0.0.1 -p $PGPORT -U postgres -d bench -Atc "select version()" > "$O/pg-version.txt" 2>&1
st "kds up"
rm -rf "$R/data"; mkdir -p "$R/data"
{ echo "data_file = $R/data/kds.db"; echo "cores = 1"; echo "port = $KPORT"; echo "durability = group"
  echo "log_dir = $R/data"; echo "log_file = kds.log"; echo "log_level = warn"; echo "buffer_pool_frames = 65536"; } > "$O/kds.conf"
taskset -c 0-7 nohup "$R/kds_server" --config "$O/kds.conf" > "$R/data/stdout.log" 2>&1 &
KPID=$!
for i in $(seq 1 150); do (exec 3<>/dev/tcp/127.0.0.1/$KPORT) 2>/dev/null && break; sleep 0.2; done
if ! kill -0 $KPID 2>/dev/null; then st "kds did not start"; touch "$O/failed"; exit 1; fi
taskset -cp $KPID > "$O/kds-affinity.txt"
{ date -u +%FT%TZ; cat /proc/loadavg; pgrep -a -f "cc1plus|cmake --build|ctest|kds_tests" | grep -v pgrep || echo "no build or test process"; } > "$O/precheck2.txt"
export PGUSER=postgres PYTHONPATH=$TOOLS
st "pass C"
taskset -c 8-23 python3 "$TOOLS/parity_bench.py" --kds-port $KPORT --kds-pid $KPID --pg-port $PGPORT \
  --kds-client send_command --pg-client fetch --shapes pk-lookup,bar-range,symbol-history,day-slice,agg-by-session \
  --reps 3 --json "$O/drivers.json" --status "$O/status" > "$O/drivers.stdout" 2> "$O/drivers.stderr"; echo $? > "$O/drivers.rc"
{ date -u +%FT%TZ; cat /proc/loadavg; pgrep -a -f "cc1plus|cmake --build|ctest|kds_tests" | grep -v pgrep || echo "no build or test process"; } > "$O/postcheck2.txt"
kill $KPID; wait $KPID 2>/dev/null; cp "$R/data/kds.log" "$O/kds.serverlog" 2>/dev/null
podman logs $PGNAME > "$O/pg.serverlog" 2>&1; podman rm -f $PGNAME >/dev/null 2>&1
date -u +%FT%TZ > "$O/end-time2.txt"; st "ALL DONE"; touch "$O/done2"
