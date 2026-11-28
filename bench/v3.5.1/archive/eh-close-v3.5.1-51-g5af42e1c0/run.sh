#!/bin/bash
# EH close: cell 1 (eleven shapes, 6 rounds old/new/new/old/old/new), cell 3 (controls, 3 rounds), cell 2 (scenario 4 day 1).
# Servers CPUs 0-7, drivers CPUs 8-23. A fresh server and data file each. Idempotent via done/<tag>.
set -uo pipefail
R=/home/ukjin/bench-runs/eh-close-v3.5.1-51-g5af42e1c0; O=$R; T=$R/tools
KPORT=15791; PGPORT=15434; PGNAME=kds-pg-eh; PGIMG=docker.io/library/postgres:18
st() { echo "$(date -u +%FT%TZ) $*" > "$O/status"; echo "$(date -u +%FT%TZ) $*" >> "$O/timeline"; }
busy() { local u n s i w q sq st i1 t1 i2 t2; read -r _ u n s i w q sq st _ < /proc/stat; i1=$((i+w)); t1=$((u+n+s+i+w+q+sq+st)); sleep 3
  read -r _ u n s i w q sq st _ < /proc/stat; i2=$((i+w)); t2=$((u+n+s+i+w+q+sq+st)); echo $((100*((t2-t1)-(i2-i1))/(t2-t1))); }
gate() { local w=0 b p; while :; do p=$(pgrep -a -f "cc1plus|cmake --build|ctest|kds_tests" | grep -v pgrep); b=$(busy)
    [ -z "$p" ] && [ "$b" -le 8 ] && break; st "$1 waiting: busy=${b}%"; w=$((w+10)); [ $w -ge 900 ] && { touch "$O/logs/$1.gate-timeout"; break; }; sleep 7; done
  { date -u +%FT%TZ; cat /proc/loadavg; echo "last gate busy %: $b"; echo "gate wait s: $w"; pgrep -a -f "cc1plus|cmake --build|ctest|kds_tests" | grep -v pgrep || echo "no build or test process"; } > "$O/logs/$1.precheck"; }
kstart() { # name port bin [conf lines]
  local n=$1 p=$2 b=$3; shift 3; rm -rf "$O/data/$n"; mkdir -p "$O/data/$n"
  { echo "data_file = $O/data/$n/kds.db"; echo "cores = 1"; echo "port = $p"; echo "durability = group"; echo "log_dir = $O/data/$n"
    echo "log_file = kds.log"; echo "log_level = warn"; echo "buffer_pool_frames = 65536"; for l in "$@"; do echo "$l"; done; } > "$O/logs/$n.conf"
  taskset -c 0-7 nohup "$O/$b" --config "$O/logs/$n.conf" > "$O/data/$n/stdout.log" 2>&1 &
  KPID=$!; local i; for i in $(seq 1 200); do (exec 3<>/dev/tcp/127.0.0.1/$p) 2>/dev/null && return 0; kill -0 $KPID 2>/dev/null || return 1; sleep 0.1; done; return 1; }
kstop() { kill $2 2>/dev/null; local i; for i in $(seq 1 200); do kill -0 $2 2>/dev/null || break; sleep 0.1; done; kill -KILL $2 2>/dev/null; rm -rf "$O/data/$1"; }
pcell1() { # tag binfile : eleven shapes
  local tag=$1 b=$2; [ -e "$O/done/$tag" ] && return
  kstart $tag $KPORT $b || { st "$tag SERVER FAILED"; return; }; local P=$KPID
  gate $tag; st "$tag"
  PGUSER=postgres PYTHONPATH=$T taskset -c 8-23 python3 "$T/eh_s0_premise.py" --kds-port $KPORT --kds-pid $P --pg-port $PGPORT --reps 2 \
    --json "$O/json/$tag.json" --status "$O/status" > "$O/logs/$tag.stdout" 2> "$O/logs/$tag.stderr"; echo $? > "$O/logs/$tag.rc"
  kstop $tag $P; touch "$O/done/$tag"; }
pcell3() { # tag binfile : controls
  local tag=$1 b=$2; [ -e "$O/done/$tag" ] && return
  kstart $tag $KPORT $b || { st "$tag SERVER FAILED"; return; }; local P=$KPID
  gate $tag; st "$tag"
  PGUSER=postgres PYTHONPATH=$T taskset -c 8-23 python3 "$T/parity_bench.py" --kds-port $KPORT --kds-pid $P --pg-port $PGPORT --reps 3 \
    --shapes pk-lookup,day-slice --json "$O/json/$tag.json" --status "$O/status" > "$O/logs/$tag.stdout" 2> "$O/logs/$tag.stderr"; echo $? > "$O/logs/$tag.rc"
  kstop $tag $P; touch "$O/done/$tag"; }
k4cell() { # tag binfile
  local tag=$1 b=$2; [ -e "$O/done/$tag" ] && return
  local C1="decay_half_life = 5" C2="cabin_optimizer_snapshot_interval_ms = 500" PO PN PD
  kstart $tag-off 15651 $b "$C1" "$C2" || { st "$tag SERVER FAILED"; return; }; PO=$KPID
  kstart $tag-on 15652 $b "$C1" "$C2" || { st "$tag SERVER FAILED"; return; }; PN=$KPID
  kstart $tag-declared 15653 $b "$C1" "$C2" || { st "$tag SERVER FAILED"; return; }; PD=$KPID
  gate $tag; st "$tag"
  PYTHONPATH=$T taskset -c 8-23 python3 "$T/scenario4_cabinopt_days.py" --port-off 15651 --port-on 15652 --port-declared 15653 \
    --pid-off $PO --pid-on $PN --pid-declared $PD --suffix a --days 1 --json "$O/json/$tag.json" > "$O/logs/$tag.stdout" 2>&1
  echo $? > "$O/logs/$tag.rc"; kstop $tag-off $PO; kstop $tag-on $PN; kstop $tag-declared $PD; touch "$O/done/$tag"; }
pg4cell() { # tag
  local tag=$1; [ -e "$O/done/$tag" ] && return
  gate $tag; st "$tag"
  PYTHONPATH=$T PGUSER=postgres taskset -c 8-23 python3 "$T/pg_scenario4_cabinopt_days.py" --port $PGPORT --database bench --suffix ${tag//-/_} --seed 1 --days 1 \
    --json "$O/json/$tag.json" > "$O/logs/$tag.stdout" 2>&1; echo $? > "$O/logs/$tag.rc"; touch "$O/done/$tag"; }
echo $$ > "$O/master.pid"; [ -e "$O/start-time.txt" ] || date -u +%FT%TZ > "$O/start-time.txt"; df -T "$R" > "$O/device.txt"
for p in $KPORT $PGPORT 15651 15652 15653; do ss -ltn | grep -q "127.0.0.1:$p " && { st "port $p held"; touch "$O/failed"; exit 1; }; done
st "pg up"; podman rm -f $PGNAME >/dev/null 2>&1
podman run -d --name $PGNAME --network host -e POSTGRES_HOST_AUTH_METHOD=trust -e POSTGRES_DB=bench \
  -e POSTGRES_INITDB_ARGS="--encoding=UTF8 --locale=C" $PGIMG -c port=$PGPORT -c listen_addresses=127.0.0.1 > "$O/pg.container" 2>&1
for k in $(seq 1 60); do podman exec $PGNAME pg_isready -q -h 127.0.0.1 -p $PGPORT -d bench 2>/dev/null && break; sleep 1; done; sleep 2
pm=$(podman inspect -f '{{.State.Pid}}' $PGNAME); taskset -a -cp 0-7 $pm >/dev/null; for c in $(pgrep -P $pm); do taskset -a -cp 0-7 $c >/dev/null; done
for c in $pm $(pgrep -P $pm); do echo "$(taskset -cp $c) $(tr '\0' ' ' < /proc/$c/cmdline)"; done > "$O/pg-affinity.txt"
podman exec $PGNAME psql -h 127.0.0.1 -p $PGPORT -U postgres -d bench -Atc "select version()" > "$O/pg-version.txt" 2>&1
n=0; for v in old new new old old new; do n=$((n+1)); pcell1 c1-r$n-$v kds_server.$v; done
n=0; for v in old new new old old new; do n=$((n+1)); pcell3 c3-r$n-$v kds_server.$v; done
pg4cell c2-pg-a
k4cell c2-old-1 kds_server.old; k4cell c2-new-1 kds_server.new; k4cell c2-new-2 kds_server.new; k4cell c2-old-2 kds_server.old
pg4cell c2-pg-b
podman logs $PGNAME > "$O/pg.serverlog" 2>&1; podman rm -f $PGNAME >/dev/null 2>&1
date -u +%FT%TZ > "$O/end-time.txt"; st "ALL DONE"; touch "$O/done/ALL"
