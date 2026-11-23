#!/usr/bin/env python3
"""DR's close: what the milestone built, measured.

`instructions/v3.2.0/workorder-dr-primary-key-over-keystone.md` section 5,
DR-S4. `cores = 1`, `relaxed`, every page resident, one fresh server a cell.

`--part ab` - **the recommended key is today's relation** (DR-R9), and what
the milestone costs a statement it did not mean to change. Three arms a
round, in an order rotated per round: `A` the opening commit's binary, `A2`
a second copy of it (the control: what two runs of one engine differ by),
`B` the closing commit's. On each, two relations of `--rows` rows whose key
is one `int64` first column, a btree and a heap (`id int64 PRIMARY KEY`),
and on each relation:

    load      the rows in 500-row INSERTs, in key order
    read      point reads by the key
    update    `UPDATE t SET w = w + 1 WHERE id = k`
    walked    `UPDATE t SET w = w + 1 WHERE v = k` - no key: the walk, which
              now asks whether a unique index names the row
    range     100 rows, bounded inclusively
    insert    single-row INSERTs of new keys
    delete    `DELETE FROM t WHERE id = k`

`--part built` - **what was built, against DR-S0's cells**, on `--bin-b`
alone. A heap per key form of DR-S0 (`int32`, `int32x2`, `varchar64`,
`mixed4`; `ref_heap` beside them): a read, an update and a delete by the
whole key, and an update that moves the key - each a walk at DR-S0. And a
btree per form the milestone gave it, the rows arriving shuffled:

    F0        (id int64, v, w)                       named keys
    F1        (a int32, b int16, v, w, PRIMARY KEY (a, b))     embedded
    Wint      (a int32, b int32, v, w, PRIMARY KEY (a, b))     a wide key
    Wstr      (s varchar(64), v, w, PRIMARY KEY (s))           a wide key

    A wide key is whatever form the binary gives it: the indexed form, rows
    in issue order (P1), through `bff54d524`, and the hashed home (F2) from
    DR-S3f on. Run against both binaries, the two cells are that pair.

    load, a point read, a range of 100 rows by the key, an update and a
    delete by the whole key.

Server CPU a statement is **the median of the phase's batches** (20 a
phase): a checkpoint that lands in a phase moves a few batches and not the
median (DR-S0's lesson). Client latency percentiles beside it.

    dr_close.py --part ab --bin-a A --bin-b B --workdir D --out OUT.json
    dr_close.py --part built --bin-b B --workdir D --out OUT.json
                [--rows 100000] [--rounds 5] [--port 15891] [--heartbeat F]

Each binary must be a copy (bench/README.md rule 3); the driver hashes it.
"""
import argparse, hashlib, json, os, random, shutil, sys, time
from statistics import median

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
from dk_close import host_state, lat_summary  # noqa: E402
from dp_close import rotated  # noqa: E402
from dq_s0_premise import quiet_gate, settled_cpu_ns  # noqa: E402
from dr_s0_premise import Server, forms, load, settle, step_line, vkey  # noqa: E402

BATCHES = 20


def phase(c, pid, sqls, want_rows=None, want_tag=None):
    """`sqls` in BATCHES batches: the median batch's server CPU a statement,
    the batches' range, and the client's latency. A refusal, a wrong tag or
    a wrong row count ends the run."""
    per = max(1, len(sqls) // BATCHES)
    cpus, lat, rows = [], [], 0
    for at in range(0, len(sqls), per):
        batch = sqls[at:at + per]
        t0 = settled_cpu_ns(pid)
        for s in batch:
            a = time.perf_counter()
            tag, got, _, err = c.query(s)
            lat.append(time.perf_counter() - a)
            if err is not None:
                raise SystemExit(f"refused: {s[:120]} -> {err}")
            if want_tag is not None and tag != want_tag:
                raise SystemExit(f"tag {tag!r}, wanted {want_tag!r}: {s[:120]}")
            rows += got
        cpus.append((settled_cpu_ns(pid) - t0) / 1000.0 / len(batch))
    if want_rows is not None and rows != want_rows:
        raise SystemExit(f"{rows} rows, wanted {want_rows}: {sqls[0][:120]}")
    return {"stmts": len(sqls), "cpu_us": round(median(cpus), 2),
            "cpu_us_lo": round(min(cpus), 2), "cpu_us_hi": round(max(cpus), 2),
            "lat_us": lat_summary(lat)}


def keyed_relation(c, srv, a, rng, table, ddl):
    """One relation whose key is an `int64` first column: every phase of
    `--part ab`."""
    n = a.rows
    c.execute(ddl)
    out = {"load": load(c, srv.pid, table, "id, v, w", [f"({i},{i % 1000},0)" for i in range(1, n + 1)])}
    out["settle_s"] = settle(srv)
    keys = rng.sample(range(1, n + 1), a.reads)
    sel = [f"SELECT v FROM {table} WHERE id = {k}" for k in keys]
    phase(c, srv.pid, sel[:2000], 2000)  # warm
    out["read"] = phase(c, srv.pid, sel, a.reads)
    out["read_step"] = step_line(c, sel[0])
    out["update"] = phase(c, srv.pid, [f"UPDATE {table} SET w = w + 1 WHERE id = {k}"
                                       for k in keys[:a.writes]], want_tag="UPDATED 1")
    # 1,000 values of v over the rows: each walked update writes rows / 1,000.
    out["walked"] = phase(c, srv.pid, [f"UPDATE {table} SET w = w + 1 WHERE v = {k % 1000}"
                                       for k in keys[:a.walks]],
                          want_tag=f"UPDATED {n // 1000}")
    starts = [rng.randrange(1, n - 200) for _ in range(a.ranges)]
    rs = [f"SELECT v FROM {table} WHERE id >= {x} AND id <= {x + 99}" for x in starts]
    out["range"] = phase(c, srv.pid, rs, 100 * a.ranges)
    out["range_step"] = step_line(c, rs[0])
    out["insert"] = phase(c, srv.pid, [f"INSERT INTO {table} (id, v, w) VALUES ({n + 1 + j},{j},0)"
                                       for j in range(a.writes)])
    out["delete"] = phase(c, srv.pid, [f"DELETE FROM {table} WHERE id = {k}"
                                       for k in keys[a.writes:2 * a.writes]],
                          want_tag="DELETED 1")
    return out


def ab_cell(a, srv, arm, rnd):
    rng = random.Random(7919 * rnd + 11)
    c = srv.conn()
    out = {"btree": keyed_relation(c, srv, a, rng, "tb",
                                   "CREATE TABLE tb (id int64, v int64, w int64) BTREE"),
           "heap": keyed_relation(c, srv, a, rng, "th",
                                  "CREATE TABLE th (id int64 PRIMARY KEY, v int64, w int64) HEAP")}
    c.close()
    return out


def heap_form_cell(a, srv, form, rnd):
    """A heap by one of DR-S0's key forms: the statements that walked there."""
    n = a.rows
    f = forms(n)[form]
    rng = random.Random(7919 * rnd + len(form))
    c = srv.conn()
    c.execute(f["ddl"])
    order = list(range(1, n + 1))
    if f["shuffle"]:
        rng.shuffle(order)
    out = {"load": load(c, srv.pid, "t", f["cols"], [f["row"](i) for i in order])}
    out["settle_s"] = settle(srv)
    keys = rng.sample(range(1, n + 1), a.reads)
    sel = [f"SELECT v FROM t WHERE {f['pred'](i)}" for i in keys]
    phase(c, srv.pid, sel[:2000], 2000)
    out["read"] = phase(c, srv.pid, sel, a.reads)
    ups = keys[:3 * a.writes]
    out["update"] = phase(c, srv.pid, [f"UPDATE t SET w = w + 1 WHERE {f['pred'](i)}"
                                       for i in ups[:a.writes]], want_tag="UPDATED 1")
    if f["move"] is not None:
        out["move"] = phase(c, srv.pid, [f"UPDATE t SET {f['move'](i)} WHERE {f['pred'](i)}"
                                         for i in ups[a.writes:2 * a.writes]],
                            want_tag="UPDATED 1")
    out["delete"] = phase(c, srv.pid, [f"DELETE FROM t WHERE {f['pred'](i)}"
                                       for i in ups[2 * a.writes:]], want_tag="DELETED 1")
    c.close()
    return out


# A btree per form: the DDL, the INSERT's column list, a row, the predicate
# naming row i's whole key, and a range of 100 rows by the key from row x.
def btree_forms(n):
    return {
        "F0": {"ddl": "CREATE TABLE t (id int64, v int64, w int64) BTREE", "cols": "id, v, w",
               "row": lambda i: f"({i},{i},0)", "pred": lambda i: f"id = {i}",
               "range": lambda x: f"id >= {x} AND id <= {x + 99}"},
        "F1": {"ddl": "CREATE TABLE t (a int32, b int16, v int64, w int64, PRIMARY KEY (a, b))",
               "cols": "a, b, v, w", "row": lambda i: f"({i // 1000},{i % 1000},{i},0)",
               "pred": lambda i: f"a = {i // 1000} AND b = {i % 1000}",
               "range": lambda x: f"a = {x // 1000} AND b >= {x % 900} AND b <= {x % 900 + 99}"},
        "Wint": {"ddl": "CREATE TABLE t (a int32, b int32, v int64, w int64, PRIMARY KEY (a, b))",
                  "cols": "a, b, v, w", "row": lambda i: f"({i // 1000},{i % 1000},{i},0)",
                  "pred": lambda i: f"a = {i // 1000} AND b = {i % 1000}",
                  "range": lambda x: f"a = {x // 1000} AND b >= {x % 900} AND b <= {x % 900 + 99}"},
        "Wstr": {"ddl": "CREATE TABLE t (s varchar(64), v int64, w int64, PRIMARY KEY (s))",
                  "cols": "s, v, w", "row": lambda i: f"('{vkey(i)}',{i},0)",
                  "pred": lambda i: f"s = '{vkey(i)}'",
                  "range": lambda x: f"s >= '{vkey(x)}' AND s <= '{vkey(x + 99)}'"},
    }


def btree_form_cell(a, srv, form, rnd):
    n = a.rows
    f = btree_forms(n)[form]
    rng = random.Random(104729 * rnd + len(form))
    c = srv.conn()
    c.execute(f["ddl"])
    order = list(range(1000, n + 1000))  # a row (a, 0) exists for every a
    rng.shuffle(order)
    out = {"load": load(c, srv.pid, "t", f["cols"], [f["row"](i) for i in order])}
    out["leaf_divisions"] = int.from_bytes(c.execute(
        "SELECT leaf_divisions FROM sys.relation_stats WHERE name = 't'")[1][0][0], "little")
    out["data_bytes"] = os.path.getsize(os.path.join(srv.dir, "dk.db"))
    out["settle_s"] = settle(srv)
    keys = rng.sample(order, a.reads)
    sel = [f"SELECT v FROM t WHERE {f['pred'](i)}" for i in keys]
    phase(c, srv.pid, sel[:2000], 2000)
    out["read"] = phase(c, srv.pid, sel, a.reads)
    out["read_step"] = step_line(c, sel[0])
    starts = [rng.randrange(1000, n) for _ in range(a.ranges)]
    rs = [f"SELECT v FROM t WHERE {f['range'](x)}" for x in starts]
    out["range"] = phase(c, srv.pid, rs, 100 * a.ranges)
    out["range_step"] = step_line(c, rs[0])
    out["update"] = phase(c, srv.pid, [f"UPDATE t SET w = w + 1 WHERE {f['pred'](i)}"
                                       for i in keys[:a.writes]], want_tag="UPDATED 1")
    out["delete"] = phase(c, srv.pid, [f"DELETE FROM t WHERE {f['pred'](i)}"
                                       for i in keys[a.writes:2 * a.writes]],
                          want_tag="DELETED 1")
    c.close()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--part", required=True, choices=["ab", "built"])
    ap.add_argument("--bin-a")
    ap.add_argument("--bin-b", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--rows", type=int, default=100000)
    ap.add_argument("--reads", type=int, default=20000)
    ap.add_argument("--writes", type=int, default=4000)
    ap.add_argument("--walks", type=int, default=40)
    ap.add_argument("--ranges", type=int, default=1000)
    ap.add_argument("--rounds", type=int, default=5)
    ap.add_argument("--frames", type=int, default=131072)
    ap.add_argument("--port", type=int, default=15891)
    ap.add_argument("--cpu", type=int, default=2, help="the server's one CPU")
    ap.add_argument("--driver-cpus", default="0,4")
    ap.add_argument("--heartbeat", default=None)
    a = ap.parse_args()
    os.sched_setaffinity(0, {int(x) for x in a.driver_cpus.split(",")})

    def beat(line):
        stamp = time.strftime("%H:%M:%S", time.gmtime())
        print(f"{stamp} {line}", flush=True)
        if a.heartbeat:
            with open(a.heartbeat, "a") as f:
                f.write(f"{stamp} {line}\n")

    def digest(path):
        with open(path, "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()[:16]

    if a.part == "ab":
        if not a.bin_a:
            raise SystemExit("--part ab needs --bin-a")
        # The control is a second copy of A's file: another inode, the same engine.
        control = a.bin_a + ".control"
        shutil.copy(a.bin_a, control)
        bins = {"A": a.bin_a, "A2": control, "B": a.bin_b}
        plan = [(arm, ab_cell) for arm in ("A", "A2", "B")]
    else:
        bins = {}
        plan = [(f, heap_form_cell) for f in ("ref_heap", "int32", "int32x2", "varchar64", "mixed4")]
        plan += [(f, btree_form_cell) for f in ("F0", "F1", "Wint", "Wstr")]
    out = {"part": a.part, "args": vars(a), "host_before": host_state(),
           "sha256_16": {k: digest(v) for k, v in {**bins, "B": a.bin_b}.items()},
           "started_utc": time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime()), "cells": []}
    t_start = time.time()
    held_s = 0.0
    for rnd in range(a.rounds):
        for arm, cell in rotated(plan, rnd):
            held = quiet_gate(beat)
            held_s += held
            srv = Server(bins.get(arm, a.bin_b), a.workdir, f"{a.part}-{arm}-{rnd}",
                         a.port + rnd % 2, a.cpu, a.frames)
            t0 = time.time()
            try:
                res = cell(a, srv, arm, rnd)
            finally:
                srv.stop()
            out["cells"].append({"arm": arm, "round": rnd, "host": host_state(),
                                 "quiet_hold_s": held, "cell_s": round(time.time() - t0, 1),
                                 **res})
            out["quiet_hold_s"] = round(held_s, 1)
            with open(a.out, "w") as f:
                json.dump(out, f, indent=1)
            flat = res if a.part == "built" else {f"{k}.{p}": v for k, r in res.items()
                                                  for p, v in r.items()}
            beat(f"{a.part} {arm} r{rnd} {round(time.time() - t0, 1)} s "
                 + " ".join(f"{k}={v['cpu_us']}" for k, v in flat.items()
                            if isinstance(v, dict) and "cpu_us" in v))
    out["host_after"] = host_state()
    out["elapsed_s"] = round(time.time() - t_start, 1)
    with open(a.out, "w") as f:
        json.dump(out, f, indent=1)
    beat(f"done elapsed_s={out['elapsed_s']}")


if __name__ == "__main__":
    main()
