
PGPORT=15434
PGDB=bench

cellpg() {  # tag script-name [args...]  (PostgreSQL 18.6 twin; fresh table names via suffix)
    local TAG=$1 SCRIPT=$2; shift 2
    [ -e "$RUN/done/$TAG" ] && return
    status "$TAG start"
    tl "== $TAG start $(date -u +%FT%TZ) =="
    precheck "$TAG"
    python3 "$TOOLS/$SCRIPT" --port $PGPORT --database $PGDB --suffix ${TAG//-/_} "$@" \
        --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    tl "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) =="
    status "$TAG end"
    touch "$RUN/done/$TAG"
}

STAGE=${STAGE:-all}
S1DRV=${S1DRV:-$TOOLS/scenario1_backtest.py}

if [ "$STAGE" = all ] || [ "$STAGE" = s1 ]; then
  for P in "" r2- r3-; do
    vcell cell1 ${P}s1-c1-g ${P}s1-c1-g "$S1DRV" 1 group
    vcell cell1 ${P}s1-c8-g ${P}s1-c8-g "$S1DRV" 8 group
    vcell cell1 ${P}s1-c1-s ${P}s1-c1-s "$S1DRV" 1 strict
  done
fi

if [ "$STAGE" = all ] || [ "$STAGE" = s2 ]; then
  for P in "" r2- r3-; do
    vcell cell2 ${P}s2-c8-s ${P}s2-c8-s s2c8s 15607 8 strict
    vcell cell2 ${P}s2-c1-s ${P}s2-c1-s s2c1s 15606 1 strict
    vcell cell2 ${P}s2-c1-g ${P}s2-c1-g s2c1g 15604 1 group
    vcell cell2 ${P}s2-c8-g ${P}s2-c8-g s2c8g 15605 8 group
  done
  vcell cell2 rr-s2-c1-g rr-s2-c1-g s2rrc1g 15604 1 group --isolation repeatable-read
  vcell cell2 rr-s2-c8-g rr-s2-c8-g s2rrc8g 15605 8 group --isolation repeatable-read
  for B in 1 2 4 16; do
    vcell cell2 x-b$B-s2-c8-g x-b$B-s2-c8-g xb${B}c8g 15605 8 group --bookers $B
    vcell cell2 x-b$B-s2-c8-s x-b$B-s2-c8-s xb${B}c8s 15607 8 strict --bookers $B
  done
  for P in "" r2- r3-; do
    vcell cell2 ${P}b1-s2-c1-g ${P}b1-s2-c1-g b1c1g 15604 1 group --bookers 1
    vcell cell2 ${P}b1-s2-c1-s ${P}b1-s2-c1-s b1c1s 15606 1 strict --bookers 1
  done
fi

if [ "$STAGE" = all ] || [ "$STAGE" = s3 ]; then
  for P in "" r2- r3-; do
    for C in 1 8; do
      for N in 200 1000 10000; do
        for M in none all; do
          vcell cell3 ${P}s3-c$C-n$N-$M ${P}s3-c$C-n$N-$M $C group $N $M
        done
      done
    done
  done
fi

if [ "$STAGE" = all ] || [ "$STAGE" = s4 ]; then
  vcell cell4 s4 s4-off ""
  vcell cell4 cd2-s4 cd2-s4-off "cabin_optimizer_cooldown_half_lives = 2"
fi

if [ "$STAGE" = all ] || [ "$STAGE" = s1sw ]; then
  for P in "" r2- r3-; do
    vcell cell1x ${P}s1s-n252-c1-g ${P}s1s-n252-c1-g "$S1DRV" 1 group --years 1 --symbols 1 --exchanges 1
    vcell cell1x ${P}s1s-n1008-c1-g ${P}s1s-n1008-c1-g "$S1DRV" 1 group --years 1 --symbols 4 --exchanges 2
    vcell cell1x ${P}s1s-n10080-c1-g ${P}s1s-n10080-c1-g "$S1DRV" 1 group --years 5 --symbols 8 --exchanges 2
  done
fi

if [ "$STAGE" = all ] || [ "$STAGE" = sat ]; then
  for B in 16 32; do
    vcell cell2s sat-b$B-s2-c8-g sat-b$B-s2-c8-g satb${B}c8g 15605 8 group --bookers $B
    vcell cell2s sat-b$B-s2-c8-s sat-b$B-s2-c8-s satb${B}c8s 15607 8 strict --bookers $B
  done
fi

if [ "$STAGE" = all ] || [ "$STAGE" = pg ]; then
  # PostgreSQL 18.6 twins, fresh, one at a time, after the KDS cells (no KDS server runs now).
  for P in "" r2- r3-; do
    vcell cellpg ${P}pg-s2-b1 ${P}pg-s2-b1 pg_scenario2_freight.py --organizations 300 --ships 30 --operations 300 --cargos 4000 --bookings 3000 --verify 100 --seed 1 --synchronous-commit on
    for N in 200 1000 10000; do
      for M in none all; do
        vcell cellpg ${P}pg-s3-n$N-$M ${P}pg-s3-n$N-$M pg_scenario3_library.py --loans $N --index-mode $M --seed 1
      done
    done
    vcell cellpg ${P}pg-s4 ${P}pg-s4 pg_scenario4_cabinopt_days.py --seed 1
    vcell cellpg ${P}pg-s1 ${P}pg-s1 pg_scenario1_backtest.py --seed 1 --verify
    vcell cellpg ${P}pg-s1s-n252 ${P}pg-s1s-n252 pg_scenario1_backtest.py --seed 1 --verify --years 1 --symbols 1 --exchanges 1
    vcell cellpg ${P}pg-s1s-n1008 ${P}pg-s1s-n1008 pg_scenario1_backtest.py --seed 1 --verify --years 1 --symbols 4 --exchanges 2
    vcell cellpg ${P}pg-s1s-n10080 ${P}pg-s1s-n10080 pg_scenario1_backtest.py --seed 1 --verify --years 5 --symbols 8 --exchanges 2
  done
fi

status "ALL DONE"
touch "$RUN/all.done"
