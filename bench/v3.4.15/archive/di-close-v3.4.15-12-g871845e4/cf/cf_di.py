#!/usr/bin/env python3
"""DI close: CF-S2's harness (cf_s2.py) with four arms over two binaries.

    A      binary A (1c7bb774), nothing opted
    A-opt  binary A, `scanprobe` opted in by `WAYSTONE ON <example>`
    B      binary B (871845e4), nothing opted
    B-opt  binary B, `scanprobe` opted in

The arm order rotates by one each rep. Cells pk, chain3, scanprobe (chain4 and control are
left out to fit the budget). `replays=` per cell is read as in CF-S0. Writes OUT/<arm>-r<rep>.json,
skipping any that exist. Changes against cf_s2.py: arms and rotation, `--bins A=..,B=..`
instead of --base/--close, the opt-in applied only to cells that are also run.
"""
import argparse, json, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cf_s0  # noqa: E402

ARMS4 = ["A", "A-opt", "B", "B-opt"]


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
    a.binary = a.bins[arm.split("-")[0]]
    srv = cf_s0.Server(a, "A", f"{rep}-{arm}")  # arm A's keys: recording and replay on
    try:
        cf_s0.setup(a, srv.conn)
        cells = cf_s0.cells(a)
        if arm.endswith("-opt"):
            for name, keys, sql_of in cells:
                if name in a.opt and (not a.cells or name in a.cells):
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
    p.add_argument("--bins", required=True, help="A=path,B=path")
    p.add_argument("--opt", default="scanprobe", help="comma-separated cells opted in on the -opt arms")
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
    p.add_argument("--cells", default="pk,chain3,scanprobe", help="comma-separated cells to run")
    p.add_argument("--arms", default="A,A-opt,B,B-opt")
    a = p.parse_args()
    a.cells = set(filter(None, a.cells.split(",")))
    a.arms = set(a.arms.split(","))
    a.opt = set(a.opt.split(","))
    a.bins = dict(x.split("=", 1) for x in a.bins.split(","))
    os.makedirs(a.out, exist_ok=True)
    os.makedirs(a.data, exist_ok=True)
    os.sched_setaffinity(0, {a.pin_driver})
    for r in range(1, a.reps + 1):
        s = (r - 1) % len(ARMS4)
        for arm in ARMS4[s:] + ARMS4[:s]:
            if arm in a.arms:
                one_run(a, arm, r)
    cf_s0.touch(os.path.join(a.out, "done"))


if __name__ == "__main__":
    main()
