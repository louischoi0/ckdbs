#!/usr/bin/env python3
"""AR2 section 9 cells C1 and C2 - the orchestrator.

One cell = a fresh data file, a fresh server started from the hashed
binary copy in this directory, a precheck (loadavg, pgrep for builds and
other servers, df -T of the data device), the driver's phases, a
SHOW META dump from every core, SIGTERM, wait. Nothing is shared between
cells but the binary copy and this host.

    run_cells.py smoke            start one cores=1 server, dump META, stop
    run_cells.py all              run every cell in CELLS, in order
    run_cells.py <cell> [<cell>]  run the named cells, in order

Everything a cell produced lands beside it as <cell>.*; the run-wide
facts land in run.json. Drivers are the unmodified tools/ drivers of the
worktree named in TOOLS.
"""
import json
import os
import signal
import socket
import subprocess
import sys
import time
from datetime import datetime, timezone

RUN = "/home/cdkbs/bench-runs/ar2-c1c2-92cb654"
BIN = os.path.join(RUN, "kds_server")
TREE = "/home/cdkbs/ckdbs/.claude/worktrees/ar2-borrow-model"
TOOLS = os.path.join(TREE, "tools")
PY = sys.executable

# AL-S8's fixed driver arguments, verbatim from
# bench/v3.0.0/results-scenario2-freight-v2.7.0-157-gf6ed10c.md section 2 and
# results-scenario0-stockmarket-v2.7.0-157-gf6ed10c.md section 2.
S2_ARGS = ("--organizations 300 --ships 30 --operations 300 --cargos 4000 "
           "--bookers 8 --bookings 3000 --verify 100 --seed 1 --sync").split()
S0_ARGS = ("--users 100 --accounts-per-user 3 --assets 30 --traders 8 "
           "--txn-per-user 50 --verify 200 --seed 1 --sync").split()

# name, scenario, port, cores, peer_listeners, range_size_ids (None = off)
# Interleaved A/B: a scenario-2 cell alternates with a scenario-0 cell, the
# headline pairs first, the references next, the repeats last.
CELLS = [
    ("s2-c8-plon",        "s2", 15590, 8, "on",  None),   # AL-S8's s2-c8-g re-run
    ("s0-c8-sp0",         "s0", 15591, 8, "on",  None),   # AL-S8's s0-c8-g re-run
    ("s2-c8-ploff",       "s2", 15592, 8, "off", None),   # C1: every session on core 0
    ("s0-c8-sp65536",     "s0", 15593, 8, "on",  65536),  # C2: ratified range size
    ("s2-c1",             "s2", 15594, 1, "off", None),   # reference
    ("s0-c8-sp4096",      "s0", 15595, 8, "on",  4096),   # C2: K-f's group optimum
    ("s0-c1",             "s0", 15596, 1, "off", None),   # reference
    ("s2-c8-plon-r2",     "s2", 15597, 8, "on",  None),   # repeats: the run-to-run floor
    ("s2-c8-ploff-r2",    "s2", 15598, 8, "off", None),
    ("s0-c8-sp0-r2",      "s0", 15599, 8, "on",  None),
    ("s0-c8-sp65536-r2",  "s0", 15600, 8, "on",  65536),
    ("s2-c1-r2",          "s2", 15601, 1, "off", None),
    ("s0-c1-r2",          "s0", 15602, 1, "off", None),
]


def now_utc():
    return datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")


def sh(args, timeout=60):
    p = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout, p.stderr


def precheck():
    load = open("/proc/loadavg").read().strip()
    code, out, _ = sh(["pgrep", "-a", "-f", "cc1plus|cmake --build|ctest|kds_server"])
    _, df, _ = sh(["df", "-T", RUN])
    return {"time": now_utc(), "loadavg": load, "pgrep": out.strip(), "df": df.strip()}


def config_text(cell, port, cores, peer_listeners, range_size_ids):
    lines = [
        f"data_file = {RUN}/{cell}.db",
        f"port = {port}",
        f"cores = {cores}",
        "durability = group",
        f"peer_listeners = {peer_listeners}",
        "placement = namespace",
        "checkpoint_interval_ms = 5000",
        "auth = off",
        "tls = off",
        f"log_dir = {RUN}/logs/{cell}",
        "log_level = warn",
    ]
    if range_size_ids is not None:
        lines.append(f"range_size_ids = {range_size_ids}")
    return "\n".join(lines) + "\n"


def wait_port(port, seconds=90):
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                return True
        except OSError:
            time.sleep(0.2)
    return False


def start_server(cell, conf_path):
    log_path = os.path.join(RUN, f"{cell}.server.txt")
    log = open(log_path, "w")
    proc = subprocess.Popen([BIN, "--config", conf_path], stdout=log, stderr=subprocess.STDOUT)
    return proc, log


def stop_server(proc, log):
    t0 = time.time()
    proc.send_signal(signal.SIGTERM)
    try:
        code = proc.wait(timeout=120)
        how = "sigterm"
    except subprocess.TimeoutExpired:
        proc.kill()
        code = proc.wait(timeout=30)
        how = "sigkill-after-120s"
    log.close()
    return {"stop": how, "exit_code": code, "stop_seconds": round(time.time() - t0, 3)}


def dump_meta(port, cores, max_attempts=500):
    sys.path.insert(0, TOOLS)
    from ckdbs_cli import ServerConnection  # noqa: E402
    got, attempts, held = {}, 0, []
    while len(got) < cores and attempts < max_attempts:
        conn = ServerConnection("127.0.0.1", port, timeout=30)
        attempts += 1
        reply = conn.send_command("SHOW META")
        core = None
        for tok in reply.split():
            if tok.startswith("core="):
                core = int(tok[5:])
                break
        if core is not None and core not in got:
            got[core] = reply
            held.append(conn)  # hold it so the next accept can land elsewhere
        else:
            try:
                conn.close()
            except Exception:
                pass
    for c in held:
        try:
            c.close()
        except Exception:
            pass
    return {"attempts": attempts, "cores_reached": sorted(got), "replies": {str(k): v for k, v in got.items()}}


def driver(cell, scenario, port, suffix, phase_flag, json_path=None, timeout=3600):
    script = "scenario2_freight.py" if scenario == "s2" else "scenario0_stockmarket.py"
    fixed = S2_ARGS if scenario == "s2" else S0_ARGS
    args = [PY, os.path.join(TOOLS, script), "--host", "127.0.0.1", "--port", str(port),
            "--suffix", suffix] + fixed
    if phase_flag:
        args.append(phase_flag)
    if json_path:
        args += ["--json", json_path]
    tag = phase_flag.strip("-") if phase_flag else "run"
    out_path = os.path.join(RUN, f"{cell}.{tag}.stdout.txt")
    t0 = time.time()
    with open(out_path, "w") as out:
        p = subprocess.run(args, stdout=out, stderr=subprocess.STDOUT, timeout=timeout)
    return {"phase": tag, "argv": args, "exit_code": p.returncode,
            "seconds": round(time.time() - t0, 3), "stdout": out_path}


def run_cell(spec):
    cell, scenario, port, cores, pl, rs = spec
    suffix = "".join(ch for ch in cell if ch.isalnum())
    record = {"cell": cell, "scenario": scenario, "port": port, "cores": cores,
              "peer_listeners": pl, "range_size_ids": rs, "suffix": suffix,
              "binary": BIN, "precheck": precheck()}
    conf_path = os.path.join(RUN, f"{cell}.conf")
    os.makedirs(os.path.join(RUN, "logs", cell), exist_ok=True)
    for stale in (f"{cell}.db",):
        p = os.path.join(RUN, stale)
        if os.path.exists(p):
            os.remove(p)
    with open(conf_path, "w") as f:
        f.write(config_text(cell, port, cores, pl, rs))
    record["config"] = open(conf_path).read()
    record["server_start"] = now_utc()
    proc, log = start_server(cell, conf_path)
    if not wait_port(port):
        record["error"] = "server never opened its port"
        record["server_stop"] = stop_server(proc, log)
        return record
    phases = []
    try:
        if scenario == "s2":
            phases.append(driver(cell, scenario, port, suffix, "--schema-only"))
            if phases[-1]["exit_code"] == 0:
                phases.append(driver(cell, scenario, port, suffix, "--load-only"))
            if phases[-1]["exit_code"] == 0:
                phases.append(driver(cell, scenario, port, suffix, None,
                                     json_path=os.path.join(RUN, f"{cell}.json")))
        else:
            phases.append(driver(cell, scenario, port, suffix, None,
                                 json_path=os.path.join(RUN, f"{cell}.json")))
        record["phases"] = phases
        try:
            meta = dump_meta(port, cores)
        except Exception as e:  # a probe failure must not lose the cell
            meta = {"error": repr(e)}
        with open(os.path.join(RUN, f"{cell}.meta.json"), "w") as f:
            json.dump(meta, f, indent=2)
        record["meta_cores_reached"] = meta.get("cores_reached")
        record["meta_attempts"] = meta.get("attempts")
    finally:
        record["server_stop"] = stop_server(proc, log)
        record["postcheck_loadavg"] = open("/proc/loadavg").read().strip()
    with open(os.path.join(RUN, f"{cell}.cell.json"), "w") as f:
        json.dump(record, f, indent=2)
    return record


def smoke():
    spec = ("smoke-c1", "s0", 15589, 1, "off", None)
    cell, _, port, cores, pl, rs = spec
    conf_path = os.path.join(RUN, f"{cell}.conf")
    os.makedirs(os.path.join(RUN, "logs", cell), exist_ok=True)
    with open(conf_path, "w") as f:
        f.write(config_text(cell, port, cores, pl, rs))
    proc, log = start_server(cell, conf_path)
    ok = wait_port(port, 60)
    meta = dump_meta(port, cores) if ok else {"error": "port never opened"}
    stop = stop_server(proc, log)
    print(json.dumps({"port_open": ok, "meta": meta, "stop": stop}, indent=2))
    print(open(os.path.join(RUN, f"{cell}.server.txt")).read()[-2000:])


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    if sys.argv[1] == "smoke":
        smoke()
        return 0
    names = [c[0] for c in CELLS] if sys.argv[1] == "all" else sys.argv[1:]
    _, describe, _ = sh(["git", "-C", TREE, "describe", "--tags"])
    _, sha, _ = sh(["sha256sum", BIN])
    run = {"describe": describe.strip(), "binary_sha256": sha.split()[0],
           "started": now_utc(), "cells": []}
    for spec in CELLS:
        if spec[0] not in names:
            continue
        print(f"[{now_utc()}] cell {spec[0]} starting", flush=True)
        rec = run_cell(spec)
        run["cells"].append(rec)
        codes = [p["exit_code"] for p in rec.get("phases", [])]
        print(f"[{now_utc()}] cell {spec[0]} done, driver exit codes {codes}, "
              f"stop {rec['server_stop']}", flush=True)
        with open(os.path.join(RUN, "run.json"), "w") as f:
            json.dump(run, f, indent=2)
        time.sleep(3)  # let the device settle between cells
    run["finished"] = now_utc()
    with open(os.path.join(RUN, "run.json"), "w") as f:
        json.dump(run, f, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
