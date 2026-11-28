#!/bin/bash
# EG close A/B: OLD vs NEW, interleaved, order alternating by round. Idempotent per cell.
set -uo pipefail
O=/home/ukjin/bench-runs/eg-close-v3.5.1-33-g01bd189f4; T=$O/tools; PORT=15791
st() { echo "$(date -u +%FT%TZ) $*" > "$O/status"; }
tl() { echo "$(date -u +%FT%TZ) $*" >> "$O/timeline"; }
busy() { local u n s i w q sq st i1 t1 i2 t2; read -r _ u n s i w q sq st _ < /proc/stat; i1=$((i+w)); t1=$((u+n+s+i+w+q+sq+st)); sleep 3
  read -r _ u n s i w q sq st _ < /proc/stat; i2=$((i+w)); t2=$((u+n+s+i+w+q+sq+st)); echo $((100*((t2-t1)-(i2-i1))/(t2-t1))); }
gate() { local w=0 b p; while :; do p=$(pgrep -a -f "cc1plus|cmake --build|ctest|kds_tests" | grep -v pgrep); b=$(busy)
    [ -z "$p" ] && [ "$b" -le 8 ] && break; st "$1 waiting: busy=${b}%"; w=$((w+10)); [ $w -ge 900 ] && { touch "$O/logs/$1.gate-timeout"; break; }; sleep 7; done
  { date -u +%FT%TZ; cat /proc/loadavg; echo "gate wait s: $w"; echo "busy3s: $b"; pgrep -a -f "cc1plus|cmake --build|ctest|kds_tests" | grep -v pgrep || echo "no build or test process"; } > "$O/logs/$1.precheck"; }
kstart() { local n=$1 bin=$2 cores=$3 d=$4; rm -rf "$O/data/$n"; mkdir -p "$O/data/$n"
  { echo "data_file = $O/data/$n/kds.db"; echo "cores = $cores"; echo "port = $PORT"; echo "durability = $d"; echo "log_dir = $O/data/$n"
    echo "log_file = kds.log"; echo "log_level = warn"; echo "buffer_pool_frames = 65536"; } > "$O/logs/$n.conf"
  taskset -c 0-7 nohup "$O/$bin" --config "$O/logs/$n.conf" > "$O/data/$n/stdout.log" 2>&1 &
  KPID=$!; local i; for i in $(seq 1 200); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && return 0; kill -0 $KPID 2>/dev/null || return 1; sleep 0.1; done; return 1; }
kstop() { kill $2 2>/dev/null; local i; for i in $(seq 1 200); do kill -0 $2 2>/dev/null || break; sleep 0.1; done; kill -KILL $2 2>/dev/null; rm -rf "$O/data/$1"; }
cell() { # id arm cores dur driver args...
  local id=$1 arm=$2 cores=$3 dur=$4 drv=$5; shift 5; local tag=$id-$arm bin=kds_server.$arm; [ -e "$O/done/$tag" ] && return
  gate $tag; st "$tag"; tl "== $tag start"
  kstart $tag $bin $cores $dur || { tl "== $tag SERVER FAILED"; return; }
  if [ "$drv" = eg_s0_premise.py ]; then
    PYTHONPATH=$T taskset -c 8-23 python3 $T/$drv --engine kds --port $PORT --pid $KPID --json "$O/json/$tag.json" > "$O/logs/$tag.stdout" 2>&1
  else
    PYTHONPATH=$T taskset -c 8-23 python3 $T/$drv --port $PORT --suffix ${tag//-/_} "$@" --json "$O/json/$tag.json" > "$O/logs/$tag.stdout" 2>&1
  fi
  echo $? > "$O/logs/$tag.rc"; kstop $tag $KPID; tl "== $tag end rc=$(cat $O/logs/$tag.rc)"; touch "$O/done/$tag"; }
pair() { local r=$1; shift; if [ $((r%2)) -eq 1 ]; then cell $1 old "${@:2}"; cell $1 new "${@:2}"; else cell $1 new "${@:2}"; cell $1 old "${@:2}"; fi; }
echo $$ > "$O/master.pid"; [ -e "$O/start-time.txt" ] || date -u +%FT%TZ > "$O/start-time.txt"
S2="--organizations 300 --ships 30 --operations 300 --cargos 4000 --bookings 3000 --verify 100 --seed 1"
S3="--loans 10000 --index-mode none --server-indexes on --seed 1"
MAXR=${MAXR:-5}
for r in $(seq 1 $MAXR); do
  pair $r c1-r$r 1 strict eg_s0_premise.py
  if [ $r -le 5 ]; then
    pair $r c2-r$r 1 strict scenario2_freight.py $S2 --bookers 1 --sync
    pair $r c3-r$r 8 strict scenario2_freight.py $S2 --bookers 8 --sync
    pair $r c4-r$r 1 group scenario3_library.py $S3
  fi
done
date -u +%FT%TZ > "$O/end-time.txt"; st "ALL DONE"; touch "$O/done/ALL"
