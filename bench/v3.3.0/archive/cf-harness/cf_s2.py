#!/usr/bin/env python3
"""CF-S2: the close of CF's Branch O (instructions/v3.2.0/workorder-cf-waystone-premise.md §8.5).

CF-S0's harness (cf_s0.py) with arms that name a binary and the cells to opt in:

    base       the base binary (722006ec): Waystone on for every shape, as before O
    close      the closing binary, nothing opted
    close-opt  the closing binary, `scanprobe` opted in by `WAYSTONE ON <example>`

run in rotating order. Expected (§8.3): `pk`, `chain3`, `chain4` on `close`
match CF-S0's arm C (both keys off) against `base`; `scanprobe` on
`close-opt` keeps CF-S0's net against `close` (+5.0 % throughput, -6.4 %
server CPU). `replays=` is read per cell as in CF-S0, so a not-opted cell on
`close` must show 0 and `scanprobe` on `close-opt` 200.

Writes `OUT/<arm>-r<rep>.json`, skipping any that exist; `OUT/progress` per cell.
Fold with compare.py, passing the arm names.
"""
import argparse, json, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cf_s0  # noqa: E402

ROTATION = [("base", "close", "close-opt"), ("close", "close-opt", "base"),
            ("close-opt", "base", "close")]


def one_run(a, arm, rep):
    path = os.path.join(a.out, f"{arm}-r{rep}.json")
    if os.path.exists(path):
        return
    waited = 0
    pre = cf_s0.host_state()
    while pre["competing"] and waited < a.quiet_wait:
        cf_s0.touch(os.path.join(a.out, "progress"))
        time.sleep(10)
        waited += 10
        pre = cf_s0.host_state()
    pre["quiet_waited_s"] = waited
    a.binary = a.base if arm == "base" else a.close
    srv = cf_s0.Server(a, "A", f"{rep}-{arm}")  # arm A's keys: recording and replay on
    try:
        cf_s0.setup(a, srv.conn)
        cells = cf_s0.cells(a)
        if arm == "close-opt":
            for name, keys, sql_of in cells:
                if name in a.opt:
                    _, _, tag, _ = srv.conn.execute("WAYSTONE ON " + sql_of(keys[0]))
                    if not tag.startswith("OK WAYSTONE ON"):
                        raise SystemExit(f"{name}: WAYSTONE ON -> {tag!r}")
        results = [cf_s0.run_cell(a, srv, n, k, f) for n, k, f in cells
                   if not a.cells or n in a.cells]
    finally:
        srv.stop()
    with open(path + ".tmp", "w") as f:
        json.dump({"arm": arm, "rep": rep, "precheck": pre, "cells": results,
                   "finished": time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime())}, f, indent=1)
    os.replace(path + ".tmp", path)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--base", required=True)
    p.add_argument("--close", required=True)
    p.add_argument("--opt", default="scanprobe", help="comma-separated cells opted in on close-opt")
    p.add_argument("--out", required=True)
    p.add_argument("--data", required=True)
    p.add_argument("--port", type=int, default=15790)
    p.add_argument("--reps", type=int, default=6)
    p.add_argument("--rows", type=int, default=20000)
    p.add_argument("--small", type=int, default=2000)
    p.add_argument("--keys", type=int, default=1000)
    p.add_argument("--passes", type=int, default=22)
    p.add_argument("--sessions", type=int, default=100)
    p.add_argument("--per-session", type=int, default=100)
    p.add_argument("--scan-keys", type=int, default=50)
    p.add_argument("--pin-server", default="2", help="taskset CPU list for the server")
    p.add_argument("--pin-driver", type=int, default=3)
    p.add_argument("--quiet-wait", type=int, default=900)
    p.add_argument("--cores", type=int, default=1)
    p.add_argument("--cells", default="", help="comma-separated cells to run (default: all)")
    p.add_argument("--arms", default="base,close,close-opt", help="comma-separated arms to run")
    a = p.parse_args()
    a.cells = set(filter(None, a.cells.split(",")))
    a.arms = set(a.arms.split(","))
    a.opt = set(a.opt.split(","))
    os.makedirs(a.out, exist_ok=True)
    os.makedirs(a.data, exist_ok=True)
    os.sched_setaffinity(0, {a.pin_driver})
    for r in range(1, a.reps + 1):
        for arm in ROTATION[(r - 1) % 3]:
            if arm in a.arms:
                one_run(a, arm, r)
    cf_s0.touch(os.path.join(a.out, "done"))


if __name__ == "__main__":
    main()
