#!/usr/bin/env python3
"""DL close: the whole change, the commit DL opened from (A) against its closing code commit (B),
and DL-Q4's two words priced on B plus a spike (X). One server lifetime per arm per round, arms
alternated per round, fresh data file.

  dl_close.py --bins a=PATH,b=PATH,x=PATH --tools DIR --wd DIR --out OUT.jsonl --status FILE
              [--cells a,b] [--rounds N]

Cells (first arm against second):
  select1     cores=1, 1 client, point SELECT                              A | B     DA's cell
  select8     cores=8, 16 clients, point SELECT                            A | B     DA's cell
  fk1         cores=1, 1 client, INSERT into a child with a foreign key    A | B     DA's cell
  select8off  select8 with access_statistics = off                         A | B     DA's control
  cabin8      cores=8, 16 clients, a Cabin probe                           A | B
  join8i      cores=8, 16 clients, a two-relation join whose order is captured and served  A | B
  evict8      cores=8, 16 clients over 6,000 statement shapes              A | B
  storm8      B: select8 beside one session reading sys.statement_stats, sys.relation_stats,
              sys.meta in a loop | no reader                               M3, DL-Q9's bar
  stormA8     the same on A                                                the tail DL removes
  pool8x      X: select8, the buffer pool's hit count counted | skipped    DL-Q4
  join8ix     X: join8i, CapturedPlans::CountServed counted | skipped      DL-Q4
Every lifetime's contention block and drop counts (sys.meta) are read before and after the window.
"""
import argparse, json, os, random, subprocess, sys, time, multiprocessing as mp

ROWS = 10000
CROWS = 2000
OPS = ["=", "<", "<=", ">", ">=", "!="]
NSHAPES = 6000


def shape_sql(i, key):
    r = random.Random(i)
    cols = sorted(r.sample(range(1, 13), 4))
    conj = " AND ".join(f"a{c} {r.choice(OPS)} {r.randint(0, 3)}" for c in cols)
    return f"SELECT id FROM w WHERE id = {key} AND {conj}"


def status(path, line):
    with open(path + ".tmp", "w") as f:
        f.write(time.strftime("%FT%TZ ", time.gmtime()) + line + "\n")
    os.replace(path + ".tmp", path)


def cpu_ticks(pid):
    f = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
    return int(f[11]) + int(f[12])  # utime + stime


def must(c, sql, tag):
    _, _, got, err = c.execute(sql)
    if not got.startswith(tag):
        raise SystemExit(f"setup {sql!r} -> {got!r} {err}")


def meta(c):
    fields, rows, _, _ = c.execute("SELECT * FROM sys.meta")
    out = {}
    for row in rows:
        vals = [v.decode() if v is not None else "" for v in row]
        name = vals[1]
        if (vals[0] == "contention" or name.endswith("_dropped") or name.endswith("_busy")
                or name.startswith("captur") or name.startswith("cabin")
                or name.startswith("plan_stats_part")):
            out[name] = vals[2]
    return out


def start(a, arm, cores, port, tag):
    data = os.path.join(a.wd, f"{tag}.db")
    conf = os.path.join(a.wd, f"{tag}.conf")
    if os.path.exists(data):
        os.remove(data)
    subprocess.run(["rm", "-rf", data + ".wal"])
    with open(conf, "w") as f:
        f.write(f"data_file = {data}\nport = {port}\ncores = {cores}\ndurability = relaxed\n"
                f"buffer_pool_frames = 65536\nlog_file = {tag}.log\nlog_dir = {a.wd}\nlog_level = warn\n")
        for k, v in arm.get("conf", {}).items():
            f.write(f"{k} = {v}\n")
    err = open(os.path.join(a.wd, f"{tag}.stderr"), "w")
    cmd = [a.bins[arm["bin"]], "--config", conf]
    if cores == 1:
        cmd = ["taskset", "-c", "2"] + cmd
    env = dict(os.environ)
    env.update(arm.get("env", {}))
    p = subprocess.Popen(cmd, stdout=err, stderr=subprocess.STDOUT, env=env)
    dl = time.time() + 30
    while True:
        try:
            c = kwp.Connection("127.0.0.1", port, timeout=120)
            break
        except OSError:
            if time.time() > dl or p.poll() is not None:
                raise SystemExit("no listen")
            time.sleep(0.1)
    return p, c


def stop(p, c):
    try:
        c.close()
    except OSError:
        pass
    p.terminate()
    try:
        p.wait(60)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
    time.sleep(0.3)


def client(port, kind, tid, secs, q, barrier, base, cores):
    if cores == 1:
        os.sched_setaffinity(0, {0, 1, 3, 4, 5, 6, 7})
    c = kwp.Connection("127.0.0.1", port, timeout=120)
    rnd = random.Random(tid * 7919 + 1)
    lat = []
    n = 0
    bad = 0
    barrier.wait()
    end = time.time() + secs
    pc = time.perf_counter
    while time.time() < end:
        t0 = pc()
        if kind == "select":
            _, _, _, err = c.query(f"SELECT * FROM t WHERE id = {rnd.randint(1, ROWS)}")
        elif kind == "evict":
            _, _, _, err = c.query(shape_sql(rnd.randint(0, NSHAPES - 1), rnd.randint(1, ROWS)))
        elif kind == "cabin":
            _, _, _, err = c.query(f"SELECT * FROM o WHERE state = {rnd.randint(1, CROWS)}")
        elif kind == "joini":
            _, _, _, err = c.query(
                f"SELECT ja.v, jb.v FROM ja JOIN jb ON ja.id = jb.id WHERE ja.v = {rnd.randint(1, ROWS)}")
        else:  # fk
            k = base + tid * 10_000_000 + n + 1
            _, _, _, err = c.query(f"INSERT INTO ch VALUES ({k}, {rnd.randint(1, 1000)}, {k})")
        if err is not None:
            bad += 1
        lat.append(int((pc() - t0) * 1e6))
        n += 1
    q.put((n, bad, lat))
    c.close()


def reader(port, stop_at, q):
    c = kwp.Connection("127.0.0.1", port, timeout=120)
    n = 0
    while time.time() < stop_at:
        for view in ("sys.statement_stats", "sys.relation_stats", "sys.meta"):
            c.query(f"SELECT * FROM {view}")
            n += 1
    q.put(n)
    c.close()


def bulk(c, table, rows, rowfn, step=500):
    for lo in range(1, rows + 1, step):
        must(c, f"INSERT INTO {table} VALUES " + ",".join(
            rowfn(i) for i in range(lo, min(rows, lo + step - 1) + 1)), "INSERT")


def cell(a, name, kind, cores, label, arm, rnd, port):
    tag = f"{name}-{label}-r{rnd}"
    p, c = start(a, arm, cores, port, tag)
    try:
        note = ""
        if kind == "select":
            must(c, "CREATE TABLE t (id int64, v int64, w int64) BTREE", "CREATE")
            bulk(c, "t", ROWS, lambda i: f"({i},{i},{i})")
        elif kind == "evict":
            must(c, "CREATE TABLE w (id int64, " + ", ".join(f"a{k} int64" for k in range(1, 13)) +
                 ") BTREE", "CREATE")
            bulk(c, "w", ROWS, lambda i: "(" + ",".join([str(i)] + [str(i % 4)] * 12) + ")")
        elif kind == "cabin":
            must(c, "CREATE TABLE o (id int64, state int64, v int64) BTREE", "CREATE")
            bulk(c, "o", CROWS, lambda i: f"({i},{i},{i})")
            c.execute("CREATE CABIN ON o(state)")  # answers a result set: its warnings
            _, rows, _, _ = c.execute("ANALYZE SELECT * FROM o WHERE state = 7")
            note = " | ".join((r[0] or b"").decode() for r in rows)[:400]
        elif kind == "joini":
            for t in ("ja", "jb"):
                must(c, f"CREATE TABLE {t} (id int64, v int64) BTREE", "CREATE")
                bulk(c, t, ROWS, lambda i: f"({i},{i})")
            c.execute("CREATE INDEX ixv ON ja(v)")  # a searched shape: captured, then served
        else:  # fk
            must(c, "CREATE TABLE par (id int64, v int64) BTREE", "CREATE")
            must(c, "INSERT INTO par VALUES " + ",".join(f"({i},{i})" for i in range(1, 1001)), "INSERT")
            must(c, "CREATE TABLE ch (id int64, pid int64 REFERENCES par, v int64) BTREE", "CREATE")
        nclients = 1 if cores == 1 else a.clients8
        res = {}
        # A join's order is captured once the steward has published its relations' priors.
        for phase, secs in (("warm", a.warm + (4 if kind == "joini" else 0)), ("meas", a.secs)):
            q = mp.Queue()
            rq = mp.Queue()
            b = mp.Barrier(nclients + 1)
            base = 0 if phase == "warm" else 5_000_000_000
            ps = [mp.Process(target=client, args=(port, kind, t, secs, q, b, base, cores))
                  for t in range(nclients)]
            for x in ps:
                x.start()
            m0 = meta(c)
            b.wait()
            t0 = time.time()
            c0 = cpu_ticks(p.pid)
            rp = None
            if arm.get("reader") and phase == "meas":
                rp = mp.Process(target=reader, args=(port, t0 + secs, rq))
                rp.start()
            outs = [q.get() for _ in ps]
            t1 = time.time()
            c1 = cpu_ticks(p.pid)
            m1 = meta(c)
            for x in ps:
                x.join()
            reads = 0
            if rp is not None:
                reads = rq.get()
                rp.join()
            if phase == "meas":
                n = sum(o[0] for o in outs)
                lat = sorted(l for o in outs for l in o[2])
                cpu_s = (c1 - c0) / os.sysconf("SC_CLK_TCK")

                def pct(f):
                    return lat[min(len(lat) - 1, int(f * len(lat)))]
                delta = {}
                for k, v in m1.items():
                    try:
                        delta[k] = int(v) - int(m0.get(k, "0"))
                    except ValueError:
                        pass
                others = subprocess.run(
                    "ps -eo args | grep -E '^[^ ]*kds_' | grep -v 'dl-close/kds_' | grep -v build-sandbox | wc -l",
                    shell=True, capture_output=True, text=True).stdout.strip()
                res = dict(cell=name, cores=cores, arm=label, bin=arm["bin"], round=rnd,
                           clients=nclients, stmts=n,
                           bad=sum(o[1] for o in outs), win_s=t1 - t0, qps=n / (t1 - t0),
                           cpu_s=cpu_s, cpu_us_per_stmt=cpu_s * 1e6 / n,
                           p0=lat[0], p25=pct(.25), p50=pct(.5), p90=pct(.9), p99=pct(.99),
                           p999=pct(.999), pmax=lat[-1], reads=reads, note=note,
                           load=open("/proc/loadavg").read().split()[0],
                           busy=subprocess.run(["pgrep", "-c", "-f", "cc1plus|cmake --build|ctest"],
                                               capture_output=True, text=True).stdout.strip(),
                           other_servers=others,
                           meta={k: v for k, v in delta.items() if v != 0})
        return res
    finally:
        stop(p, c)


A = {"bin": "a"}
B = {"bin": "b"}
X = {"bin": "x"}
OFF = {"access_statistics": "off"}
CELLS = {
    # name: (kind, cores, [(label, arm), (label, arm)], rounds)
    "select1": ("select", 1, [("A", A), ("B", B)], 6),
    "select8": ("select", 8, [("A", A), ("B", B)], 6),
    "fk1": ("fk", 1, [("A", A), ("B", B)], 4),
    "select8off": ("select", 8, [("A", dict(A, conf=OFF)), ("B", dict(B, conf=OFF))], 4),
    "cabin8": ("cabin", 8, [("A", A), ("B", B)], 4),
    "join8i": ("joini", 8, [("A", A), ("B", B)], 4),
    "evict8": ("evict", 8, [("A", A), ("B", B)], 3),
    "storm8": ("select", 8, [("reader", dict(B, reader=True)), ("quiet", B)], 4),
    "stormA8": ("select", 8, [("reader", dict(A, reader=True)), ("quiet", A)], 3),
    "pool8x": ("select", 8, [("counted", X),
                             ("skipped", dict(X, env={"KDS_DL_NO_POOL_COUNT": "1"}))], 4),
    "join8ix": ("joini", 8, [("counted", X),
                             ("skipped", dict(X, env={"KDS_DL_NO_PLAN_COUNT": "1"}))], 4),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bins", required=True, help="a=PATH,b=PATH,x=PATH")
    ap.add_argument("--tools", required=True)
    ap.add_argument("--wd", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--status", required=True)
    ap.add_argument("--cells", default=",".join(CELLS))
    ap.add_argument("--rounds", type=int, default=0, help="override every cell's rounds")
    ap.add_argument("--first-round", type=int, default=1, help="resume: the first round to run")
    ap.add_argument("--secs", type=float, default=8)
    ap.add_argument("--warm", type=float, default=2)
    ap.add_argument("--clients8", type=int, default=16)
    ap.add_argument("--port", type=int, default=28300)
    a = ap.parse_args()
    a.bins = dict(kv.split("=", 1) for kv in a.bins.split(","))
    sys.path.insert(0, a.tools)
    global kwp
    import kwp
    os.makedirs(a.wd, exist_ok=True)
    port = a.port
    for cn in a.cells.split(","):
        kind, cores, arms, rounds = CELLS[cn]
        rounds = a.rounds or rounds
        for rnd in range(a.first_round, rounds + 1):
            order = arms if rnd % 2 else list(reversed(arms))
            for label, arm in order:
                status(a.status, f"{cn} round {rnd}/{rounds} {label}")
                port += 1
                r = cell(a, cn, kind, cores, label, arm, rnd, port)
                with open(a.out, "a") as f:
                    f.write(json.dumps(r) + "\n")
    status(a.status, "DONE")


if __name__ == "__main__":
    main()
