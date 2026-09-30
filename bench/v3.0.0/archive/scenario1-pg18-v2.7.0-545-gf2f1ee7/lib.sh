#!/bin/bash
# Shared helpers for the PostgreSQL 18.6 floor run at f2f1ee7. Source, do not run.
RUN=/home/cdkbs/bench-runs/pg18-f2f1ee7
TOOLS=/home/cdkbs/ckdbs/.claude/worktrees/bench-pg-floor/tools
export PGROOT=/home/cdkbs/pg-bench-18
export PGPORT=15700
export PGDATA=$PGROOT/data
export PGLOG=$PGROOT/pg.log
PSQL="psql -h 127.0.0.1 -p $PGPORT -U $(id -un)"

# want TAG: true unless CELLS_ONLY (space-separated tags) is set and omits TAG
want() {
    [ -z "${CELLS_ONLY:-}" ] && return 0
    for t in $CELLS_ONLY; do [ "$t" = "$1" ] && return 0; done
    return 1
}

# foreign() lists build/test processes that are not part of this run
foreign() {
    pgrep -a -f "cc1plus|cmake --build|ctest" | grep -v pgrep | grep -v pg18-f2f1ee7 | grep -v "shell-snapshots"
}

# precheck TAG: wait (max 900 s) for the host to be free of build/test processes, then record
precheck() {
    local waited=0
    while { [ -n "$(foreign)" ] || awk "BEGIN{exit !($(cut -d" " -f1 /proc/loadavg) > 1.6)}"; } && [ $waited -lt 900 ]; do sleep 5; waited=$((waited+5)); done
    {
        echo "time: $(date -u +%FT%TZ)"
        echo "waited_for_quiet_s: $waited"
        echo "loadavg: $(cat /proc/loadavg)"
        echo "pgrep cc1plus|cmake --build|ctest:"
        foreign || echo "  none"
    } > "$RUN/logs/$1.precheck" 2>&1
}

# fresh cluster per cell: destroy, initdb, start, create db bench
fresh_pg() {  # tag
    bash $TOOLS/pg_setup.sh destroy --yes > /dev/null 2>&1
    bash $TOOLS/pg_setup.sh init > "$RUN/logs/$1.pginit" 2>&1 || return 1
    $PSQL -d bench -Atc "SELECT name, setting, source FROM pg_settings WHERE source <> 'default' ORDER BY name" > "$RUN/logs/$1.pg_settings"
    return 0
}

stop_pg() {  # tag
    bash $TOOLS/pg_setup.sh stop >> "$RUN/logs/$1.pginit" 2>&1
    cp "$PGLOG" "$RUN/logs/$1.pglog" 2>/dev/null
    bash $TOOLS/pg_setup.sh destroy --yes >> "$RUN/logs/$1.pginit" 2>&1
}
