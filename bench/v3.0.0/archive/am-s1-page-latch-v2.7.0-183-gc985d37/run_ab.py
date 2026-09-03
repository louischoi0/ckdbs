#!/usr/bin/env python3
"""AM-S1's cores = 1 A/B - the orchestrator.

Two hashed release binaries, A (the parent engine) and B (the landing
commit), scenario 0 at cores = 1 under group and strict durability,
interleaved so neither arm ever runs twice in a row and each pair is
repeated with the order reversed. One cell = a fresh data file, a fresh
server from the named binary, a precheck (loadavg, pgrep, df), the driver,
SHOW META, SIGTERM, wait.

    run_ab.py <run dir> <binary A> <binary B> [all | <cell> ...]
"""
import json
import os
import signal
import socket
import subprocess
import sys
import time
from datetime import datetime, timezone

RUN = sys.argv[1]
BIN = {"A": sys.argv[2], "B": sys.argv[3]}
TREE = "/home/cdkbs/ckdbs/.claude/worktrees/ar2-borrow-model"
TOOLS = os.path.join(TREE, "tools")
PY = sys.executable

# AL-S8's scenario-0 arguments, verbatim (results-scenario0-stockmarket-…md section 2).
S0_ARGS = ("--users 100 --accounts-per-user 3 --assets 30 --traders 8 "
           "--txn-per-user 50 --verify 200 --seed 1 --sync").split()

# name, arm, port, durability. Interleaved A/B, each pair twice, order reversed.
CELLS = [
    ("g1-A", "A", 15610, "group"),
    ("g1-B", "B", 15611, "group"),
    ("s1-A", "A", 15612, "strict"),
    ("s1-B", "B", 15613, "strict"),
    ("g2-B", "B", 15614, "group"),
    ("g2-A", "A", 15615, "group"),
    ("s2-B", "B", 15616, "strict"),
    ("s2-A", "A", 15617, "strict"),
    # Pass 2, added after pass 1 finished: g1-B's mount-time checkpoint took 434 ms
    # against 5-7 ms in every other cell, so the group durability had one clean B
    # cell; a third group pair restores two per arm.
    ("g3-A", "A", 15618, "group"),
    ("g3-B", "B", 15619, "group"),
    # Pass 3: both g3 cells were degraded the same way (mount checkpoints of 168 ms
    # and 61 ms); a fourth pair, B first, after a calm fdatasync probe.
    ("g4-B", "B", 15620, "group"),
    ("g4-A", "A", 15621, "group"),
]


def now_utc():
    return datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")


def sh(args, timeout=60):
    p = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout, p.stderr


def precheck():
    load = open("/proc/loadavg").read().strip()
    _, out, _ = sh(["pgrep", "-a", "-f", "cc1plus|cmake --build|ctest|kds_server"])
    _, df, _ = sh(["df", "-T", RUN])
    return {"time": now_utc(), "loadavg": load, "pgrep": out.strip(), "df": df.strip()}


def config_text(cell, port, durability):
    return "\n".join([
        f"data_file = {RUN}/{cell}.db",
        f"port = {port}",
        "cores = 1",
        f"durability = {durability}",
        "peer_listeners = off",
        "placement = namespace",
        "checkpoint_interval_ms = 5000",
        "auth = off",
        "tls = off",
        f"log_dir = {RUN}/logs/{cell}",
        "log_level = warn",
    ]) + "\n"


def wait_port(port, seconds=90):
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                return True
        except OSError:
            time.sleep(0.2)
    return False


def dump_meta(port):
    sys.path.insert(0, TOOLS)
    from ckdbs_cli import ServerConnection  # noqa: E402
    conn = ServerConnection("127.0.0.1", port, timeout=30)
    reply = conn.send_command("SHOW META")
    try:
        conn.close()
    except Exception:
        pass
    return reply


def run_cell(spec):
    cell, arm, port, durability = spec
    suffix = "".join(ch for ch in cell if ch.isalnum()).lower()
    record = {"cell": cell, "arm": arm, "binary": BIN[arm], "port": port,
              "durability": durability, "suffix": suffix, "precheck": precheck()}
    conf_path = os.path.join(RUN, f"{cell}.conf")
    os.makedirs(os.path.join(RUN, "logs", cell), exist_ok=True)
    db = os.path.join(RUN, f"{cell}.db")
    if os.path.exists(db):
        os.remove(db)
    with open(conf_path, "w") as f:
        f.write(config_text(cell, port, durability))
    record["config"] = open(conf_path).read()
    log = open(os.path.join(RUN, f"{cell}.server.txt"), "w")
    record["server_start"] = now_utc()
    proc = subprocess.Popen([BIN[arm], "--config", conf_path], stdout=log, stderr=subprocess.STDOUT)
    if not wait_port(port):
        record["error"] = "server never opened its port"
    else:
        args = [PY, os.path.join(TOOLS, "scenario0_stockmarket.py"), "--host", "127.0.0.1",
                "--port", str(port), "--suffix", suffix] + S0_ARGS + \
               ["--json", os.path.join(RUN, f"{cell}.json")]
        t0 = time.time()
        with open(os.path.join(RUN, f"{cell}.run.stdout.txt"), "w") as out:
            p = subprocess.run(args, stdout=out, stderr=subprocess.STDOUT, timeout=3600)
        record["driver"] = {"argv": args, "exit_code": p.returncode,
                            "seconds": round(time.time() - t0, 3)}
        try:
            record["meta"] = dump_meta(port)
        except Exception as e:
            record["meta_error"] = repr(e)
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
    record["server_stop"] = {"stop": how, "exit_code": code, "stop_seconds": round(time.time() - t0, 3)}
    record["postcheck_loadavg"] = open("/proc/loadavg").read().strip()
    with open(os.path.join(RUN, f"{cell}.cell.json"), "w") as f:
        json.dump(record, f, indent=2)
    return record


def main():
    names = [c[0] for c in CELLS] if (len(sys.argv) < 5 or sys.argv[4] == "all") else sys.argv[4:]
    _, describe, _ = sh(["git", "-C", TREE, "describe", "--tags"])
    hashes = {}
    for arm, path in BIN.items():
        _, sha, _ = sh(["sha256sum", path])
        hashes[arm] = sha.split()[0]
    run = {"describe_tree": describe.strip(), "binaries": BIN, "sha256": hashes,
           "started": now_utc(), "cells": []}
    # A later pass appends to the earlier one's run.json instead of replacing it.
    prev_path = os.path.join(RUN, "run.json")
    if os.path.exists(prev_path):
        prev = json.load(open(prev_path))
        run["cells"] = prev.get("cells", [])
        run["started"] = prev.get("started", run["started"])
        run["passes"] = prev.get("passes", []) + [
            {"started": prev.get("started"), "finished": prev.get("finished"),
             "cells": [c["cell"] for c in prev.get("cells", [])]}]
        run["pass_started"] = now_utc()
    for spec in CELLS:
        if spec[0] not in names:
            continue
        print(f"[{now_utc()}] cell {spec[0]} ({spec[1]}, {spec[3]}) starting", flush=True)
        rec = run_cell(spec)
        run["cells"].append(rec)
        print(f"[{now_utc()}] cell {spec[0]} done, driver exit {rec.get('driver', {}).get('exit_code')}, "
              f"stop {rec['server_stop']}", flush=True)
        with open(os.path.join(RUN, "run.json"), "w") as f:
            json.dump(run, f, indent=2)
        time.sleep(3)
    run["finished"] = now_utc()
    with open(os.path.join(RUN, "run.json"), "w") as f:
        json.dump(run, f, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
