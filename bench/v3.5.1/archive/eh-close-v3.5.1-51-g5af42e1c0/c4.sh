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
pcell4() { local tag=$1 b=$2; [ -e "$O/done/$tag" ] && return
  kstart $tag $KPORT $b || { st "$tag SERVER FAILED"; return; }; local P=$KPID; gate $tag; st "$tag"
  PGUSER=postgres PYTHONPATH=$T taskset -c 8-23 python3 "$T/eh_s0_premise.py" --kds-port $KPORT --kds-pid $P --pg-port $PGPORT --reps 2 \
    --shapes group-8,group-7560,group-7560-sum,group-60480 --json "$O/json/$tag.json" --status "$O/status" > "$O/logs/$tag.stdout" 2> "$O/logs/$tag.stderr"; echo $? > "$O/logs/$tag.rc"
  kstop $tag $P; touch "$O/done/$tag"; }
date -u +%FT%TZ > $O/c4-start.txt
podman rm -f $PGNAME >/dev/null 2>&1
podman run -d --name $PGNAME --network host -e POSTGRES_HOST_AUTH_METHOD=trust -e POSTGRES_DB=bench -e POSTGRES_INITDB_ARGS="--encoding=UTF8 --locale=C" $PGIMG -c port=$PGPORT -c listen_addresses=127.0.0.1 > "$O/pg4.container" 2>&1
for k in $(seq 1 60); do podman exec $PGNAME pg_isready -q -h 127.0.0.1 -p $PGPORT -d bench 2>/dev/null && break; sleep 1; done; sleep 2
pm=$(podman inspect -f '{{.State.Pid}}' $PGNAME); taskset -a -cp 0-7 $pm >/dev/null; for c in $(pgrep -P $pm); do taskset -a -cp 0-7 $c >/dev/null; done
n=0; for v in s2 new new s2 s2 new; do n=$((n+1)); pcell4 c4-r$n-$v kds_server.$v; done
podman rm -f $PGNAME >/dev/null 2>&1; date -u +%FT%TZ > $O/c4-end.txt; echo done > $O/done/C4
