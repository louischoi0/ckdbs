#!/usr/bin/env bash
# Lifecycle for the scratch PostgreSQL cluster that tools/pg_benchmark.py
# drives. Nothing here is part of the engine; it exists so the comparison
# baseline is reproducible instead of "whatever was on the box".
#
# The cluster is deliberately *not* the distro's default one: it lives under
# $PGDATA below, listens on port 15433 (ckdbs uses 15432), and runs as the
# invoking user with trust auth on loopback only. That keeps it disposable -
# `pg_setup.sh destroy --yes` removes it and nothing else.
#
#   ./tools/pg_setup.sh init            initdb, start, create the bench db
#   ./tools/pg_setup.sh start|stop|status|restart
#   ./tools/pg_setup.sh psql [SQL]      psql into the bench db
#   ./tools/pg_setup.sh timing on|off   log_min_duration_statement 0|-1,
#                                       which is what pg_benchmark.py's
#                                       --server-log reads back
#   ./tools/pg_setup.sh destroy --yes   stop and delete the data directory
#
# Override with environment variables: PGDATA, PGPORT, PGDATABASE, PGLOG.
# Tuning knobs are left at PostgreSQL defaults on purpose - a baseline tuned
# by hand is not a baseline. The one exception is logging, which `timing`
# toggles, and even that is off by default because it costs write throughput.
set -euo pipefail

PGROOT=${PGROOT:-$HOME/pg-bench}
PGDATA=${PGDATA:-$PGROOT/data}
PGLOG=${PGLOG:-$PGROOT/pg.log}
PGPORT=${PGPORT:-15433}
PGDATABASE=${PGDATABASE:-bench}
PGHOST=${PGHOST:-127.0.0.1}
PGUSER=${PGUSER:-$(id -un)}

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "$1 not found. Install the server package first, e.g." >&2
        echo "  sudo dnf install -y postgresql17-server postgresql17" >&2
        exit 1
    }
}

do_init() {
    need initdb
    if [ -f "$PGDATA/PG_VERSION" ]; then
        echo "cluster already initialized at $PGDATA (PG $(cat "$PGDATA/PG_VERSION"))"
    else
        mkdir -p "$PGROOT"
        # --locale=C: collation cost is not what we are measuring, and C is
        # the one locale every host has.
        initdb -D "$PGDATA" --username="$PGUSER" --auth=trust \
               --encoding=UTF8 --locale=C >/dev/null
        {
            echo ""
            echo "# --- tools/pg_setup.sh ---"
            echo "port = $PGPORT"
            echo "listen_addresses = '$PGHOST'"
            echo "unix_socket_directories = '$PGDATA'"
            echo "logging_collector = off"
            echo "log_min_duration_statement = -1"
            echo "log_line_prefix = '%m [%p] '"
        } >>"$PGDATA/postgresql.conf"
        echo "initialized $PGDATA on port $PGPORT"
    fi
    do_start
    if ! psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d postgres -tAc \
            "SELECT 1 FROM pg_database WHERE datname = '$PGDATABASE'" | grep -q 1; then
        createdb -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" "$PGDATABASE"
        echo "created database $PGDATABASE"
    fi
    echo "ready: python3 tools/pg_benchmark.py --port $PGPORT --database $PGDATABASE"
}

do_start() {
    need pg_ctl
    if pg_ctl -D "$PGDATA" status >/dev/null 2>&1; then
        echo "already running"
    else
        pg_ctl -D "$PGDATA" -l "$PGLOG" start
    fi
}

do_stop() {
    need pg_ctl
    pg_ctl -D "$PGDATA" -m fast stop
}

do_timing() {
    case "${1:-}" in
        on)  value=0 ;;
        off) value=-1 ;;
        *)   echo "usage: pg_setup.sh timing on|off" >&2; exit 2 ;;
    esac
    psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d postgres \
         -c "ALTER SYSTEM SET log_min_duration_statement = $value" \
         -c "SELECT pg_reload_conf()" >/dev/null
    echo "log_min_duration_statement = $value (server log: $PGLOG)"
}

case "${1:-}" in
    init)    do_init ;;
    start)   do_start ;;
    stop)    do_stop ;;
    restart) do_stop || true; do_start ;;
    status)  pg_ctl -D "$PGDATA" status ;;
    timing)  shift; do_timing "${1:-}" ;;
    psql)    shift; exec psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" "$@" ;;
    log)     exec tail -n "${2:-40}" "$PGLOG" ;;
    destroy)
        [ "${2:-}" = "--yes" ] || { echo "refusing: pg_setup.sh destroy --yes" >&2; exit 2; }
        pg_ctl -D "$PGDATA" -m immediate stop >/dev/null 2>&1 || true
        rm -rf "$PGDATA"
        echo "removed $PGDATA"
        ;;
    *)
        sed -n '1,30p' "$0" | sed 's/^# \{0,1\}//'
        exit 2
        ;;
esac
