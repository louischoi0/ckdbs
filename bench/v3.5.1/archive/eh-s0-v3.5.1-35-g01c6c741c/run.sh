#!/bin/bash
# EH-S0: the fold's cost a row, shape by shape, on main's engine (the EG close binary: EH opens with no engine
# source changed) beside PostgreSQL 18, through counting clients; then a stack sample of the server inside two
# folds. Both servers on CPUs 0-7, the driver on 8-23.
set -uo pipefail
R=/home/ukjin/bench-runs/eh-v3.5.1-35-g01c6c741c; O=$R/s0; T=$R/tools
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
rm -rf "$R/data"; mkdir -p "$R/data"
{ echo "data_file = $R/data/kds.db"; echo "cores = 1"; echo "port = $KPORT"; echo "durability = group"
  echo "log_dir = $R/data"; echo "log_file = kds.log"; echo "log_level = warn"; echo "buffer_pool_frames = 65536"; } > "$O/kds.conf"
taskset -c 0-7 nohup "$R/kds_server" --config "$O/kds.conf" > "$R/data/stdout.log" 2>&1 &
KPID=$!; for i in $(seq 1 150); do (exec 3<>/dev/tcp/127.0.0.1/$KPORT) 2>/dev/null && break; sleep 0.2; done
kill -0 $KPID 2>/dev/null || { st "kds did not start"; touch "$O/failed"; exit 1; }
export PGUSER=postgres PYTHONPATH=$T
gate shapes; st "shapes"
taskset -c 8-23 python3 "$T/eh_s0_premise.py" --kds-port $KPORT --kds-pid $KPID --pg-port $PGPORT --reps 5 \
  --json "$O/shapes.json" --status "$O/status" > "$O/shapes.stdout" 2> "$O/shapes.stderr"; echo $? > "$O/shapes.rc"
# The stack samples: the tables are dropped by the driver, so load them again and loop one fold.
for shape in agg-global group-7560-sum; do
  st "sample $shape"
  taskset -c 8-23 python3 - "$shape" "$KPORT" > "$O/sample-$shape.client" 2>&1 <<'PY' &
import sys, time, argparse
sys.path.insert(0, "/home/ukjin/bench-runs/eh-v3.5.1-35-g01c6c741c/tools")
import parity_bench as pb, eh_s0_premise as eh
a = argparse.Namespace(symbols=8, sessions=7560, seed=1, bars=8 * 7560)
k = pb.Kds(int(sys.argv[2]), 0)
pb.load(k, a)
sql = eh.SHAPES[sys.argv[1]][1]
open(f"/home/ukjin/bench-runs/eh-v3.5.1-35-g01c6c741c/s0/sample-{sys.argv[1]}.ready", "w").write("ready")
end = time.time() + 70; n = 0
while time.time() < end and not __import__("os").path.exists(f"/home/ukjin/bench-runs/eh-v3.5.1-35-g01c6c741c/s0/sample-{sys.argv[1]}.stop"):
    k.run(sql); n += 1
print("statements", n)
PY
  CL=$!
  for i in $(seq 1 100); do [ -e "$O/sample-$shape.ready" ] && break; sleep 0.2; done
  : > "$O/sample-$shape.stacks"
  for i in $(seq 1 150); do
    gdb -p $KPID -batch -ex "thread apply all bt 14" 2>/dev/null >> "$O/sample-$shape.stacks"; echo "=== sample $i" >> "$O/sample-$shape.stacks"
  done
  touch "$O/sample-$shape.stop"; wait $CL
done
kill $KPID; wait $KPID 2>/dev/null; cp "$R/data/kds.log" "$O/kds.serverlog" 2>/dev/null
podman rm -f $PGNAME >/dev/null 2>&1
date -u +%FT%TZ > "$O/end-time.txt"; st "ALL DONE"; touch "$O/done"
