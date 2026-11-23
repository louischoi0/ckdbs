#!/usr/bin/env python3
"""DR-S0, DR-S0b and DR-S0c: what a primary key over an unchanged Keystone costs today.

`instructions/v3.2.0/workorder-dr-primary-key-over-keystone.md` section 5. One
binary, `cores = 1`, `relaxed`, every page resident, one fresh server a cell,
the cells of a round in an order rotated per round. Nothing in the engine is
changed: every cell runs statements the engine answers today.

`--measure heap` (DR-S0's M1 and M4, DR-S0b's M2 and M3). One relation `t`
of `--rows` rows per key form (`--forms`):

    ref_btree  (id int64, v, w) BTREE                    read by its tuple key
    ref_heap   (id int64 PRIMARY KEY, v, w) HEAP         the declared Keystone
    nopk_heap  (a int32, v, w) HEAP                      hidden Keystone, no key (M2 only)
    int32      (a int32, v, w, PRIMARY KEY (a)) HEAP
    int32x2    (a int32, b int32, v, w, PRIMARY KEY (a, b)) HEAP
    varchar64  (s varchar(64), v, w, PRIMARY KEY (s)) HEAP   45-byte values that
               differ inside their first 32 bytes
    mixed4     (a int32, d date, n int64, s varchar(16), v, w,
                PRIMARY KEY (a, d, n, s)) HEAP

    M2  the load: rows in 500-row INSERTs, the reference forms in key order
        and a primary key's rows in a shuffled order
    M1  --reads point reads naming the whole key
    M3  --ranges ranges of 100 rows by the key, bounded inclusively at both
        ends; as many pages (`key >= x ORDER BY key LIMIT 100`); and the
        bare `ORDER BY key LIMIT 100`
    M4  --updates UPDATEs of a column no index covers, then as many that move
        the key's last column to a free value

`--measure prefix` (DR-S0b's M5). `varchar(64)` keys sharing their first 32
bytes in runs of `--runs` rows: the load's and a point read's cost by run
length.

`--measure btree` (DR-S0c's M6 to M9). One btree `t (id int64, k
varchar(64), v int64)` of `--rows` rows per arm, the keys arriving shuffled:

    F0    id is twice the arrival order; no index           the reference
    F2e   id is the driver's 47-bit hash of k; a unique index on k
    P1e   id is issued; the same unique index, read through it

    M6  the load, with leaf divisions, the data file's size and the log's
        durable end after it
    M7  --reads point reads of present keys: F0 by id, F2e by id and k, P1e by k
    M8  --reads point reads of absent keys spread through the loaded range:
        F2e as two statements, the home (id and k) and then the index (k),
        each priced apart
    M9  M3's three shapes: F0 by id, F2e and P1e by k

Per phase: server CPU a statement (`/proc/<pid>/task/*/schedstat`, read once
no thread is runnable), client latency percentiles, and one statement's
ANALYZE step line, so fetches and descents are counted and not inferred.
After each load the driver waits until the server is quiet (`settle`), so
no phase is charged the writeback and checkpoint the load left behind.

    dr_s0_premise.py --bin B --workdir D --out OUT.json --measure heap|prefix|btree
                     [--rows 100000] [--rounds 3] [--port 15881] [--heartbeat F]

The binary must be a copy (bench/README.md rule 3); the driver hashes it.
"""
import argparse, hashlib, json, os, random, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
import sys_meta  # noqa: E402
import dk_close  # noqa: E402
from dk_close import host_state, lat_summary  # noqa: E402
from dp_close import rotated  # noqa: E402
from dq_s0_premise import quiet_gate, settled_cpu_ns  # noqa: E402

BATCH = 500


def vkey(i):
    """45 bytes; the row's number sits inside the first 32."""
    return f"acct-{i:010d}-" + "p" * 29


def bkey(i):
    return f"key-{i:010d}-" + "q" * 30


def hash47(text):
    h = int.from_bytes(hashlib.blake2b(text.encode(), digest_size=8).digest(), "little")
    return h % ((1 << 47) - 1) + 1


def day(i):
    return time.strftime("%Y-%m-%d", time.gmtime(86400 * (20000 + i % 365)))


# Per form: the DDL, the INSERT's column list, a row's VALUES tuple, the
# predicate naming row i's whole key, the SET that moves row i's key to a
# free value, and the M3 range of 100 rows starting at row x (a WHERE and
# the ORDER BY column), or None where the form has none.
def forms(n):
    return {
        **{name: {
            "ddl": ddl, "cols": "id, v, w", "row": lambda i: f"({i},{i},0)",
            "pred": lambda i: f"id = {i}", "move": None, "shuffle": False,
            "range": lambda x: (f"id >= {x} AND id <= {x + 99}", f"id >= {x}", "id")}
           for name, ddl in (
               ("ref_btree", "CREATE TABLE t (id int64, v int64, w int64) BTREE"),
               ("ref_heap", "CREATE TABLE t (id int64 PRIMARY KEY, v int64, w int64) HEAP"))},
        "nopk_heap": {
            "ddl": "CREATE TABLE t (a int32, v int64, w int64) HEAP",
            "cols": "a, v, w", "row": lambda i: f"({i},{i},0)",
            "pred": None, "move": None, "shuffle": True, "range": None},
        "int32": {
            "ddl": "CREATE TABLE t (a int32, v int64, w int64, PRIMARY KEY (a)) HEAP",
            "cols": "a, v, w", "row": lambda i: f"({i},{i},0)",
            "pred": lambda i: f"a = {i}", "move": lambda i: f"a = {n + i}", "shuffle": True,
            "range": lambda x: (f"a >= {x} AND a <= {x + 99}", f"a >= {x}", "a")},
        "int32x2": {
            "ddl": "CREATE TABLE t (a int32, b int32, v int64, w int64, PRIMARY KEY (a, b)) HEAP",
            "cols": "a, b, v, w", "row": lambda i: f"({i // 1000},{i % 1000},{i},0)",
            "pred": lambda i: f"a = {i // 1000} AND b = {i % 1000}",
            "move": lambda i: f"b = {1000 + i % 1000}", "shuffle": True,
            "range": lambda x: (f"a = {x // 1000} AND b >= {x % 900} AND b <= {x % 900 + 99}",
                                f"a = {x // 1000} AND b >= {x % 900}", "b")},
        "varchar64": {
            "ddl": "CREATE TABLE t (s varchar(64), v int64, w int64, PRIMARY KEY (s)) HEAP",
            "cols": "s, v, w", "row": lambda i: f"('{vkey(i)}',{i},0)",
            "pred": lambda i: f"s = '{vkey(i)}'",
            "move": lambda i: f"s = 'moved-{i:010d}-" + "m" * 28 + "'", "shuffle": True,
            "range": lambda x: (f"s >= '{vkey(x)}' AND s <= '{vkey(x + 99)}'",
                                f"s >= '{vkey(x)}'", "s")},
        "mixed4": {
            "ddl": "CREATE TABLE t (a int32, d date, n int64, s varchar(16), v int64, w int64, "
                   "PRIMARY KEY (a, d, n, s)) HEAP",
            "cols": "a, d, n, s, v, w",
            "row": lambda i: f"({i % 97},'{day(i)}',{i},'s{i % 1000}',{i},0)",
            "pred": lambda i: f"a = {i % 97} AND d = '{day(i)}' AND n = {i} AND s = 's{i % 1000}'",
            "move": lambda i: f"n = {10 * n + i}", "shuffle": True, "range": None},
    }


SETTLE_WINDOW_S = 0.5


class Server(dk_close.Server):
    """`dk_close.Server` at `cores = 1`, every thread on the one CPU, with
    the CPU an idle server uses in `SETTLE_WINDOW_S` read before any
    statement (`settle`'s baseline: an idle server still ticks)."""

    def __init__(self, binary, workdir, tag, port, cpu, frames):
        super().__init__(binary, workdir, tag, port, 1, [cpu], frames)
        self.pid = self.proc.pid
        dk_close.pin(self.pid, cpu)
        t0 = dk_close.cpu_ns(self.pid)
        time.sleep(SETTLE_WINDOW_S)
        self.idle_ns = dk_close.cpu_ns(self.pid) - t0


def settle(srv, limit_s=60.0):
    """Waits until the server's CPU over `SETTLE_WINDOW_S` is back within
    twice its idle baseline plus 1 ms: a load leaves writeback and a
    checkpoint behind it, on the server's one CPU, and a phase timed over
    them is charged their CPU (DR-S0's review). Returns the seconds waited,
    outside every timed window; `limit_s` where the server never settled."""
    t0 = time.time()
    last = dk_close.cpu_ns(srv.pid)
    while time.time() - t0 < limit_s:
        time.sleep(SETTLE_WINDOW_S)
        now = dk_close.cpu_ns(srv.pid)
        if now - last <= 2 * srv.idle_ns + 1_000_000:
            break
        last = now
    return round(time.time() - t0, 1)


def timed(c, pid, sqls, want_rows=None, want_tag=None):
    """Runs `sqls` and returns CPU and wall a statement and the latency
    percentiles. A refusal, a row count other than `want_rows` in total, or a
    tag other than `want_tag` ends the run: a cell that measured a statement
    answering something else is not a result."""
    lat = []
    rows = 0
    t0 = settled_cpu_ns(pid)
    w0 = time.perf_counter()
    for s in sqls:
        a = time.perf_counter()
        tag, got, _, err = c.query(s)
        lat.append(time.perf_counter() - a)
        if err is not None:
            raise SystemExit(f"refused: {s[:120]} -> {err}")
        if want_tag is not None and tag != want_tag:
            raise SystemExit(f"tag {tag!r}, wanted {want_tag!r}: {s[:120]}")
        rows += got
    wall = time.perf_counter() - w0
    cpu = settled_cpu_ns(pid) - t0
    if want_rows is not None and rows != want_rows:
        raise SystemExit(f"{rows} rows, wanted {want_rows}: {sqls[0][:120]}")
    n = len(sqls)
    return {"stmts": n, "rows": rows, "cpu_us": round(cpu / 1000.0 / n, 2),
            "wall_us": round(wall * 1e6 / n, 2), "lat_us": lat_summary(lat)}


def step_line(c, select):
    """The last `step 0` line of the statement's ANALYZE: its counters."""
    try:
        rows = c.execute("ANALYZE " + select)[1]
    except kwp.KwpError as e:
        return "REFUSED " + str(e)[:160]
    text = [" ".join((f or b"").decode() for f in r) for r in rows]
    steps = [t.strip() for t in text if t.strip().startswith("step 0") and " opens=" in t]
    return steps[-1] if steps else text[0][:200]


def refusal(c, sql):
    tag, _, _, err = c.query(sql)
    return None if err is None else str(err)[:200]


def load(c, pid, table, cols, values):
    """M2 and M6: the rows in BATCH-row statements. CPU and wall a row."""
    t0 = settled_cpu_ns(pid)
    w0 = time.perf_counter()
    for i in range(0, len(values), BATCH):
        tag, _, _, err = c.query(f"INSERT INTO {table} ({cols}) VALUES "
                                 + ",".join(values[i:i + BATCH]))
        if err is not None:
            raise SystemExit(f"load refused at row {i}: {err}")
    wall = time.perf_counter() - w0
    cpu = settled_cpu_ns(pid) - t0
    n = len(values)
    return {"rows": n, "cpu_us_row": round(cpu / 1000.0 / n, 3),
            "wall_us_row": round(wall * 1e6 / n, 3), "rows_s": round(n / wall)}


def one_int(c, sql):
    rows = c.execute(sql)[1]
    return [int.from_bytes(f, "little", signed=True) if f is not None else None for f in rows[0]]


def ranges(c, pid, rs, pg, bare, m):
    """M3 and M9: a range of 100 rows bounded inclusively at both ends
    (IX8's range), a page (`key >= x ORDER BY key LIMIT 100`), and the bare
    `ORDER BY key LIMIT 100`, each 100 rows a statement."""
    reps = [bare] * max(1, len(rs) // 5)
    return {m + "_range": timed(c, pid, rs, 100 * len(rs)),
            m + "_range_step": step_line(c, rs[0]),
            m + "_page": timed(c, pid, pg, 100 * len(pg)),
            m + "_page_step": step_line(c, pg[0]),
            m + "_bare": timed(c, pid, reps, 100 * len(reps)),
            m + "_bare_step": step_line(c, bare)}


def heap_cell(a, srv, form, rnd):
    n = a.rows
    f = forms(n)[form]
    rng = random.Random(7919 * rnd + len(form))
    c = srv.conn()
    c.execute(f["ddl"])
    order = list(range(1, n + 1))
    if f["shuffle"]:
        rng.shuffle(order)
    out = {"m2_load": load(c, srv.pid, "t", f["cols"], [f["row"](i) for i in order])}
    out["settle_s"] = settle(srv)
    if f["pred"] is None:
        c.close()
        return out
    keys = rng.sample(range(1, n + 1), a.reads)
    sel = lambda i: f"SELECT v FROM t WHERE {f['pred'](i)}"  # noqa: E731
    timed(c, srv.pid, [sel(i) for i in keys[:2000]], 2000)  # warm
    out["m1_read"] = timed(c, srv.pid, [sel(i) for i in keys], a.reads)
    out["m1_step"] = step_line(c, sel(keys[0]))
    if f["range"] is not None:
        starts = [rng.randrange(1000, n - 200) for _ in range(a.ranges)]
        col = f["range"](1000)[2]
        out.update(ranges(
            c, srv.pid,
            [f"SELECT v FROM t WHERE {f['range'](x)[0]}" for x in starts],
            [f"SELECT v FROM t WHERE {f['range'](x)[1]} ORDER BY {col} LIMIT 100" for x in starts],
            f"SELECT v FROM t ORDER BY {'a, b' if form == 'int32x2' else col} LIMIT 100", "m3"))
    ups = rng.sample(range(1, n + 1), 2 * a.updates)
    plain = [f"UPDATE t SET w = w + 1 WHERE {f['pred'](i)}" for i in ups[:a.updates]]
    out["m4_plain"] = timed(c, srv.pid, plain, want_tag="UPDATED 1")
    if f["move"] is None:
        out["m4_move_refused"] = refusal(c, f"UPDATE t SET id = {n + 1} WHERE id = {ups[-1]}")
    else:
        move = [f"UPDATE t SET {f['move'](i)} WHERE {f['pred'](i)}" for i in ups[a.updates:]]
        out["m4_move"] = timed(c, srv.pid, move, want_tag="UPDATED 1")
    c.close()
    return out


def prefix_cell(a, srv, run, rnd):
    """M5: `total` varchar keys in runs of `run` sharing their first 32 bytes."""
    total = a.prefix_rows
    rng = random.Random(31 * rnd + run)
    c = srv.conn()
    c.execute("CREATE TABLE t (s varchar(64), v int64, w int64, PRIMARY KEY (s)) HEAP")
    key = lambda i: f"run-{i // run:08d}-" + "z" * 19 + f"{i % run:06d}"  # noqa: E731
    assert len(key(0)[:32]) == 32 and key(0)[:32] == key(run - 1)[:32]
    order = list(range(total))
    rng.shuffle(order)
    out = {"m5_load": load(c, srv.pid, "t", "s, v, w", [f"('{key(i)}',{i},0)" for i in order])}
    out["settle_s"] = settle(srv)
    keys = rng.sample(range(total), min(total, a.prefix_reads))
    sel = lambda i: f"SELECT v FROM t WHERE s = '{key(i)}'"  # noqa: E731
    timed(c, srv.pid, [sel(i) for i in keys[:500]], 500)
    out["m5_read"] = timed(c, srv.pid, [sel(i) for i in keys], len(keys))
    out["m5_step"] = step_line(c, sel(keys[0]))
    out["m5_duplicate"] = refusal(c, f"INSERT INTO t (s, v, w) VALUES ('{key(keys[0])}',0,0)")
    c.close()
    return out


def btree_cell(a, srv, arm, rnd):
    n = a.rows
    rng = random.Random(104729 * rnd + n)
    c = srv.conn()
    c.execute("CREATE TABLE t (id int64, k varchar(64), v int64) BTREE")
    if arm != "F0":
        c.execute("CREATE UNIQUE INDEX t_k ON t (k)")
    order = list(range(1, n + 1))
    rng.shuffle(order)
    homes = {}
    if arm == "F2e":
        taken = set()
        spill = (1 << 47)
        for i in order:
            h = hash47(bkey(i))
            if h in taken:  # a colliding key takes an id of the issued half
                h = spill
                spill += 1
            taken.add(h)
            homes[i] = h
    if arm == "F0":
        cols, vals = "id, k, v", [f"({2 * (j + 1)},'{bkey(i)}',{i})" for j, i in enumerate(order)]
        ident = {i: 2 * (j + 1) for j, i in enumerate(order)}
    elif arm == "F2e":
        cols, vals = "id, k, v", [f"({homes[i]},'{bkey(i)}',{i})" for i in order]
    else:
        cols, vals = "k, v", [f"('{bkey(i)}',{i})" for i in order]
    out = {"m6_load": load(c, srv.pid, "t", cols, vals),
           "collisions": sum(1 for h in homes.values() if h >= (1 << 47))}
    out["m6_leaf_divisions"] = one_int(
        c, "SELECT leaf_divisions, leaf_compactions FROM sys.relation_stats WHERE name = 't'")
    if arm != "F0":
        out["m6_index_divisions"] = one_int(
            c, "SELECT leaf_divisions FROM sys.indexes WHERE name = 't_k'")
    # What the load left: the data file's size, and the log's durable end -
    # the stream's byte position (`sys.meta`'s `wal_durable_lsn`), read once
    # the server is quiet, outside every timed window. A fresh server's own
    # records are in it for every arm alike.
    out["m6_data_bytes"] = os.path.getsize(os.path.join(srv.dir, "dk.db"))
    out["settle_s"] = settle(srv)
    _, rows, tag, _ = c.execute("SHOW META")
    lsn = sys_meta.from_rows(rows, tag).get("wal_durable_lsn")
    out["m6_wal_durable_lsn"] = int(lsn) if lsn not in (None, "") else None

    keys = rng.sample(range(1, n + 1), a.reads)
    # Absent keys spread through the loaded range, so every arm misses on a
    # leaf chosen as a hit's is: a key sorting just above a present one and
    # differing from every key inside the indexed 32 bytes (a key sharing
    # them would name the present row's entry, M5's case), and for F0 an id
    # between two placed ones (ids are placed two apart for it).
    absent = [f"key-{i:010d}." + "q" * 30 for i in rng.sample(range(1, n + 1), a.reads)]
    if arm == "F0":
        present = lambda i: f"SELECT v FROM t WHERE id = {ident[i]}"  # noqa: E731
        gone = [[f"SELECT v FROM t WHERE id = {2 * i + 1}"
                 for i in rng.sample(range(1, n), a.reads)]]
    elif arm == "F2e":
        present = lambda i: f"SELECT v FROM t WHERE id = {homes[i]} AND k = '{bkey(i)}'"  # noqa: E731
        gone = [[f"SELECT v FROM t WHERE id = {hash47(k)} AND k = '{k}'" for k in absent],
                [f"SELECT v FROM t WHERE k = '{k}'" for k in absent]]
    else:
        present = lambda i: f"SELECT v FROM t WHERE k = '{bkey(i)}'"  # noqa: E731
        gone = [[f"SELECT v FROM t WHERE k = '{k}'" for k in absent]]
    timed(c, srv.pid, [present(i) for i in keys[:2000]], 2000)
    out["m7_present"] = timed(c, srv.pid, [present(i) for i in keys], a.reads)
    out["m7_step"] = step_line(c, present(keys[0]))
    out["m8_absent"] = [timed(c, srv.pid, g, 0) for g in gone]
    out["m8_step"] = [step_line(c, g[0]) for g in gone]

    starts = [rng.randrange(1, n - 200) for _ in range(a.ranges)]
    if arm == "F0":
        rs = [f"SELECT v FROM t WHERE id >= {2 * x} AND id <= {2 * x + 198}" for x in starts]
        pg = [f"SELECT v FROM t WHERE id >= {2 * x} ORDER BY id LIMIT 100" for x in starts]
        bare = "SELECT v FROM t ORDER BY id LIMIT 100"
    else:
        rs = [f"SELECT v FROM t WHERE k >= '{bkey(x)}' AND k <= '{bkey(x + 99)}'" for x in starts]
        pg = [f"SELECT v FROM t WHERE k >= '{bkey(x)}' ORDER BY k LIMIT 100" for x in starts]
        bare = "SELECT v FROM t ORDER BY k LIMIT 100"
    out.update(ranges(c, srv.pid, rs, pg, bare, "m9"))
    c.close()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--measure", required=True, choices=["heap", "prefix", "btree"])
    ap.add_argument("--forms", default="ref_btree,ref_heap,nopk_heap,int32,int32x2,varchar64,mixed4")
    ap.add_argument("--arms", default="F0,F2e,P1e")
    ap.add_argument("--runs", default="1,16,256,4096")
    ap.add_argument("--rows", type=int, default=100000)
    ap.add_argument("--reads", type=int, default=20000)
    ap.add_argument("--ranges", type=int, default=1000)
    ap.add_argument("--updates", type=int, default=500)
    ap.add_argument("--prefix-rows", type=int, default=8192)
    ap.add_argument("--prefix-reads", type=int, default=4000)
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--frames", type=int, default=131072)
    ap.add_argument("--port", type=int, default=15881)
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

    with open(a.bin, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()[:16]
    out = {"binary": a.bin, "sha256_16": digest, "args": vars(a), "host_before": host_state(),
           "started_utc": time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime()), "cells": []}
    kinds = {"heap": (a.forms.split(","), heap_cell),
             "prefix": ([int(x) for x in a.runs.split(",")], prefix_cell),
             "btree": (a.arms.split(","), btree_cell)}
    arms, cell = kinds[a.measure]
    t_start = time.time()
    held_s = 0.0
    for rnd in range(a.rounds):
        for arm in rotated(arms, rnd):
            held = quiet_gate(beat)
            held_s += held
            srv = Server(a.bin, a.workdir, f"{a.measure}-{arm}-{rnd}", a.port + rnd % 2, a.cpu,
                         a.frames)
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
            beat(f"{a.measure} {arm} r{rnd} {round(time.time() - t0, 1)} s "
                 + " ".join(f"{k}={v['cpu_us']}" for k, v in res.items()
                            if isinstance(v, dict) and "cpu_us" in v))
    out["host_after"] = host_state()
    out["elapsed_s"] = round(time.time() - t_start, 1)
    with open(a.out, "w") as f:
        json.dump(out, f, indent=1)
    beat(f"done elapsed_s={out['elapsed_s']}")


if __name__ == "__main__":
    main()
