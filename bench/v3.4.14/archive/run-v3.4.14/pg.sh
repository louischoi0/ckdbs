#!/bin/bash
# PostgreSQL 17 floor on this host (defaults, tools/pg_setup.sh, port 15433), after the KDS cells. Idempotent per cell.
set -uo pipefail
source /home/louis/bench-runs/scenarios-v3.4.14/lib.sh
echo $$ > "$RUN/pg.pid"
while [ ! -e "$RUN/extras.done" ]; do sleep 5; done
export PATH=/usr/lib/postgresql/17/bin:$PATH
export PGROOT=$RUN/pg PGPORT=15433 PGDATABASE=bench
export PYTHONPATH=$TOOLS
mkdir -p "$PGROOT"
status "pg: init"
bash "$TOOLS/pg_setup.sh" init > "$RUN/logs/pg-init.log" 2>&1
PGPORT_=15433
cellpg() {
    local TAG=$1 SCRIPT=$2; shift 2
    [ -e "$RUN/done/$TAG" ] && return
    status "$TAG start"
    tl "== $TAG start $(date -u +%FT%TZ) =="
    precheck "$TAG"
    python3 "$TOOLS/$SCRIPT" --port $PGPORT_ --database bench --suffix ${TAG//-/_} "$@" --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    tl "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) =="
    touch "$RUN/done/$TAG"
}
for P in "" r2- r3-; do
  cellpg ${P}pg-s2-b1 pg_scenario2_freight.py --organizations 300 --ships 30 --operations 300 --cargos 4000 --bookings 3000 --verify 100 --seed 1 --synchronous-commit on
  for N in 200 1000 10000; do for M in none all; do
    cellpg ${P}pg-s3-n$N-$M pg_scenario3_library.py --loans $N --index-mode $M --seed 1
  done; done
done
for P in "" r2- r3-; do
  cellpg ${P}pg-s1 pg_scenario1_backtest.py --seed 1 --verify
  cellpg ${P}pg-s1s-n252 pg_scenario1_backtest.py --seed 1 --verify --years 1 --symbols 1 --exchanges 1
  cellpg ${P}pg-s1s-n1008 pg_scenario1_backtest.py --seed 1 --verify --years 1 --symbols 4 --exchanges 2
  cellpg ${P}pg-s1s-n10080 pg_scenario1_backtest.py --seed 1 --verify --years 5 --symbols 8 --exchanges 2
done
bash "$TOOLS/pg_setup.sh" stop >> "$RUN/logs/pg-init.log" 2>&1
status "PG DONE"
touch "$RUN/pg.done"
