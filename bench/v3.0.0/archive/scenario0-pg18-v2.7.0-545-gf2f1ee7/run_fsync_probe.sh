#!/bin/bash
# Instrumented extra cells (not part of any median): the same s0 and s2 pg-on cell,
# then pg_stat_io / pg_stat_wal read BEFORE the cluster stops, to count WAL fsyncs
# against commits. Counters cover the whole cluster life (load included).
set -uo pipefail
source /home/cdkbs/bench-runs/pg18-f2f1ee7/lib.sh
bash $RUN/monitor.sh > /dev/null 2>&1 &
S0_ARGS="--users 100 --accounts-per-user 3 --assets 30 --traders 8 --txn-per-user 50 --verify 200 --seed 1 --sync"
S2_ARGS="--organizations 300 --ships 30 --operations 300 --cargos 4000 --bookings 3000 --verify 100 --seed 1"
probe() {
    $PSQL -d bench -Atc "select 'wal io: ' || writes || ' writes, ' || coalesce(fsyncs,0) || ' fsyncs' from pg_stat_io where object='wal' and context='normal' and backend_type='client backend'"
    $PSQL -d bench -Atc "select 'wal io all backends: ' || sum(writes) || ' writes, ' || sum(coalesce(fsyncs,0)) || ' fsyncs' from pg_stat_io where object='wal'"
    $PSQL -d bench -Atc "select 'pg_stat_wal: records=' || wal_records || ' fpi=' || wal_fpi || ' bytes=' || wal_bytes from pg_stat_wal"
    $PSQL -d bench -Atc "select 'xact_commit=' || xact_commit || ' xact_rollback=' || xact_rollback from pg_stat_database where datname='bench'"
}
for c in s0 s2; do
    TAG=$c-pg-on-fsyncprobe
    echo "== $TAG start $(date -u +%FT%TZ) ==" >> "$RUN/timeline"
    precheck "$TAG"
    fresh_pg "$TAG" || exit 1
    if [ $c = s0 ]; then
        python3 "$TOOLS/pg_scenario0_stockmarket.py" --port "$PGPORT" --user "$(id -un)" $S0_ARGS --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    else
        python3 "$RUN/pg_s2_shim.py" --port "$PGPORT" --user "$(id -un)" $S2_ARGS --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    fi
    echo "$?" > "$RUN/logs/$TAG.rc"
    probe > "$RUN/logs/$TAG.stats" 2>&1
    stop_pg "$TAG"
    echo "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) ==" >> "$RUN/timeline"
done
kill "$(cat $RUN/monitor.pid)" 2>/dev/null
touch $RUN/probe.done
