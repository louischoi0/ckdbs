#!/bin/bash
# PostgreSQL control run for scenarios 1-4: helpers. Source, do not run.
# The gate ignores kds_server: another project's server (firestrike) runs permanently on this host.
# Drivers: tools/ snapshot at 481d101aa (v3.4.17-202-g481d101aa) in $RUN/src/tools.
# Server: a fresh docker.io/library/postgres:18 container per run, PostgreSQL defaults
# (port and listen address only), host network (no rootless port proxy), pinned to CPUs 0-7 by
# taskset after start (rootless cgroups here do not delegate cpuset, and podman's conmon does not
# inherit the caller's affinity): the postmaster and its children are set, and later backends inherit it;
# drivers pinned to CPUs 8-23 (bench-host-hybrid-cpu-pin-for-ab).
RUN=/home/ukjin/bench-runs/pg-control-v3.4.17-202-g481d101aa
TOOLS=$RUN/src/tools
REPO=/home/ukjin/workspaces/ckdbs
PGIMG=docker.io/library/postgres:18
PGNAME=kds-pg-control
PGPORT=15434   # 15433 is held by the sql-tuning-kdbs container since 2026-11-27 14:02 UTC (attempt 1 failed on it)
PGDB=bench
export PGUSER=postgres
export PYTHONPATH=$TOOLS
mkdir -p "$RUN/logs" "$RUN/json" "$RUN/done" "$RUN/flag" "$RUN/conf"

status() { echo "$(date -u +%FT%TZ) $*" > "$RUN/status"; }   # the watchdog reads its mtime
tl() { echo "$*" >> "$RUN/timeline"; }

busy_pct() {  # % of all CPUs busy over 3 s (every process)
    local u n s i w q sq st i1 t1 i2 t2
    read -r _ u n s i w q sq st _ < /proc/stat; i1=$((i + w)); t1=$((u + n + s + i + w + q + sq + st))
    sleep 3
    read -r _ u n s i w q sq st _ < /proc/stat; i2=$((i + w)); t2=$((u + n + s + i + w + q + sq + st))
    echo $(( 100 * ( (t2 - t1) - (i2 - i1) ) / (t2 - t1) ))
}
foreign_procs() { pgrep -a -f "cc1plus|cmake --build|ctest|kds_tests" | grep -v pgrep || true; }

quiet_gate() {  # tag: wait (up to 3600 s) for no build/test process and <= 8 % busy
    local TAG=$1 waited=0 b procs
    while :; do
        procs=$(foreign_procs); b=$(busy_pct)
        if [ -z "$procs" ] && [ "$b" -le 8 ]; then break; fi
        status "$TAG waiting for a quiet host: busy=${b}% procs=$(echo -n "$procs" | grep -c .)"
        waited=$((waited + 3))
        if [ "$waited" -ge 3600 ]; then touch "$RUN/flag/$TAG.gate-timeout"; break; fi
        sleep 7; waited=$((waited + 7))
    done
    echo "$waited" > "$RUN/logs/$TAG.gatewait"
}
precheck() {
    quiet_gate "$1"
    { echo "time: $(date -u +%FT%TZ)"; echo "loadavg: $(cat /proc/loadavg)"
      echo "gate wait s: $(cat "$RUN/logs/$1.gatewait")"; echo "foreign processes:"
      foreign_procs | sed 's/^/  /'; [ -z "$(foreign_procs)" ] && echo "  none"; } > "$RUN/logs/$1.precheck" 2>&1
}

pg_ready() { podman exec "$PGNAME" pg_isready -q -h 127.0.0.1 -p "$PGPORT" -d "$PGDB" 2>/dev/null; }
pg_up() {  # start the run's container if it is not running; fresh cluster on first start
    if ! podman container exists "$PGNAME"; then
        if ss -ltn | grep -q "127.0.0.1:$PGPORT "; then echo "port $PGPORT already held: $(ss -ltnp | grep ":$PGPORT ")" >> "$RUN/logs/pg.container"; return 1; fi
        podman run -d --name "$PGNAME" --network host \
            -e POSTGRES_HOST_AUTH_METHOD=trust -e POSTGRES_DB="$PGDB" \
            -e POSTGRES_INITDB_ARGS="--encoding=UTF8 --locale=C" \
            "$PGIMG" -c port="$PGPORT" -c listen_addresses=127.0.0.1 >> "$RUN/logs/pg.container" 2>&1 || return 1
    elif [ "$(podman inspect -f '{{.State.Running}}' "$PGNAME")" != true ]; then
        podman start "$PGNAME" >> "$RUN/logs/pg.container" 2>&1 || return 1
    fi
    local k
    for k in $(seq 1 60); do pg_ready && { pg_pin; return 0; }; sleep 1; done
    return 1
}
pg_pin() {  # the postmaster and every process under it to CPUs 0-7
    local pm c
    pm=$(podman inspect -f '{{.State.Pid}}' "$PGNAME") || return 1
    taskset -a -cp 0-7 "$pm" > /dev/null
    for c in $(pgrep -P "$pm"); do taskset -a -cp 0-7 "$c" > /dev/null; done
}
pg_down() { podman logs "$PGNAME" > "$RUN/logs/pg.serverlog" 2>&1; podman rm -f "$PGNAME" >> "$RUN/logs/pg.container" 2>&1; }

cellpg() {  # tag script [args...]  (fresh table names via --suffix)
    local TAG=$1 SCRIPT=$2; shift 2
    [ -e "$RUN/done/$TAG" ] && return
    status "$TAG start"; tl "== $TAG start $(date -u +%FT%TZ) =="
    precheck "$TAG"
    pg_ready || pg_up || { tl "== $TAG PG NOT READY =="; return; }
    status "$TAG running"
    taskset -c 8-23 python3 "$TOOLS/$SCRIPT" --port "$PGPORT" --database "$PGDB" --suffix "${TAG//-/_}" "$@" \
        --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    tl "== $TAG end $(date -u +%FT%TZ) rc=$(cat "$RUN/logs/$TAG.rc") =="
    status "$TAG end"; touch "$RUN/done/$TAG"
}
