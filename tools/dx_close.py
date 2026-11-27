#!/usr/bin/env python3
"""DX's milestone-close overhead measurement: a pool that is not full pays nothing (DX-R5).

`instructions/v3.2.0/workorder-dx-pool-under-pressure.md` section 3, DX-R5.

Built on `tools/dw_close.py` (imported unmodified: its Server, kwp helpers, CPU
read and quiet gate). Three arms, each a COPY of a binary, each on a fresh
server and data file per round, `durability = relaxed`, `cores = 1`, a roomy
pool (`--frames`, default 262144) so nothing is evicted or refused:

    A    the commit DX opened from (481d101aa)
    A2   a second copy of A: the control
    B    DX's closing code (5e25f1d26)

Per round the three arms run once in a rotated order (the six orders of three).
On each arm, per relation (BTREE and HEAP, 200 / 1,000 / 10,000 rows,
`(id int64 PRIMARY KEY, k int64, w int64)` with `CREATE INDEX ix ON t (k)`),
`--stmts` prepared statements a shape, in this order:

    sel_pk  SELECT * FROM t WHERE id = ?        the hit path
    ins     INSERT INTO t VALUES (?, ?, 0)      new ids above the rows (autocommit)
    upd_nk  UPDATE t SET w = ? WHERE id = ?     by tuple key, autocommit
    rb      BEGIN; UPDATE t SET w = ? WHERE id = ?; ROLLBACK   (one unit = three frames)

Server CPU per unit is the median over the phase's batches (`--batch` units)
of the batch's schedstat delta; client latency (p0..p99.9) beside it. After the
phases each arm reads SHOW META's pool counters (refused, inline/background
reclaim, resident) so that "nothing was evicted or refused" is read, not assumed.

    tools/dx_close.py --run-dir DIR --bin-a A --bin-a2 A2 --bin-b B --rounds 12
"""
import argparse, json, os, sys, time
from itertools import permutations
from statistics import median

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dw_close as dw  # noqa: E402
import kwp  # noqa: E402
from dq_s0_premise import settled_cpu_ns, quiet_gate  # noqa: E402

dw.SERVER_CPUS[1] = "0-7"       # the P-cores: the host is a hybrid CPU
i64 = dw.i64
SIZES = dw.SIZES
KINDS = dw.KINDS
POOL = ("pool_refused", "pool_reclaimed_inline", "pool_reclaimed_background", "pool_batches_inline",
        "pool_batches_background", "pool_resident", "pool_misses", "pool_hits", "pool_dirty_queued",
        "pool_dirty_drained")


def phase(c, pid, name, plist, want_aff, batch, rollback=False):
    cpus, lat = [], []
    t_all = time.perf_counter()
    for at in range(0, len(plist), batch):
        part = plist[at:at + batch]
        c0 = settled_cpu_ns(pid)
        for params in part:
            a = time.perf_counter()
            try:
                if rollback:
                    c.begin()
                _f, rows, _t, aff = c.execute(None, params, statement=name)
                if rollback:
                    c.rollback()
            except kwp.KwpError as e:
                raise SystemExit(f"refused {name}: {e.code} {e}")
            lat.append(time.perf_counter() - a)
            if want_aff is not None and aff != want_aff:
                raise SystemExit(f"{name}: affected {aff}, want {want_aff}")
            if want_aff is None and len(rows) != 1:
                raise SystemExit(f"{name}: {len(rows)} rows")
        cpus.append((settled_cpu_ns(pid) - c0) / len(part) / 1e3)
    return {"cpu_us_med": round(median(cpus), 3), "cpu_us_min": round(min(cpus), 3),
            "cpu_us_max": round(max(cpus), 3), "lat": dw.pcts(lat),
            "qps_client": round(len(plist) / sum(lat), 1), "wall_s": round(time.perf_counter() - t_all, 3)}


def poolstat(c):
    m = dw.meta(c)
    return {k: (float(m[k]) if m.get(k) not in (None, "") else None) for k in POOL}


def arm(args, label, binpath, rnd, port):
    wd = os.path.join(args.run_dir, "p1", f"r{rnd}-{label}")
    srv = dw.Server(binpath, wd, port, 1, False, args.frames)
    c = srv.connect()
    out = {"arm": label, "round": rnd, "bin_sha": dw.sha(binpath), "cells": {}}
    for kind in KINDS:
        for size in SIZES:
            t = f"{kind}{size}"
            dw.must(c, f"CREATE TABLE {t} (id int64 PRIMARY KEY, k int64, w int64) {'BTREE' if kind == 'bt' else 'HEAP'}")
            dw.must(c, f"CREATE INDEX ix_{t} ON {t} (k)")
            for at in range(1, size + 1, 500):
                vals = ", ".join(f"({i},{i * 7},0)" for i in range(at, min(at + 500, size + 1)))
                dw.must(c, f"INSERT INTO {t} VALUES {vals}")
            c.prepare(f"{t}_sel_pk", f"SELECT * FROM {t} WHERE id = ?")
            c.prepare(f"{t}_ins", f"INSERT INTO {t} VALUES (?, ?, 0)")
            c.prepare(f"{t}_upd_nk", f"UPDATE {t} SET w = ? WHERE id = ?")
    time.sleep(2)
    out["pool_before"] = poolstat(c)
    S = args.stmts
    for kind in KINDS:
        for size in SIZES:
            t = f"{kind}{size}"
            ids = [(q % size) + 1 for q in range(S)]
            newids = [size + 1 + q for q in range(S)]
            cell = {}
            cell["sel_pk"] = phase(c, srv.pid, f"{t}_sel_pk", [[i64(i)] for i in ids], None, args.batch)
            cell["ins"] = phase(c, srv.pid, f"{t}_ins", [[i64(i), i64(i * 7)] for i in newids], 1, args.batch)
            cell["upd_nk"] = phase(c, srv.pid, f"{t}_upd_nk", [[i64(q), i64(i)] for q, i in enumerate(ids)], 1, args.batch)
            cell["rb"] = phase(c, srv.pid, f"{t}_upd_nk", [[i64(q), i64(i)] for q, i in enumerate(ids)], 1, args.batch, True)
            out["cells"][t] = cell
        dw.beat(args.run_dir, phase="dx", arm=label, round=rnd, kind=kind)
    out["pool_after"] = poolstat(c)
    srv.stop(c)
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--run-dir", required=True)
    p.add_argument("--bin-a", required=True)
    p.add_argument("--bin-a2", required=True)
    p.add_argument("--bin-b", required=True)
    p.add_argument("--base-port", type=int, default=15611)
    p.add_argument("--frames", type=int, default=262144)
    p.add_argument("--stmts", type=int, default=2000)
    p.add_argument("--batch", type=int, default=100)
    p.add_argument("--rounds", type=int, default=12)
    args = p.parse_args()
    os.makedirs(args.run_dir, exist_ok=True)
    arms = {"A": args.bin_a, "A2": args.bin_a2, "B": args.bin_b}
    shas = {k: dw.sha(v) for k, v in arms.items()}
    held = quiet_gate(lambda *a, **k: None)
    json.dump({"started": time.time(), "shas": shas, "gate_hold_s": held, "host": dw.host_state(),
               "frames": args.frames, "stmts": args.stmts, "batch": args.batch},
              open(os.path.join(args.run_dir, "meta.json"), "w"))
    perms = list(permutations(("A", "A2", "B")))
    results = []
    for rnd in range(args.rounds):
        order = perms[rnd % len(perms)]
        for pos, label in enumerate(order):
            quiet_gate(lambda *a, **k: None)
            host = dw.host_state()
            r = arm(args, label, arms[label], rnd, args.base_port + (rnd * 3 + pos) % 30)
            r["order_in_round"], r["host"] = pos, host
            results.append(r)
            json.dump(results, open(os.path.join(args.run_dir, "dx.json"), "w"))
            dw.beat(args.run_dir, phase="arm-done", arm=label, round=rnd)
    print("done")


if __name__ == "__main__":
    main()
