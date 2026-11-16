#!/usr/bin/env python3
"""CP-S0 server launcher: one kds_server per workdir, started from a copy of
the binary, stopped by SIGTERM, every fact a results file owes written to a
JSON sidecar beside the data file.

Used by every S0 cell (run_m1m4.sh first). Nothing here is the engine's: it
reads `kds.conf.sample` for the key names, `src/server/main.cpp` for the
banner and `include/kds/server/stop_signal.hpp` for the stop path, all at
cc54a296.

    srv.py start --binary BIN --workdir DIR --port P [--debug-text-port D]
                 [--cores N] [--taskset 2-5] [--reactor-cpus 2,3,4,5]
                 [--durability relaxed] [--buffer-pool-frames 65536]
                 [--ash on|off] [--label NAME] [--allow-tmpfs] [--timeout 60]
    srv.py stop   --workdir DIR [--timeout 60]
    srv.py status --workdir DIR

What `start` does, in order:

1. Refuses a workdir whose filesystem is tmpfs (`bench/README.md` rule 2: a
   block device, never tmpfs, for the data file) unless `--allow-tmpfs`;
   records `df -T` of the workdir either way. On this box `/tmp` (and so
   the session scratchpad) is tmpfs; `/var/tmp` and `/home` are ext4.
2. Refuses a workdir that already holds a data file: every cell starts a
   fresh server on a fresh volume, so a reused directory is a mistake.
3. Copies the binary into the workdir and hashes the copy (`bench/README.md`
   rule 3) - the server is started from the copy, never from a build tree
   another session may rebuild mid-run.
4. Writes `kds.conf` with the keys of `kds.conf.sample`: `data_file` (:19),
   `wal_dir` (:23, default `<data_file>.wal`), `cores` (:74),
   `reactor_cpus` (:82), `buffer_pool_frames` (:99, required),
   `port` (:139, KWP/1), `listen_address` (:145), `durability` (:235),
   `debug_text_port` (:314, the newline debug surface, 0 = closed),
   `kwp_port` (:321, the v0 load endpoint, 0 = closed), `ash_enabled`
   (:445), `log_dir`/`log_file` (:678-679), `log_level` (:685).
5. Starts `taskset -c CPUS <copy> --config <workdir>/kds.conf`
   (`main.cpp:221-237` applies the file before any flag) detached in its own
   session, stdout and stderr to `<workdir>/server.out`.
6. Waits for the banner `listening on <addr>:<port>` (`main.cpp:467-477`,
   flushed with `std::endl`) **and then** for a TCP connect to succeed: the
   banner is printed *before* `Serve()` binds the port
   (`main.cpp:479` -> `expeditor.cpp:1586-1591` -> `TcpServer::Listen` at
   `expeditor.cpp:1718`), so the line alone is not readiness.
7. Writes `<workdir>/server.json`: binary paths and sha256 (source and copy),
   pid, ports, config, `/proc/loadavg` and
   `pgrep -a -f "cc1plus|cmake --build|ctest"` at start (`bench/README.md`
   rule 4), `df -T` of the workdir, start time UTC, the CPU mask asked for.

`stop` sends SIGTERM: `StopSignal::Install` (`main.cpp:446`,
`stop_signal.hpp`) turns SIGTERM/SIGINT into a signalfd the reactor reads,
and the handler at `expeditor.cpp:2018-2040` runs the same shutdown tail a
client's `STOP` does - final sync and checkpoint, then `stopped; N pages
persisted` on stdout (`main.cpp:484`). `scripts/stop.sh` instead sends
`STOP` through `tools/ckdbs_cli.py` (KWP on the default port); SIGTERM
needs no client and is what a process manager would send, so it is the one
used here. A server still alive at the timeout gets SIGKILL and the
sidecar says so (`forced: true`), which makes that run's shutdown
checkpoint and clean close unreliable - treat such a cell as suspect.

**Pinning.** `--taskset` restricts every thread of the process (reactors,
the WAL writer, the data-sync thread). But a reactor pins itself with
`pthread_setaffinity_np` (`expeditor.cpp:1460-1470`), and with no
`reactor_cpus` key peer reactor k goes to CPU k and core 0 runs unpinned
(`expeditor.cpp:2003-2017`, `kds.conf.sample:76-82`) - which would move
reactors 1..3 to CPUs 1..3, outside a `2-5` mask. So when `--taskset` is
given and `--reactor-cpus` is not, this launcher writes `reactor_cpus` as
the first `cores` CPUs of the taskset list. `CheckReactorCpus`
(`expeditor.cpp:142-165`) refuses a list that is not exactly `cores`
distinct CPUs below `hardware_concurrency()` (`expeditor.cpp:807`).
"""

import argparse
import datetime as _dt
import hashlib
import json
import os
import shutil
import signal
import socket
import stat
import subprocess
import sys
import time

SIDECAR = "server.json"
CONF = "kds.conf"
OUT = "server.out"
DATA_FILE = "kds.db"
LOG_FILE = "kdb.log"
BINARY_COPY = "kds_server"
READY_LINE = "listening on "
STOPPED_LINE = "stopped;"


def utc_now():
    return _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def host_state():
    """`bench/README.md` rule 4, the same two readings
    `tools/lock_contention_benchmark.py:94-106` takes per cell."""
    try:
        with open("/proc/loadavg") as f:
            load = f.read().strip()
    except OSError:
        load = "unavailable"
    try:
        busy = subprocess.run(["pgrep", "-a", "-f", "cc1plus|cmake --build|ctest"],
                              capture_output=True, text=True, timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        busy = "unavailable"
    return {"loadavg": load, "competing": busy.splitlines() if busy else [], "at": utc_now()}


def df_t(path):
    """`df -T` of the workdir, kept verbatim and parsed for the type
    (`bench/README.md` rule 2 says name the device in the results file)."""
    try:
        out = subprocess.run(["df", "-T", path], capture_output=True, text=True,
                             timeout=10).stdout
    except (OSError, subprocess.SubprocessError) as e:
        return {"raw": f"unavailable: {e}", "fstype": None, "source": None}
    lines = out.strip().splitlines()
    fstype = source = mount = None
    if len(lines) >= 2:
        cols = lines[-1].split()
        if len(cols) >= 7:
            source, fstype, mount = cols[0], cols[1], cols[-1]
    return {"raw": out, "fstype": fstype, "source": source, "mounted_on": mount}


def parse_cpu_list(text):
    """`2-5,7` -> [2, 3, 4, 5, 7] (taskset's spelling)."""
    cpus = []
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            lo, hi = part.split("-", 1)
            cpus.extend(range(int(lo), int(hi) + 1))
        else:
            cpus.append(int(part))
    return cpus


def write_conf(path, a, workdir, reactor_cpus):
    data_file = os.path.join(workdir, DATA_FILE)
    lines = [
        "# generated by cp-s0/m1m4/srv.py - every key named in kds.conf.sample at cc54a296",
        f"data_file = {data_file}",
        f"wal_dir = {data_file}.wal",
        f"cores = {a.cores}",
    ]
    if reactor_cpus:
        lines.append("reactor_cpus = " + ",".join(str(c) for c in reactor_cpus))
    lines += [
        f"buffer_pool_frames = {a.buffer_pool_frames}",
        f"port = {a.port}",
        "listen_address = 127.0.0.1",
        f"debug_text_port = {a.debug_text_port}",
        f"kwp_port = {a.kwp_port}",
        f"durability = {a.durability}",
        f"ash_enabled = {a.ash}",
        f"log_dir = {workdir}",
        f"log_file = {LOG_FILE}",
        f"log_level = {a.log_level}",
    ]
    if a.conf_extra:
        lines.append("# --conf-extra lines follow")
        lines.extend(a.conf_extra)
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    return lines


def port_open(port, host="127.0.0.1", timeout=0.5):
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def pid_alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def pid_is_ours(pid, binary_copy):
    """The pid still names our server: its cmdline carries the copied binary.
    A reused pid after a crash must not be signalled."""
    try:
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            argv = f.read().split(b"\0")
    except OSError:
        return False
    return any(binary_copy.encode() in part for part in argv)


def load_sidecar(workdir):
    path = os.path.join(workdir, SIDECAR)
    with open(path) as f:
        return json.load(f), path


def save_sidecar(path, side):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(side, f, indent=2)
    os.replace(tmp, path)


def tail(path, n=30):
    try:
        with open(path, errors="replace") as f:
            return "".join(f.readlines()[-n:])
    except OSError:
        return ""


# ---- start -----------------------------------------------------------------

def cmd_start(a):
    workdir = os.path.abspath(a.workdir)
    os.makedirs(workdir, exist_ok=True)

    fs = df_t(workdir)
    if fs["fstype"] == "tmpfs" and not a.allow_tmpfs:
        sys.exit(f"srv.py start: {workdir} is on tmpfs ({fs['source']}); bench/README.md rule 2 "
                 f"wants a block device for the data file. Use an ext4 path (on this box "
                 f"/var/tmp and /home are ext4) or pass --allow-tmpfs to record the violation.")
    if fs["fstype"] is None:
        print(f"srv.py start: warning: could not read the filesystem type of {workdir}",
              file=sys.stderr)

    data_file = os.path.join(workdir, DATA_FILE)
    if os.path.exists(data_file) or os.path.exists(os.path.join(workdir, SIDECAR)):
        sys.exit(f"srv.py start: {workdir} already holds a server (kds.db or server.json); "
                 f"every cell starts fresh - pick a new workdir")

    binary_src = os.path.abspath(a.binary)
    if not (os.path.isfile(binary_src) and os.access(binary_src, os.X_OK)):
        sys.exit(f"srv.py start: {binary_src} is not an executable file")
    binary_copy = os.path.join(workdir, BINARY_COPY)
    shutil.copy2(binary_src, binary_copy)
    os.chmod(binary_copy, os.stat(binary_copy).st_mode | stat.S_IXUSR)
    sha_src = sha256_of(binary_src)
    sha_copy = sha256_of(binary_copy)
    if sha_src != sha_copy:
        sys.exit("srv.py start: the copied binary's hash differs from the source's; "
                 "the source changed while it was copied")

    taskset_cpus = parse_cpu_list(a.taskset) if a.taskset else None
    reactor_cpus = parse_cpu_list(a.reactor_cpus) if a.reactor_cpus else None
    reactor_note = None
    if reactor_cpus is None and taskset_cpus is not None:
        if len(taskset_cpus) >= a.cores:
            reactor_cpus = taskset_cpus[:a.cores]
            reactor_note = ("reactor_cpus derived from --taskset: the default map puts peer "
                            "reactor k on CPU k (expeditor.cpp:2003-2017), outside the mask")
        else:
            reactor_note = (f"--taskset names {len(taskset_cpus)} CPUs for {a.cores} cores; "
                            f"reactor_cpus left unset, reactors will pin themselves to CPUs "
                            f"0..{a.cores - 1} regardless of the mask")
            print("srv.py start: warning: " + reactor_note, file=sys.stderr)
    if reactor_cpus is not None and len(reactor_cpus) != a.cores:
        sys.exit(f"srv.py start: reactor_cpus names {len(reactor_cpus)} CPUs for {a.cores} cores")

    conf_path = os.path.join(workdir, CONF)
    conf_lines = write_conf(conf_path, a, workdir, reactor_cpus)

    for p, what in ((a.port, "port"), (a.debug_text_port, "debug_text_port"),
                    (a.kwp_port, "kwp_port")):
        if p and port_open(p):
            sys.exit(f"srv.py start: {what} {p} already accepts connections; "
                     f"something else is listening (bench/README.md rule 5)")

    argv = []
    if taskset_cpus is not None:
        argv += ["taskset", "-c", a.taskset]
    argv += [binary_copy, "--config", conf_path]

    before = host_state()
    out_path = os.path.join(workdir, OUT)
    out_fd = open(out_path, "ab", buffering=0)
    started_at = utc_now()
    t0 = time.monotonic()
    try:
        proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=out_fd, stderr=out_fd,
                                cwd=workdir, start_new_session=True)
    finally:
        out_fd.close()

    side = {
        "label": a.label or os.path.basename(workdir),
        "workdir": workdir,
        "binary_source": binary_src,
        "binary_source_sha256": sha_src,
        "binary_copy": binary_copy,
        "binary_copy_sha256": sha_copy,
        "argv": argv,
        "pid": proc.pid,
        "port": a.port,
        "debug_text_port": a.debug_text_port,
        "kwp_port": a.kwp_port,
        "cores": a.cores,
        "taskset": a.taskset,
        "reactor_cpus": reactor_cpus,
        "reactor_cpus_note": reactor_note,
        "durability": a.durability,
        "buffer_pool_frames": a.buffer_pool_frames,
        "ash_enabled": a.ash,
        "conf_path": conf_path,
        "conf": conf_lines,
        "data_file": data_file,
        "wal_dir": data_file + ".wal",
        "stdout_path": out_path,
        "df_T": fs,
        "host_at_start": before,
        "started_at_utc": started_at,
        "ready_at_utc": None,
        "ready_after_s": None,
        "stopped_at_utc": None,
        "stop": None,
    }
    side_path = os.path.join(workdir, SIDECAR)
    save_sidecar(side_path, side)

    # Readiness: the banner, then a connect. The banner precedes the bind.
    deadline = time.monotonic() + a.timeout
    saw_banner = False
    while time.monotonic() < deadline:
        rc = proc.poll()
        if rc is not None:
            side["stop"] = {"how": "exited-at-start", "exit_code": rc, "at": utc_now()}
            save_sidecar(side_path, side)
            sys.exit(f"srv.py start: server exited with {rc} before it was ready; "
                     f"{out_path} ends:\n{tail(out_path)}")
        if not saw_banner:
            saw_banner = READY_LINE in tail(out_path, 40)
        if saw_banner and port_open(a.port):
            side["ready_at_utc"] = utc_now()
            side["ready_after_s"] = round(time.monotonic() - t0, 3)
            save_sidecar(side_path, side)
            print(json.dumps({"pid": proc.pid, "port": a.port, "workdir": workdir,
                              "ready_after_s": side["ready_after_s"],
                              "binary_sha256": sha_copy}))
            return 0
        time.sleep(0.05)

    side["stop"] = {"how": "not-ready-at-timeout", "at": utc_now()}
    save_sidecar(side_path, side)
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except OSError:
        pass
    sys.exit(f"srv.py start: no '{READY_LINE}' banner and open port within {a.timeout}s; "
             f"killed. {out_path} ends:\n{tail(out_path)}")


# ---- stop / status ---------------------------------------------------------

def cmd_stop(a):
    workdir = os.path.abspath(a.workdir)
    side, side_path = load_sidecar(workdir)
    pid = side["pid"]
    if side.get("stopped_at_utc"):
        print(json.dumps({"pid": pid, "already_stopped": True, "stop": side["stop"]}))
        return 0
    if not (pid_alive(pid) and pid_is_ours(pid, side["binary_copy"])):
        side["stopped_at_utc"] = utc_now()
        side["stop"] = {"how": "gone-before-stop", "at": side["stopped_at_utc"],
                        "stdout_tail": tail(side["stdout_path"])}
        side["host_at_stop"] = host_state()
        save_sidecar(side_path, side)
        print(json.dumps({"pid": pid, "gone_before_stop": True}))
        return 1

    t0 = time.monotonic()
    os.kill(pid, signal.SIGTERM)
    how = "SIGTERM"
    deadline = time.monotonic() + a.timeout
    while pid_alive(pid) and pid_is_ours(pid, side["binary_copy"]):
        if time.monotonic() > deadline:
            os.kill(pid, signal.SIGKILL)
            how = "SIGTERM-then-SIGKILL"
            # A killed server leaves no `stopped;` line; give the kernel a moment.
            for _ in range(100):
                if not pid_alive(pid):
                    break
                time.sleep(0.05)
            break
        time.sleep(0.05)

    stdout_tail = tail(side["stdout_path"])
    side["stopped_at_utc"] = utc_now()
    side["stop"] = {
        "how": how,
        "forced": how != "SIGTERM",
        "took_s": round(time.monotonic() - t0, 3),
        "clean_banner": STOPPED_LINE in stdout_tail,
        "stdout_tail": stdout_tail,
    }
    side["host_at_stop"] = host_state()
    try:
        side["data_file_bytes"] = os.path.getsize(side["data_file"])
    except OSError:
        side["data_file_bytes"] = None
    save_sidecar(side_path, side)
    print(json.dumps({"pid": pid, "how": how, "took_s": side["stop"]["took_s"],
                      "clean_banner": side["stop"]["clean_banner"]}))
    return 0 if side["stop"]["clean_banner"] and how == "SIGTERM" else 2


def cmd_status(a):
    workdir = os.path.abspath(a.workdir)
    side, _ = load_sidecar(workdir)
    pid = side["pid"]
    side["alive_now"] = pid_alive(pid) and pid_is_ours(pid, side["binary_copy"])
    side["port_open_now"] = port_open(side["port"])
    print(json.dumps(side, indent=2))
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("start", help="start one kds_server from a copy of --binary")
    s.add_argument("--binary", required=True)
    s.add_argument("--workdir", required=True, help="fresh directory on a block device")
    s.add_argument("--port", type=int, required=True, help="KWP/1 port, chosen (never 15432)")
    s.add_argument("--debug-text-port", type=int, default=0,
                   help="newline debug surface; 0 keeps it closed (kds.conf.sample:314)")
    s.add_argument("--kwp-port", type=int, default=0,
                   help="the v0 load endpoint; 0 keeps it closed (kds.conf.sample:321)")
    s.add_argument("--cores", type=int, default=4)
    s.add_argument("--taskset", default=None, help="CPU list for taskset -c, e.g. 2-5")
    s.add_argument("--reactor-cpus", default=None,
                   help="kds.conf reactor_cpus; derived from --taskset when omitted")
    s.add_argument("--durability", default="relaxed", choices=["strict", "group", "relaxed"])
    s.add_argument("--buffer-pool-frames", type=int, default=65536)
    s.add_argument("--ash", default="on", choices=["on", "off"])
    s.add_argument("--log-level", default="info")
    s.add_argument("--conf-extra", action="append", default=[],
                   help="an extra `key = value` line for kds.conf (repeatable)")
    s.add_argument("--label", default=None)
    s.add_argument("--allow-tmpfs", action="store_true")
    s.add_argument("--timeout", type=float, default=60.0, help="seconds to wait for readiness")
    s.set_defaults(run=cmd_start)

    t = sub.add_parser("stop", help="SIGTERM the server named by --workdir/server.json")
    t.add_argument("--workdir", required=True)
    t.add_argument("--timeout", type=float, default=60.0,
                   help="seconds before SIGKILL (recorded as forced)")
    t.set_defaults(run=cmd_stop)

    u = sub.add_parser("status", help="print the sidecar with liveness")
    u.add_argument("--workdir", required=True)
    u.set_defaults(run=cmd_status)

    a = p.parse_args()
    return a.run(a)


if __name__ == "__main__":
    sys.exit(main())
