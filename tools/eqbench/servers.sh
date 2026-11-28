#!/bin/bash
# The two control servers for tools/eqbench, each in a fresh container at its
# defaults (bench/README.md, "Three engines on one client", "The Oracle
# control"), host networking so no port forwarder sits in their path.
#
#   servers.sh pg-up      <port> [profile]   PostgreSQL 18; `profile` adds the
#                                            instruments EN-Q8's breakdown reads
#   servers.sh ora-up     <secret-file>      Oracle Database Free on 1521
#   servers.sh pg-down | ora-down
#   servers.sh pin        <name> <cpus>      every process of a container on <cpus>
#   servers.sh cgroup     <name>             the container's cgroup directory
#
# The Oracle password is generated into <secret-file> (mode 600) on first use
# and never printed; the harness reads it from the environment (ORA_PW).
set -euo pipefail
PG_IMG=docker.io/library/postgres:18
ORA_IMG=docker.io/gvenzl/oracle-free:slim
PG_NAME=eq-pg
ORA_NAME=eq-ora

port_free() {
  if ss -ltn | awk '{print $4}' | grep -qE "[:.]$1\$"; then
    echo "port $1 is held" >&2
    exit 1
  fi
}

case "${1:-}" in
pg-up)
  port=$2
  port_free "$port"
  podman rm -f $PG_NAME >/dev/null 2>&1 || true
  extra=()
  if [ "${3:-}" = profile ]; then
    extra=(-c shared_preload_libraries=pg_stat_statements -c pg_stat_statements.track_planning=on
           -c track_io_timing=on)
  fi
  podman run -d --name $PG_NAME --network host -e POSTGRES_HOST_AUTH_METHOD=trust -e POSTGRES_DB=bench \
    -e POSTGRES_INITDB_ARGS="--encoding=UTF8 --locale=C" $PG_IMG \
    -c port="$port" -c listen_addresses=127.0.0.1 "${extra[@]}" >/dev/null
  for _ in $(seq 1 90); do
    podman exec $PG_NAME pg_isready -q -h 127.0.0.1 -p "$port" -d bench 2>/dev/null && break
    sleep 1
  done
  sleep 2
  podman exec $PG_NAME psql -h 127.0.0.1 -p "$port" -U postgres -d bench -Atc "select version()"
  ;;
ora-up)
  secret=$2
  port_free 1521
  if [ ! -s "$secret" ]; then
    (umask 077; printf 'ORA_PW=Eq%s\n' "$(head -c 24 /dev/urandom | base64 | tr -dc 'A-Za-z0-9' | head -c 20)" > "$secret")
  fi
  # shellcheck disable=SC1090
  . "$secret"
  podman rm -f $ORA_NAME >/dev/null 2>&1 || true
  podman run -d --name $ORA_NAME --network host -e ORACLE_PASSWORD="$ORA_PW" -e APP_USER=bench \
    -e APP_USER_PASSWORD="$ORA_PW" $ORA_IMG >/dev/null
  # Read the log whole each time: `podman logs | grep -q` dies of SIGPIPE
  # under pipefail once grep has matched, and reads as "not ready".
  ready() { [[ "$(podman logs $ORA_NAME 2>&1)" == *"DATABASE IS READY TO USE"* ]]; }
  for _ in $(seq 1 600); do ready && break; sleep 1; done
  ready || { echo "Oracle did not come up" >&2; exit 1; }
  podman exec $ORA_NAME bash -c 'echo "select banner_full from v\$version;" | sqlplus -s / as sysdba' | grep -i oracle | head -1
  ;;
kds-up)
  # kds-up <binary-copy> <dir> <port> <durability> <cpus>: a fresh data file
  # under <dir>, the server from the copy (bench/README.md rule 3), its pid on
  # stdout.
  bin=$2 dir=$3 port=$4 dur=$5 cpus=$6
  port_free "$port"
  rm -rf "$dir/data"; mkdir -p "$dir/data"
  { echo "data_file = $dir/data/kds.db"; echo "cores = 1"; echo "port = $port"; echo "durability = $dur"
    echo "log_dir = $dir/data"; echo "log_file = kds.log"; echo "log_level = warn"
    echo "buffer_pool_frames = 65536"; } > "$dir/kds.conf"
  taskset -c "$cpus" nohup "$bin" --config "$dir/kds.conf" > "$dir/data/stdout.log" 2>&1 &
  kpid=$!
  for _ in $(seq 1 150); do (exec 3<>/dev/tcp/127.0.0.1/"$port") 2>/dev/null && break; sleep 0.2; done
  kill -0 $kpid 2>/dev/null || { echo "kds did not start" >&2; exit 1; }
  echo $kpid > "$dir/pid"
  echo $kpid
  ;;
pg-down) podman rm -f $PG_NAME >/dev/null 2>&1 || true ;;
ora-down) podman rm -f $ORA_NAME >/dev/null 2>&1 || true ;;
pin)
  pid=$(podman inspect -f '{{.State.Pid}}' "$2")
  for p in $pid $(pgrep -P "$pid") $(pgrep -g "$(ps -o pgid= -p "$pid" | tr -d ' ')" 2>/dev/null); do
    taskset -a -cp "$3" "$p" >/dev/null 2>&1 || true
  done
  # Every process in the container's cgroup, the forked backends included.
  cg=$(podman inspect -f '{{.State.CgroupPath}}' "$2")
  for p in $(cat "/sys/fs/cgroup$cg/cgroup.procs" 2>/dev/null); do taskset -a -cp "$3" "$p" >/dev/null 2>&1 || true; done
  ;;
cgroup)
  echo "/sys/fs/cgroup$(podman inspect -f '{{.State.CgroupPath}}' "$2")"
  ;;
*)
  sed -n 2,16p "$0"; exit 2 ;;
esac
