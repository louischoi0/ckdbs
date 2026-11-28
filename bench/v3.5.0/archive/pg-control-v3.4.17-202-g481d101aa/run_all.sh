#!/bin/bash
# PostgreSQL 18 control for scenarios 1-4: the v3.3.0-33-g9682b725 run's PostgreSQL cells, unchanged
# (bench/v3.3.0/archive/run-v3.3.0-33-g9682b725/run_all.sh:226-239), three rounds, one cell at a time.
# Idempotent: a cell with $RUN/done/<tag> is skipped, so a restart resumes.
set -uo pipefail
source /home/ukjin/bench-runs/pg-control-v3.4.17-202-g481d101aa/lib.sh
echo $$ > "$RUN/master.pid"
[ -e "$RUN/start-time.txt" ] || date -u +%FT%TZ > "$RUN/start-time.txt"
git -C "$REPO" diff --stat 481d101aa main -- tools/ > "$RUN/logs/tools-drift-vs-main.txt" 2>&1
status "pg up"
if ! pg_up; then tl "== PG FAILED TO START $(date -u +%FT%TZ) =="; touch "$RUN/flag/pg-start-failed"; exit 1; fi
if [ ! -e "$RUN/conf/pg-settings.txt" ]; then
    podman exec "$PGNAME" psql -h 127.0.0.1 -p "$PGPORT" -U postgres -d "$PGDB" -Atc "select version()" > "$RUN/conf/pg-version.txt" 2>&1
    podman exec "$PGNAME" psql -h 127.0.0.1 -p "$PGPORT" -U postgres -d "$PGDB" -Atc "select name, setting, source from pg_settings where source <> 'default' order by 1" > "$RUN/conf/pg-nondefault.txt" 2>&1
    podman exec "$PGNAME" psql -h 127.0.0.1 -p "$PGPORT" -U postgres -d "$PGDB" -Atc "show all" > "$RUN/conf/pg-settings.txt" 2>&1
    pm=$(podman inspect -f "{{.State.Pid}}" "$PGNAME"); for p in $pm $(pgrep -P $pm); do echo "$(taskset -cp $p) $(tr "\0" " " < /proc/$p/cmdline)"; done > "$RUN/conf/pg-affinity.txt" 2>&1
    podman image inspect "$PGIMG" --format '{{.Id}} {{.Created}}' > "$RUN/conf/pg-image.txt" 2>&1
fi
for P in "" r2- r3-; do
    cellpg ${P}pg-s2-b1 pg_scenario2_freight.py --organizations 300 --ships 30 --operations 300 --cargos 4000 --bookings 3000 --verify 100 --seed 1 --synchronous-commit on
    for N in 200 1000 10000; do
        for M in none all; do
            cellpg ${P}pg-s3-n$N-$M pg_scenario3_library.py --user postgres --loans $N --index-mode $M --seed 1
        done
    done
    cellpg ${P}pg-s4 pg_scenario4_cabinopt_days.py --seed 1
    cellpg ${P}pg-s1 pg_scenario1_backtest.py --seed 1 --verify
    cellpg ${P}pg-s1s-n252 pg_scenario1_backtest.py --seed 1 --verify --years 1 --symbols 1 --exchanges 1
    cellpg ${P}pg-s1s-n1008 pg_scenario1_backtest.py --seed 1 --verify --years 1 --symbols 4 --exchanges 2
    cellpg ${P}pg-s1s-n10080 pg_scenario1_backtest.py --seed 1 --verify --years 5 --symbols 8 --exchanges 2
done
pg_down
date -u +%FT%TZ > "$RUN/end-time.txt"
status "ALL DONE"
touch "$RUN/all.done"
