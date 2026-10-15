#!/usr/bin/env python3
"""BV close: interleaved A/B of a single-row INSERT and a single-row UPDATE
(literal SET of a varchar column, by tuple key) on a BTREE relation.

Arms (all servers side by side, pinned to one CPU, only one busy at a time;
every block of every cell is taken once per arm, the order rotating):
  A1   binary A (b0bc959c), table without a Transition
  A2   binary A again (second server) - the noise floor
  B    binary B (3eb5a1eb), table without a Transition
  BT   binary B, the same schema with a Transition on `st`

Uses helpers of tools/bn_census.py unmodified.
"""
import argparse, json, os, sys, time

TOOLS = "/home/cdkbs/ckdbs/.claude/worktrees/cn4-transition-refresh/tools"
sys.path.insert(0, TOOLS)
import kwp  # noqa: E402
import bn_census as bn  # noqa: E402
from bb_overhead_benchmark import host_state  # noqa: E402

SIZES = [200, 1000, 10000]
PATHS = ["literal", "prepared"]


def tname(prefix, kind, n, path=""):
    return f"{prefix}{kind}{n}" + (f"_{path}" if path else "")


def setup(srv, with_g):
    c = srv.conn
    prefixes = ["p", "g"] if with_g else ["p"]
    for pre in prefixes:
        for n in SIZES:
            tabs = [tname(pre, "u", n)] + [tname(pre, "i", n, p) for p in PATHS]
            for t in tabs:
                bn.must(c, f"CREATE TABLE {t} (id int64, st varchar(16), v int64) BTREE", "CREATE")
                for lo in range(1, n + 1, 500):
                    hi = min(n, lo + 499)
                    bn.must(c, f"INSERT INTO {t} VALUES " +
                            ",".join(f"({i}, 'A', {i})" for i in range(lo, hi + 1)), "INSERT")
                if pre == "g":
                    bn.must(c, f"CREATE TRANSITION tr_{t} ON {t}(st) ON INSERT ('A') "
                               f"ON UPDATE ('A' => 'B', 'B' => 'A')", "CREATE")


def block(srv, prefix, shape, n, path, qs, acc, ops_n):
    c = srv.conn
    i64 = kwp.Connection.int64
    tab = tname(prefix, "u" if shape == "update" else "i", n, "" if shape == "update" else path)
    stmt = None
    if path == "prepared":
        stmt = f"s_{tab}"
        if stmt not in srv.prepared:
            c.prepare(stmt, f"UPDATE {tab} SET st = ? WHERE id = ?" if shape == "update"
                      else f"INSERT INTO {tab} VALUES (?, ?, ?)")
            srv.prepared.add(stmt)
    s0, c0, w0 = bn.sched_ns(srv.proc.pid), bn.self_ns(), time.perf_counter()
    for q in qs:
        if shape == "update":
            key = 1 + (q % n)
            st = "B" if (q // n) % 2 == 0 else "A"   # every UPDATE is a declared edge
            if stmt:
                p = [(kwp.T_VARCHAR, 0, st.encode()), i64(key)]
            else:
                sql = f"UPDATE {tab} SET st = '{st}' WHERE id = {key}"
            want = "UPDATED 1"
        else:
            key = 10_000_000 + q
            if stmt:
                p = [i64(key), (kwp.T_VARCHAR, 0, b"A"), i64(q)]
            else:
                sql = f"INSERT INTO {tab} VALUES ({key}, 'A', {q})"
            want = "INSERT"
        t0 = time.perf_counter()
        if stmt:
            _, _, got, _ = c.execute_prepared(stmt, p)
        else:
            _, _, got, _ = c.execute(sql)
        acc["lat"].append(time.perf_counter() - t0)
        if not got.startswith(want):
            acc["bad"] += 1
    acc["wall"] += time.perf_counter() - w0
    acc["server_ns"] += bn.sched_ns(srv.proc.pid) - s0
    acc["driver_ns"] += bn.self_ns() - c0


def one_run(a, r):
    arms = [("A1", "A", "p"), ("A2", "A", "p"), ("B", "B", "p"), ("BT", "B", "g")]
    srvs = {}
    out = {"run": r, "host_before": host_state(), "cells": [], "checks": {}}
    try:
        for i, (label, which, pre) in enumerate([("A1", "A", "p"), ("A2", "A", "p"), ("B", "B", "p")]):
            srvs[label] = bn.Server(a, label, a.bins[which], a.port + i, f"r{r}")
        for label, srv in srvs.items():
            setup(srv, with_g=(label == "B"))
        for srv in srvs.values():
            bn.wait_idle(srv)
        order = [x[0] for x in arms]
        q = 1
        for n in SIZES:
            for shape in ("update", "insert"):
                paths = PATHS if r % 2 == 0 else PATHS[::-1]
                for path in paths:
                    per = a.ops // a.blocks
                    accs = {l: {"lat": [], "bad": 0, "wall": 0.0, "server_ns": 0, "driver_ns": 0}
                            for l in order}
                    host = host_state()
                    for b in range(a.blocks):
                        qs = range(q + b * per, q + (b + 1) * per)
                        k = (b + r) % 4
                        for lab in order[k:] + order[:k]:
                            _, which, pre = next(x for x in arms if x[0] == lab)
                            srv = srvs["B" if lab == "BT" else lab]
                            block(srv, pre, shape, n, path, qs, accs[lab], a.ops)
                    host["after"] = host_state()["loadavg"]
                    q += a.blocks * per
                    for lab in order:
                        res = bn.summarize(f"{shape}", path, lab, accs[lab])
                        res.update({"rows": n, "host": host, "run": r,
                                    "p95_us": bn.pct(accs[lab]["lat"], 95) * 1e6})
                        res.pop("block_server_us", None)
                        out["cells"].append(res)
                    print(r, n, shape, path, {l: round(accs[l]["server_ns"] / len(accs[l]["lat"]) / 1e3, 1)
                                              for l in order}, flush=True)
        # end-state and liveness of the check
        for label, srv in srvs.items():
            pres = ["p", "g"] if label == "B" else ["p"]
            for pre in pres:
                _, rows, _, _ = srv.conn.execute(f"SELECT st FROM {tname(pre,'u',200)} WHERE id = 1")
                out["checks"][f"{label}/{pre}-u200-id1-st"] = rows[0][0].decode() if rows else None
                _, rows, tag, _ = srv.conn.execute(f"SELECT COUNT(*) FROM {tname(pre,'i',1000,'literal')}")
                out["checks"][f"{label}/{pre}-i1000_literal-count"] = tag + " " + repr(rows)
        try:
            _, _, got, _ = srvs["B"].conn.execute("UPDATE gu200 SET st = 'C' WHERE id = 1")
        except kwp.KwpError as e:
            got = "REFUSED: " + str(e)
        out["checks"]["B/g violating UPDATE"] = got
        _, _, got, _ = srvs["B"].conn.execute("UPDATE pu200 SET st = 'C' WHERE id = 1")
        out["checks"]["B/p same UPDATE (no Transition)"] = got
    finally:
        for s in srvs.values():
            s.stop()
    out["host_after"] = host_state()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin-a", required=True)
    ap.add_argument("--bin-b", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--port", type=int, default=31941)
    ap.add_argument("--pin-server", default="2")
    ap.add_argument("--pin-driver", default="4")
    ap.add_argument("--ops", type=int, default=6000)
    ap.add_argument("--blocks", type=int, default=12)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    a.bins = {"A": a.bin_a, "B": a.bin_b}
    a.cores, a.durability = 1, "relaxed"
    os.sched_setaffinity(0, {int(a.pin_driver)})
    res = {"args": {k: v for k, v in vars(a).items()}, "runs": []}
    for r in range(a.runs):
        with open(os.path.join(os.path.dirname(a.out), "progress"), "w") as f:
            f.write(f"run {r} started {time.time()}\n")
        res["runs"].append(one_run(a, r))
        with open(a.out, "w") as f:
            json.dump(res, f, indent=1)
    with open(os.path.join(os.path.dirname(a.out), "progress"), "w") as f:
        f.write("DONE\n")


if __name__ == "__main__":
    main()
