#!/usr/bin/env python3
"""BM-S1's census: a scanned row's server cost, by shape, on KDS.

    bm_scan.py census  --bin BIN --workdir DIR [--port P] [--runs 8]
                       [--pin-server 2] [--pin-driver 4] [--cells a,b,...]
                       [--quiet-load 3 --quiet-wait 600] --out OUT.json
    bm_scan.py writers --bin BIN --workdir DIR [--port P] [--runs 8]
                       [--pin-driver 4] [--pin-writer 0]
                       [--reactor-cpus 2,6] [--pin-server 2,3,6,7]
                       [--variant point,range] --out OUT.json
    bm_scan.py profile --bin BIN_FP --sampler SO --sampler-dir DIR
                       --workdir DIR [--profile-cells bn,wide-h]
                       [--profile-scans 20000] [--writers]
                       [--variant point] --out OUT.json

`instructions/v3.0.0/workorder-bm-scan-row-kernel.md` §5.1 (BM-S1). It reuses
`tools/bn_census.py`'s server, schedstat reader, literal renderer, quiet gate
and host record, and measures the same two things per statement: the
client's wall time and **the server's on-CPU time** (the sum over every
server thread of /proc/<pid>/task/*/schedstat). The scan row is the net
`(scan - point) / rows`: `point` is `SELECT * FROM <rel> WHERE id = ?` on the
same relation, same path, same run.

`census`: per run a fresh server and data file (`cores = 1`, `relaxed`,
`buffer_pool_frames = 65536`), every relation loaded, then every cell over
two driver paths - **literal** (an unnamed C_PARSE of the text per statement)
and **prepared** (one named C_PARSE, then BIND/EXECUTE) - cut into blocks
taken alternately by the two paths, the first path alternating by block and
run, so host drift lands on both. `contention_visibility_window_lookups`
(SHOW META, BM-S1's `Tally::kWindowLookups`) is read around every block.

The cells (name, relation, SQL):

  rows-200, rows-1k, bn,  s200 / s1k / s /   SELECT * FROM <rel> WHERE w = ?
  rows-100k               s100k              200 / 1,000 / 10,000 / 100,000 rows
  bn-recent               srec               the bn shape on a relation loaded last,
                                             its final 500-row INSERT above the
                                             visibility floor (5 % of rows take
                                             LookupCommit, `flood_commits`)
  sel-0, sel-1, sel-10,   s                  SELECT * FROM s WHERE w < ?
  sel-100                                    0 / 100 / 1,000 / 10,000 rows out
  wide-h                  wd  (13 columns)   WHERE h = ?   (decimal(30,4))
  wide-h-k                                   WHERE h = ? AND k = ?
  wide-a|c|d|e|f|g|k|m                       WHERE <col> = ?  (one per kernel
                                             type family; d is true on one row)
  nul-0, nul-50           nul0 / nul50       SELECT * FROM <rel> WHERE n = ?
                                             over (id, n int64 NULL, w);
                                             0 % / 50 % of n NULL

`writers` (BM-Q8): `cores = 2`, `group`, the scan session on core 1 and a
second session, a child process on its own CPU, on core 0 updating `s.v` by
primary key (`point`: `UPDATE s SET v = ? WHERE id = ?`, back to back, each
waiting for its group commit) or, as the extreme the point writer does not
reach, `range` (`UPDATE s SET v = v + 1 WHERE id >= 1`, every row). Blocks of
the bn scan alternate between "writer going" and "writer stopped"; each
reports lookups per scanned row, the scan core's reactor CPU and the
scan's wall percentiles. No PostgreSQL twin.

`profile`: the sampler (`tools/kds_sampler.c`, a frame-pointer build)
brackets `--profile-scans` literal scans of each `--profile-cells` cell; the
scan's and an unsampled point statement's server CPU are recorded beside the
dump so `tools/kds_sampler_report.py --parts tools/bn_parts.json` can be
turned into ns per row. With `--writers` the writers cell is sampled with the
writer going and again without it.
"""
import argparse, json, os, signal, subprocess, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
import bn_census as bc  # noqa: E402
from bn_census import (Server, sched_ns, self_ns, literal_of, quiet_gate, host_state,  # noqa: E402
                       pct, must)

TARGET_ID = lambda q, rows: 1 + (q * 7919) % rows
BASE_DATE = (2026, 1, 1)
LONG = "x" * 292  # p = 'p%08d' + LONG: 301 bytes, over the instance's inline cell (64)
WIDE_ROWS = 10000
D_ONE = 5000  # the one row on which wd.d is true

WIDE_COLS = ("id int64, a int32, b int64, c uint64, d bool, e date, f timestamp, "
             "g decimal(12,2), h decimal(30,4), k char(16), m varchar, n int64 NULL, p varchar")


# -- values: one place, both engines ------------------------------------------

def _civil(days):
    """The (y, m, d) of 2026-01-01 + `days`."""
    import datetime
    d = datetime.date(*BASE_DATE) + datetime.timedelta(days=days)
    return d.isoformat()


def _stamp(secs):
    import datetime
    t = datetime.datetime(*BASE_DATE) + datetime.timedelta(seconds=secs)
    return t.strftime("%Y-%m-%d %H:%M:%S")


def wide_values(i):
    """Row i of `wd` as a dict of column -> SQL text, dialect-neutral except `d`."""
    return {"id": str(i), "a": str(i), "b": str(i), "c": str(i),
            "d": "1" if i == D_ONE else "0",
            "e": "'%s'" % _civil(i), "f": "'%s'" % _stamp(i),
            "g": "%d.%02d" % (i // 100, i % 100), "h": "%d.%04d" % (i, (i * 7) % 10000),
            "k": "'k%08d'" % i, "m": "'m%08d'" % i, "n": str(i),
            "p": "'p%08d%s'" % (i, LONG)}


def wide_row_sql(i, dialect):
    v = wide_values(i)
    if dialect == "pg":
        v["d"] = "true" if i == D_ONE else "false"
    return "(" + ", ".join(v[c] for c in ("id", "a", "b", "c", "d", "e", "f", "g", "h", "k", "m",
                                           "n", "p")) + ")"


def nul_row_sql(i, pct_null):
    """nul0: n = i. nul50: n = i on even i, NULL on odd."""
    n = "NULL" if (pct_null == 50 and i % 2 == 1) else str(i)
    return "(%d, %s, %d)" % (i, n, i)


# -- cells ----------------------------------------------------------------------

class Cell:
    def __init__(self, name, rel, rows, sql, params, expect, ops, group, pg_sql=None):
        self.name, self.rel, self.rows, self.sql = name, rel, rows, sql
        self.params, self.expect, self.ops, self.group = params, expect, ops, group
        self.pg_sql = pg_sql  # None: the same text as `sql`


def _t(rows):
    return lambda q: TARGET_ID(q, rows)


def make_cells(a):
    i64 = kwp.Connection.int64
    vc = lambda s: (kwp.T_VARCHAR, 0, s.encode())
    one = lambda q: 1
    W = WIDE_ROWS
    wv = lambda col: (lambda q: wide_values(TARGET_ID(q, W))[col].strip("'"))
    cells = []
    k = a.scan_ops
    for name, rel, rows, ops in (("rows-200", "s200", 200, k * 8), ("rows-1k", "s1k", 1000, k * 4),
                                 ("bn", "s", 10000, k),
                                 ("rows-100k", "s100k", 100000, max(12, k // 4))):
        cells.append(Cell(name, rel, rows, f"SELECT * FROM {rel} WHERE w = ?",
                          lambda q, rows=rows: [i64(TARGET_ID(q, rows))], one, ops, "rows"))
    cells.append(Cell("bn-recent", "srec", 10000, "SELECT * FROM srec WHERE w = ?",
                      lambda q: [i64(TARGET_ID(q, 10000))], one, k, "recent"))
    for name, bound, out, ops in (("sel-0", 1, 0, k), ("sel-1", 101, 100, k),
                                  ("sel-10", 1001, 1000, k), ("sel-100", 10001, 10000, k // 2)):
        cells.append(Cell(name, "s", 10000, "SELECT * FROM s WHERE w < ?",
                          lambda q, bound=bound: [i64(bound)], lambda q, out=out: out, ops,
                          "selectivity"))
    cells.append(Cell("wide-h", "wd", W, "SELECT * FROM wd WHERE h = ?",
                      lambda q: [vc(wv("h")(q))], one, k, "width",
                      pg_sql="SELECT * FROM wd WHERE h = ?"))
    cells.append(Cell("wide-h-k", "wd", W, "SELECT * FROM wd WHERE h = ? AND k = ?",
                      lambda q: [vc(wv("h")(q)), vc(wv("k")(q))], one, k, "width"))
    for col, kind in (("a", "i"), ("c", "i"), ("d", "d"), ("e", "s"), ("f", "s"), ("g", "s"),
                      ("k", "s"), ("m", "s")):
        if kind == "i":
            pf = lambda q, col=col: [i64(int(wv(col)(q)))]
        elif kind == "d":
            pf = lambda q: [i64(1)]
        else:
            pf = lambda q, col=col: [vc(wv(col)(q))]
        cells.append(Cell("wide-" + col, "wd", W, f"SELECT * FROM wd WHERE {col} = ?", pf, one, k,
                          "width"))
    for name, rel, pn in (("nul-0", "nul0", 0), ("nul-50", "nul50", 50)):
        tgt = (lambda q: TARGET_ID(q, 10000)) if pn == 0 else \
            (lambda q: 2 * (1 + (q * 7919) % 5000))
        cells.append(Cell(name, rel, 10000, f"SELECT * FROM {rel} WHERE n = ?",
                          lambda q, tgt=tgt: [i64(tgt(q))], one, k, "nullable"))
    return cells


RELS = {  # relation -> (columns, rows, row text)
    "s200": ("id int64, v int64, w int64", 200, lambda i: f"({i}, {i}, {i})"),
    "s1k": ("id int64, v int64, w int64", 1000, lambda i: f"({i}, {i}, {i})"),
    "s": ("id int64, v int64, w int64", 10000, lambda i: f"({i}, {i}, {i})"),
    "s100k": ("id int64, v int64, w int64", 100000, lambda i: f"({i}, {i}, {i})"),
    "wd": (WIDE_COLS, WIDE_ROWS, lambda i: wide_row_sql(i, "kds")),
    "nul0": ("id int64, n int64 NULL, w int64", 10000, lambda i: nul_row_sql(i, 0)),
    "nul50": ("id int64, n int64 NULL, w int64", 10000, lambda i: nul_row_sql(i, 50)),
    # Loaded last, with no commit after it: its final INSERT batch (500 rows)
    # is the instance's latest commit, which the visibility floor has not
    # passed, so those rows take `LookupCommit` (BM-S1's `bn-recent`).
    "srec": ("id int64, v int64, w int64", 10000, lambda i: f"({i}, {i}, {i})"),
}


def load_rel(conn, name):
    cols, rows, row = RELS[name]
    must(conn, f"CREATE TABLE {name} ({cols}) BTREE", "CREATE")
    step = 200 if name == "wd" else 500
    for lo in range(1, rows + 1, step):
        hi = min(rows, lo + step - 1)
        must(conn, f"INSERT INTO {name} VALUES " + ",".join(row(i) for i in range(lo, hi + 1)),
             "INSERT")


# -- measurement ------------------------------------------------------------------

def meta_vals(conn, keys=()):
    """SHOW META as a dict of every `key=int` token (or only `keys`)."""
    _, rows, tag, _ = conn.execute("SHOW META")
    text = tag + " " + " ".join(b" ".join(c or b"" for c in r).decode() for r in rows)
    out = {}
    for tok in text.replace("\\n", " ").split():
        if "=" not in tok:
            continue
        k, v = tok.split("=", 1)
        if keys and k not in keys:
            continue
        try:
            out[k] = int(v)
        except ValueError:
            pass
    return out


def lookups_of(m):
    """(total, per-core dict) of window lookups from a meta dict."""
    total = m.get("contention_visibility_window_lookups", 0)
    per = {k: v for k, v in m.items() if k.startswith("load_c") and
           k.endswith("_visibility_window_lookups")}
    return total, per


META_KEYS = {"contention_visibility_window_lookups", "contention_window_wait_us",
             "contention_window_waits", "core", "pool_misses"} | \
    {f"load_c{k}_visibility_window_lookups" for k in range(8)}


FLOOD = 2200


def flood_commits(conn, n, first=1):
    """`n` one-row relaxed transactions on the scratch relation `z`.

    **Why.** The visibility floor (`InstanceVisibility::Reclaim`) rises when
    the commit window reaches 1,024 entries (and on an undo page's growth, at
    mount, at an idle core's burn), not per commit; a row whose writer's
    commit is above it takes `LookupCommit`, so a freshly loaded relation has
    a tail of rows on the window path that a later read-only workload never
    clears. Enough commits to fill the window twice guarantee one pass, which
    puts every earlier commit below the floor - the floor path BN's shape
    ran on. Relaxed, so a `group` server does not wait on a sync per commit."""
    for k in range(first, first + n):
        conn.begin(3)
        conn.execute(f"INSERT INTO z VALUES ({k}, {k})")
        conn.commit()
    return first + n


def wait_idle(srv, limit_s=90.0, ceiling_us_per_s=10000):
    """Waits for the server's CPU to fall to idle burn (the checkpoint and the
    pool's background writeback after a load): two consecutive one-second
    windows at or under the ceiling. Returns (waited s, last us/s)."""
    t0 = time.time()
    quiet = 0
    while True:
        s0 = sched_ns(srv.proc.pid)
        time.sleep(1.0)
        us = (sched_ns(srv.proc.pid) - s0) / 1e3
        quiet = quiet + 1 if us <= ceiling_us_per_s else 0
        if quiet >= 2 or time.time() - t0 >= limit_s:
            return round(time.time() - t0, 1), round(us, 1)


def run_stmts(srv, conn, cell_sql, params_fn, path, qs, expect, stmt_name, tag="SELECT"):
    """Runs statements `qs`; returns (lat list, bad count)."""
    lat, bad = [], 0
    for q in qs:
        p = params_fn(q)
        t0 = time.perf_counter()
        if path == "prepared":
            _, rows, got, _ = conn.execute_prepared(stmt_name, p)
        else:
            _, rows, got, _ = conn.execute(literal_of(cell_sql, p))
        lat.append(time.perf_counter() - t0)
        if not got.startswith(tag) or len(rows) != expect(q):
            bad += 1
    return lat, bad


def summarize_lat(lat):
    us = [x * 1e6 for x in lat]
    return {"p0": min(us), "p25": pct(us, 25), "p50": pct(us, 50), "p90": pct(us, 90),
            "p95": pct(us, 95), "p99": pct(us, 99), "mean": sum(us) / len(us)}


def measure_arm(srv, cell, kind, ops, a, r, q0, cores_meta=False):
    """One cell (kind 'scan') or its relation's point arm (kind 'point'), both
    paths, cut into blocks taken alternately. Returns {path: arm dict}."""
    conn = srv.conn
    if kind == "point":
        sql = f"SELECT * FROM {cell.rel} WHERE id = ?"
        rows_fn = lambda q: [kwp.Connection.int64(TARGET_ID(q, RELS[cell.rel][1]))]
        expect = lambda q: 1
    else:
        sql, rows_fn, expect = cell.sql, cell.params, cell.expect
    name = f"m_{kind}_{cell.name if kind == 'scan' else cell.rel}"
    if name not in srv.prepared:
        conn.prepare(name, sql)
        srv.prepared.add(name)
    names = {"literal": name, "prepared": name}
    per = max(1, ops // a.blocks)
    accs = {p: {"lat": [], "bad": 0, "server_ns": 0, "driver_ns": 0, "lookups": 0,
                "window_wait_us": 0, "block_server_us": [], "ops": 0, "pool_misses": 0}
            for p in names}
    # Untimed warm-up on each path: first-touch of the plan and the pages.
    for path in ("literal", "prepared"):
        run_stmts(srv, conn, sql, rows_fn, path, range(q0, q0 + 3), expect, names[path])
    q = q0 + 3
    for b in range(a.blocks):
        order = ("literal", "prepared") if (b + r) % 2 == 0 else ("prepared", "literal")
        for path in order:
            acc = accs[path]
            qs = range(q, q + per)
            q += per
            m0 = meta_vals(conn, META_KEYS)
            s0, c0 = sched_ns(srv.proc.pid), self_ns()
            lat, bad = run_stmts(srv, conn, sql, rows_fn, path, qs, expect, names[path])
            s1, c1 = sched_ns(srv.proc.pid), self_ns()
            m1 = meta_vals(conn, META_KEYS)
            acc["lat"].extend(lat)
            acc["bad"] += bad
            acc["ops"] += len(qs)
            acc["server_ns"] += s1 - s0
            acc["driver_ns"] += c1 - c0
            acc["lookups"] += lookups_of(m1)[0] - lookups_of(m0)[0]
            acc["window_wait_us"] += m1.get("contention_window_wait_us", 0) - \
                m0.get("contention_window_wait_us", 0)
            acc["pool_misses"] += m1.get("pool_misses", 0) - m0.get("pool_misses", 0)
            acc["block_server_us"].append(round((s1 - s0) / len(qs) / 1e3, 2))
    out = {}
    for path, acc in accs.items():
        n = acc["ops"]
        res = {"cell": cell.name if kind == "scan" else "point:" + cell.rel, "kind": kind,
               "rel": cell.rel, "path": path, "ops": n,
               "bad": acc["bad"], "rows": cell.rows if kind == "scan" else 1,
               "server_cpu_us": acc["server_ns"] / n / 1e3,
               "driver_cpu_us": acc["driver_ns"] / n / 1e3,
               "lookups": acc["lookups"], "window_wait_us": acc["window_wait_us"],
               "pool_misses": acc["pool_misses"], "block_server_us": acc["block_server_us"],
               "lat_us": [round(x * 1e6, 1) for x in acc["lat"]]}
        res.update({"p_" + k: v for k, v in summarize_lat(acc["lat"]).items()})
        out[path] = res
    return out, q


def one_census_run(a, r, cells, rels):
    waited, load = quiet_gate(a)
    out = {"run": r, "host_before": host_state(), "arms": [],
           "quiet_gate": {"waited_s": waited, "load": load}}
    srv = Server(a, "bm", a.bin, a.port, f"r{r}")
    try:
        t0 = time.time()
        must(srv.conn, "CREATE TABLE z (id int64, v int64) BTREE", "CREATE")
        for name in rels:
            if name == "srec":
                # The floor passes everything loaded so far (see flood_commits);
                # `srec`, loaded after it, is the one relation with rows above it.
                flood_commits(srv.conn, FLOOD)
            load_rel(srv.conn, name)
        if "srec" not in rels:
            flood_commits(srv.conn, FLOOD)
        out["load_s"] = round(time.time() - t0, 1)
        out["idle_wait"] = wait_idle(srv)
        # A SHOW META costs lookups itself (its catalog reads): the control
        # the per-block deltas include once, measured here back to back.
        m0 = meta_vals(srv.conn, META_KEYS)
        m1 = meta_vals(srv.conn, META_KEYS)
        out["meta_self_lookups"] = lookups_of(m1)[0] - lookups_of(m0)[0]
        s0 = sched_ns(srv.proc.pid)
        time.sleep(1.0)
        out["idle_cpu_us_per_s"] = (sched_ns(srv.proc.pid) - s0) / 1e3
        # An untimed prelude: the first few hundred statements of a fresh
        # server fault catalog and system pages (`pool_misses`), which would
        # land in the first arm's point statement.
        m0 = meta_vals(srv.conn, META_KEYS)
        rel0 = cells[0].rel
        for q in range(1, 401):
            srv.conn.execute("SELECT * FROM %s WHERE id = %d" % (rel0, TARGET_ID(q, RELS[rel0][1])))
        out["prelude_pool_misses"] = meta_vals(srv.conn, META_KEYS).get("pool_misses", 0) - \
            m0.get("pool_misses", 0)
        out["analyze"] = {}
        done_points = set()
        for cell in cells:
            if cell.rel not in done_points:
                done_points.add(cell.rel)
                host = host_state()
                arms, _ = measure_arm(srv, cell, "point", a.point_ops, a, r, 1)
                for arm in arms.values():
                    arm["host"] = host
                    out["arms"].append(arm)
            host = host_state()
            arms, _ = measure_arm(srv, cell, "scan", cell.ops, a, r, 100)
            for path, arm in arms.items():
                arm["host"] = host
                out["arms"].append(arm)
                print(json.dumps({k: (round(v, 2) if isinstance(v, float) else v)
                                  for k, v in arm.items()
                                  if k not in ("host", "block_server_us", "lat_us")}), flush=True)
            if r == 0:
                try:
                    q = 7
                    _, rows, tag, _ = srv.conn.execute(
                        "ANALYZE " + literal_of(cell.sql, cell.params(q)))
                    out["analyze"][cell.name] = tag + " | " + " | ".join(
                        b" ".join(c or b"" for c in rw).decode(errors="replace") for rw in rows[:6])
                except Exception as e:  # noqa: BLE001 - recorded, not fatal
                    out["analyze"][cell.name] = "ERR %s" % str(e)[:200]
        out["final_meta_lookups"] = lookups_of(meta_vals(srv.conn, META_KEYS))[0]
    finally:
        srv.stop()
    out["host_after"] = host_state()
    return out


def census(a):
    cells = make_cells(a)
    if a.cells:
        want = a.cells.split(",")
        cells = [c for c in cells if c.name in want]
    rels = [n for n in RELS if any(c.rel == n for c in cells)]  # RELS order: srec is last
    os.sched_setaffinity(0, {int(a.pin_driver)})
    result = {"args": vars(a), "runs": []}
    for r in range(a.runs):
        result["runs"].append(one_census_run(a, r, cells, rels))
        with open(a.out, "w") as f:
            json.dump(result, f, indent=1)


# -- the writer cell ------------------------------------------------------------------

def writer_child(a):
    """A session on its own CPU that updates `s` while told to (see `writers`).
    Commands on stdin: `go`, `pause` (answers `PAUSED n=<updates> bad=<wrong
    replies>` once the in-flight statement has returned), `quit`."""
    os.sched_setaffinity(0, {int(a.child_cpu)})
    rows = 10000
    while True:
        conn = kwp.Connection("127.0.0.1", a.port, timeout=120)
        core = meta_vals(conn, {"core"}).get("core", -1)
        print("READY core=%d" % core, flush=True)
        if sys.stdin.readline().strip() == "ok":
            break
        conn.close()
    st = {"go": False, "quit": False, "busy": False, "n": 0, "bad": 0}
    cv = threading.Condition()

    def loop():
        q = 0
        while True:
            with cv:
                while not st["go"] and not st["quit"]:
                    cv.wait()
                if st["quit"]:
                    return
                st["busy"] = True
            q += 1
            if a.variant == "point":
                sql = "UPDATE s SET v = %d WHERE id = %d" % (q, TARGET_ID(q, rows))
                want = "UPDATED 1"
            else:
                sql = "UPDATE s SET v = v + 1 WHERE id >= 1"
                want = "UPDATED %d" % rows
            _, _, got, _ = conn.execute(sql)
            with cv:
                st["busy"] = False
                st["n"] += 1
                st["bad"] += 0 if got.startswith(want) else 1
                cv.notify_all()

    threading.Thread(target=loop, daemon=True).start()
    for line in sys.stdin:
        cmd = line.strip()
        with cv:
            if cmd == "go":
                st["go"] = True
                cv.notify_all()
            elif cmd == "pause":
                st["go"] = False
                while st["busy"]:
                    cv.wait()
                n, bad, st["n"], st["bad"] = st["n"], st["bad"], 0, 0
            elif cmd == "quit":
                st["quit"] = True
                cv.notify_all()
        if cmd == "pause":
            core = meta_vals(conn, {"core"}).get("core", -1)
            print("PAUSED n=%d bad=%d core=%d" % (n, bad, core), flush=True)
        elif cmd == "quit":
            break


class Writer:
    def __init__(self, a, avoid_core, variant):
        cmd = [sys.executable, os.path.abspath(__file__), "writer-child", "--port", str(a.port),
               "--child-cpu", str(a.pin_writer), "--avoid-core", str(avoid_core),
               "--variant", variant, "--workdir", a.workdir]
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                                  bufsize=1)
        self.core = None
        for _ in range(40):
            line = self.p.stdout.readline().strip()
            if not line.startswith("READY"):
                raise SystemExit("writer child: %r" % line)
            self.core = int(line.split("core=")[1])
            if self.core != avoid_core:
                self.p.stdin.write("ok\n")
                self.p.stdin.flush()
                return
            self.p.stdin.write("retry\n")
            self.p.stdin.flush()
        raise SystemExit("writer session never landed off core %d" % avoid_core)

    def go(self):
        self.p.stdin.write("go\n")
        self.p.stdin.flush()

    def pause(self):
        self.p.stdin.write("pause\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline().strip()
        n = int(line.split("n=")[1].split()[0])
        bad = int(line.split("bad=")[1].split()[0])
        self.core = int(line.split("core=")[1])
        return n, bad

    def quit(self):
        try:
            self.p.stdin.write("quit\n")
            self.p.stdin.flush()
        except OSError:
            pass
        try:
            self.p.wait(10)
        except subprocess.TimeoutExpired:
            self.p.kill()


def reactor_threads(pid, cpus):
    """{core index: tid} by the CPU each reactor thread is pinned to."""
    want = [int(x) for x in cpus.split(",")]
    out = {}
    for tid in os.listdir(f"/proc/{pid}/task"):
        aff = os.sched_getaffinity(int(tid))
        if len(aff) == 1:
            (c,) = aff
            if c in want:
                out[want.index(c)] = int(tid)
    return out


def tid_ns(pid, tid):
    with open(f"/proc/{pid}/task/{tid}/schedstat") as f:
        return int(f.read().split()[0])


def session_on_core(srv, core):
    """Reconnects srv.conn until its session lands on `core`."""
    for _ in range(60):
        if meta_vals(srv.conn, {"core"}).get("core") == core:
            return
        srv.conn.close()
        srv.conn = kwp.Connection("127.0.0.1", srv.port, timeout=120)
        srv.prepared = set()
    raise SystemExit("scan session never landed on core %d" % core)


def session_back(srv, conn, core):
    """The scan session, on `core` again if the load placer moved it between
    two statements (BL-S3); returns the connection to use."""
    if meta_vals(conn, {"core"}).get("core") == core:
        return conn
    srv.conn = conn
    session_on_core(srv, core)
    return srv.conn


def writers_run(a, r, variant, a2):
    waited, load = quiet_gate(a)
    out = {"run": r, "variant": variant, "host_before": host_state(), "blocks": [],
           "quiet_gate": {"waited_s": waited, "load": load}}
    srv = Server(a2, "wr", a.bin, a.port, f"w{variant}{r}")
    srv.port = a.port
    writer = None
    try:
        load_rel(srv.conn, "s")
        must(srv.conn, "CREATE TABLE z (id int64, v int64) BTREE", "CREATE")
        zq = flood_commits(srv.conn, FLOOD)  # every loaded row below the floor
        out["idle_wait"] = wait_idle(srv, ceiling_us_per_s=40000)
        scan_core = 1
        session_on_core(srv, scan_core)
        threads = reactor_threads(srv.proc.pid, a2.reactor_cpus)
        out["reactor_tids"] = threads
        writer = Writer(a, scan_core, variant)
        out["writer_core"] = writer.core
        cell = [c for c in make_cells(a) if c.name == "bn"][0]
        conn = srv.conn
        conn.prepare("m_wr", cell.sql)
        per = a.writer_block_ops
        q = 100
        # Warm-up, untimed, writer stopped.
        run_stmts(srv, conn, cell.sql, cell.params, "literal", range(q, q + 20), cell.expect,
                  "m_wr")
        q += 20
        m0 = meta_vals(conn, META_KEYS)
        m1 = meta_vals(conn, META_KEYS)
        out["meta_self_lookups"] = lookups_of(m1)[0] - lookups_of(m0)[0]
        for b in range(a.blocks):
            order = ("writer", "alone") if (b + r) % 2 == 0 else ("alone", "writer")
            for kind in order:
                if kind == "writer":
                    writer.go()
                    time.sleep(0.05)
                else:
                    # Enough commits of the scan session's own, on a relation
                    # nobody scans, for one reclaim pass: the visibility floor
                    # passes the writer's last commit, so no row the writer
                    # wrote is still "recent" when the writer-free block starts.
                    zq = flood_commits(conn, FLOOD, zq)
                    time.sleep(a.settle_s)
                conn = session_back(srv, conn, scan_core)
                core_before = meta_vals(conn, {"core"}).get("core")
                m0 = meta_vals(conn, META_KEYS)
                t0 = time.perf_counter()
                p0, s0 = {k: tid_ns(srv.proc.pid, t) for k, t in threads.items()}, \
                    sched_ns(srv.proc.pid)
                lat, bad = run_stmts(srv, conn, cell.sql, cell.params, "literal",
                                     range(q, q + per), cell.expect, "m_wr")
                q += per
                s1 = sched_ns(srv.proc.pid)
                p1 = {k: tid_ns(srv.proc.pid, t) for k, t in threads.items()}
                wall = time.perf_counter() - t0
                m1 = meta_vals(conn, META_KEYS)
                n_upd = bad_upd = 0
                if kind == "writer":
                    n_upd, bad_upd = writer.pause()
                tot0, per0 = lookups_of(m0)
                tot1, per1 = lookups_of(m1)
                blk = {"kind": kind, "ops": per, "bad": bad, "wall_s": round(wall, 4),
                       "lat_us": [round(x * 1e6, 1) for x in lat],
                       "server_total_us_per_stmt": (s1 - s0) / per / 1e3,
                       "reactor_us_per_stmt": {str(k): (p1[k] - p0[k]) / per / 1e3 for k in p0},
                       "lookups": tot1 - tot0,
                       "lookups_per_core": {k: per1[k] - per0.get(k, 0) for k in per1},
                       "window_wait_us": m1.get("contention_window_wait_us", 0) -
                       m0.get("contention_window_wait_us", 0),
                       "window_waits": m1.get("contention_window_waits", 0) -
                       m0.get("contention_window_waits", 0),
                       "updates": n_upd, "update_bad": bad_upd,
                       "writer_core": writer.core, "scan_core_before": core_before,
                       "scan_core_after": meta_vals(conn, {"core"}).get("core")}
                out["blocks"].append(blk)
                print(json.dumps({k: (round(v, 2) if isinstance(v, float) else v)
                                  for k, v in blk.items() if k != "lat_us"}), flush=True)
    finally:
        if writer is not None:
            writer.quit()
        srv.stop()
    out["host_after"] = host_state()
    return out


def writers(a):
    os.sched_setaffinity(0, {int(a.pin_driver)})
    a2 = argparse.Namespace(**vars(a))
    a2.cores, a2.durability = 2, "group"
    a.reactor_cpus = a2.reactor_cpus = a.reactor_cpus or "2,6"
    a.pin_server = a2.pin_server = a.pin_server if "," in a.pin_server else "2,3,6,7"
    result = {"args": vars(a), "runs": []}
    for r in range(a.runs):
        for variant in a.variant.split(","):
            result["runs"].append(writers_run(a, r, variant, a2))
            with open(a.out, "w") as f:
                json.dump(result, f, indent=1)


# -- the profile -------------------------------------------------------------------------

def profile(a):
    os.makedirs(a.sampler_dir, exist_ok=True)
    os.sched_setaffinity(0, {int(a.pin_driver)})
    base = os.path.join(a.sampler_dir, "dump")
    cells = {c.name: c for c in make_cells(a)}
    want = a.profile_cells.split(",") if a.profile_cells else []
    a2 = argparse.Namespace(**vars(a))
    a2.reactor_cpus = None  # the census cells run at cores = 1
    results = []
    if want:
        srv = Server(a2, "prof", a.bin, a.port, "prof", sampler_out=base)
        try:
            for name in sorted({cells[n].rel for n in want}):
                load_rel(srv.conn, name)
            must(srv.conn, "CREATE TABLE z (id int64, v int64) BTREE", "CREATE")
            flood_commits(srv.conn, FLOOD)
            wait_idle(srv)
            n_dump = 0
            for name in want:
                cell = cells[name]
                for kind in ("scan", "point"):
                    ops = a.profile_scans if kind == "scan" else a.point_ops
                    sql = cell.sql if kind == "scan" else f"SELECT * FROM {cell.rel} WHERE id = ?"
                    pf = cell.params if kind == "scan" else \
                        (lambda q, rel=cell.rel: [kwp.Connection.int64(TARGET_ID(q, RELS[rel][1]))])
                    ex = cell.expect if kind == "scan" else (lambda q: 1)
                    run_stmts(srv, srv.conn, sql, pf, "literal", range(1, 11), ex, "")
                    s0 = sched_ns(srv.proc.pid)
                    if kind == "scan":
                        os.kill(srv.proc.pid, signal.SIGUSR1)
                    t0 = time.perf_counter()
                    lat, bad = run_stmts(srv, srv.conn, sql, pf, "literal", range(100, 100 + ops),
                                         ex, "")
                    wall = time.perf_counter() - t0
                    s1 = sched_ns(srv.proc.pid)
                    rec = {"cell": name, "kind": kind, "rows": cell.rows, "ops": ops, "bad": bad,
                           "server_cpu_us": (s1 - s0) / ops / 1e3, "wall_us": wall / ops * 1e6}
                    if kind == "scan":
                        os.kill(srv.proc.pid, signal.SIGUSR2)
                        time.sleep(0.7)
                        rec["dump"] = f"{base}.{n_dump}"
                        n_dump += 1
                    results.append(rec)
                    print(json.dumps(rec), flush=True)
        finally:
            srv.stop()
    if a.writers:
        results.extend(profile_writers(a, base + "w"))
    with open(a.out, "w") as f:
        json.dump({"args": vars(a), "profile": results}, f, indent=1)


def profile_writers(a, base):
    a2 = argparse.Namespace(**vars(a))
    a2.cores, a2.durability = 2, "group"
    a.reactor_cpus = a2.reactor_cpus = a.reactor_cpus or "2,6"
    a2.pin_server = a.pin_server if "," in a.pin_server else "2,3,6,7"
    out = []
    for variant in a.variant.split(","):
        srv = Server(a2, "profw", a.bin, a.port, "profw" + variant, sampler_out=base + variant)
        srv.port = a.port
        writer = None
        try:
            load_rel(srv.conn, "s")
            must(srv.conn, "CREATE TABLE z (id int64, v int64) BTREE", "CREATE")
            zq = flood_commits(srv.conn, FLOOD)
            wait_idle(srv, ceiling_us_per_s=40000)
            session_on_core(srv, 1)
            threads = reactor_threads(srv.proc.pid, a2.reactor_cpus)
            writer = Writer(a, 1, variant)
            cell = [c for c in make_cells(a) if c.name == "bn"][0]
            n_dump = 0
            for kind in ("writer", "alone"):
                if kind == "writer":
                    writer.go()
                    time.sleep(0.2)
                else:
                    zq = flood_commits(srv.conn, FLOOD, zq)  # the floor passes the writer's last
                    time.sleep(0.3)
                srv.conn = session_back(srv, srv.conn, 1)
                m0 = meta_vals(srv.conn, META_KEYS)
                s0 = sched_ns(srv.proc.pid)
                p0 = {k: tid_ns(srv.proc.pid, t) for k, t in threads.items()}
                os.kill(srv.proc.pid, signal.SIGUSR1)
                t0 = time.perf_counter()
                lat, bad = run_stmts(srv, srv.conn, cell.sql, cell.params, "literal",
                                     range(100, 100 + a.profile_writer_scans), cell.expect, "")
                wall = time.perf_counter() - t0
                s1 = sched_ns(srv.proc.pid)
                p1 = {k: tid_ns(srv.proc.pid, t) for k, t in threads.items()}
                os.kill(srv.proc.pid, signal.SIGUSR2)
                time.sleep(0.7)
                m1 = meta_vals(srv.conn, META_KEYS)
                n = a.profile_writer_scans
                rec = {"cell": "writers-" + variant, "kind": kind, "ops": n, "bad": bad,
                       "server_total_us_per_stmt": (s1 - s0) / n / 1e3,
                       "reactor_us_per_stmt": {str(k): (p1[k] - p0[k]) / n / 1e3 for k in p0},
                       "wall_us": wall / n * 1e6,
                       "lookups": lookups_of(m1)[0] - lookups_of(m0)[0],
                       "window_wait_us": m1.get("contention_window_wait_us", 0) -
                       m0.get("contention_window_wait_us", 0),
                       "dump": f"{base}{variant}.{n_dump}"}
                n_dump += 1
                if kind == "writer":
                    rec["updates"], rec["update_bad"] = writer.pause()
                out.append(rec)
                print(json.dumps(rec), flush=True)
        finally:
            if writer is not None:
                writer.quit()
            srv.stop()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["census", "writers", "profile", "writer-child"])
    ap.add_argument("--bin")
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--port", type=int, default=32101)
    ap.add_argument("--pin-server", default="2")
    ap.add_argument("--pin-driver", default="4")
    ap.add_argument("--pin-writer", default="0")
    ap.add_argument("--reactor-cpus", default=None,
                    help="writers/profile --writers: one CPU per reactor, core 0 first (default 2,6)")
    ap.add_argument("--runs", type=int, default=8)
    ap.add_argument("--blocks", type=int, default=12)
    ap.add_argument("--scan-ops", type=int, default=192,
                    help="scans per path per run on a 10,000-row cell (1k: x4, 100k: /4)")
    ap.add_argument("--point-ops", type=int, default=3000)
    ap.add_argument("--cells", help="census: comma-separated cell names (default: all)")
    ap.add_argument("--variant", default="point", help="writers: point, range or point,range")
    ap.add_argument("--writer-block-ops", type=int, default=100)
    ap.add_argument("--settle-s", type=float, default=0.15)
    ap.add_argument("--quiet-load", type=float, default=0.0)
    ap.add_argument("--quiet-wait", type=float, default=600.0)
    ap.add_argument("--sampler")
    ap.add_argument("--sampler-dir")
    ap.add_argument("--profile-cells", default="")
    ap.add_argument("--profile-scans", type=int, default=20000)
    ap.add_argument("--profile-writer-scans", type=int, default=4000)
    ap.add_argument("--writers", action="store_true", help="profile: also the writers cell")
    ap.add_argument("--child-cpu", default="0")
    ap.add_argument("--avoid-core", type=int, default=1)
    ap.add_argument("--out")
    a = ap.parse_args()
    if a.mode == "census":
        census(a)
    elif a.mode == "writers":
        writers(a)
    elif a.mode == "profile":
        profile(a)
    else:
        writer_child(a)


if __name__ == "__main__":
    main()
