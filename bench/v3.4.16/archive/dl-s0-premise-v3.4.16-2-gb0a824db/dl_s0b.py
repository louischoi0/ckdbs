#!/usr/bin/env python3
"""DL-S0 premise: what a statistic costs the statements it describes.
One server lifetime per arm per round, arms alternated per round, fresh data file.

  dl_s0.py --bin COPY --tools DIR --wd DIR --out OUT.jsonl --status FILE [--cells a,b] [--rounds N]

Cells (each: arm A against arm B, the same binary):
  select1  cores=1, 1 client, point SELECT        collector set | unset (KDS_DL_NO_COLLECTOR)   M2, M1
  select8  cores=8, 16 clients, point SELECT      collector set | unset                         M2, M1
  insert1  cores=1, 1 client, keyed INSERT        collector set only                            M1
  insert8  cores=8, 16 clients, keyed INSERT      collector set only                            M1
  storm8   select8 beside one session reading sys.statement_stats, sys.relation_stats, sys.meta
           in a loop | no reader                                                                M3
  tick8    select8 with cabin_optimizer on, a 100 ms tick | off                                 M3
  evict8   cores=8, 16 clients over 6,000 statement shapes: collector set | unset               M2 (P1's scan)
  pool8    select8: the buffer pool's hit count (one word every core adds to at every page
           fetch) counted | skipped (KDS_DL_NO_POOL_COUNT)                                     DL-Q4
  join8    cores=8, 16 clients, a two-relation join under join_order = fixed:
           CapturedPlans::CountServed counted | skipped (KDS_DL_NO_PLAN_COUNT)                 DL-Q4
  cabin8   cores=8, 16 clients, a Cabin probe: counters as built | skipped (KDS_DL_NO_CABIN_COUNT) M4
Every lifetime's contention block (sys.meta) is read before and after the measured window.
"""
import argparse, json, os, random, subprocess, sys, time, multiprocessing as mp

ROWS = 10000
CROWS = 2000
OPS = ["=", "<", "<=", ">", ">=", "!="]
NSHAPES = 6000


def shape_sql(i, key):
    # Shape i of NSHAPES: four of twelve columns, an operator each - one fingerprint a shape.
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
        if (vals[0] == "contention" or vals[1].endswith("_dropped")
                or vals[1].startswith("captur") or vals[1].startswith("cabin")):
            out[vals[1]] = vals[2]
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
    cmd = [a.bin, "--config", conf]
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
        elif kind == "join":
            _, _, _, err = c.query(
                f"SELECT ja.v, jb.v FROM ja JOIN jb ON ja.id = jb.id WHERE ja.id = {rnd.randint(1, ROWS)}")
        else:
            k = base + tid * 10_000_000 + n + 1
            t, _, _, err = c.query(f"INSERT INTO ins VALUES ({k}, {k}, {k})")
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
            note = " | ".join((r[0] or b"").decode() for r in rows)[:600]
        elif kind in ("join", "joini"):
            for t in ("ja", "jb"):
                must(c, f"CREATE TABLE {t} (id int64, v int64) BTREE", "CREATE")
                bulk(c, t, ROWS, lambda i: f"({i},{i})")
            if kind == "joini":
                c.execute("CREATE INDEX ixv ON ja(v)")  # makes the point join a searched shape: captured, then served
        else:
            must(c, "CREATE TABLE ins (id int64, v int64, w int64) BTREE", "CREATE")
        nclients = 1 if cores == 1 else a.clients8
        res = {}
        # A join's order is captured once the steward has published its relations' priors.
        for phase, secs in (("warm", a.warm + (3 if kind in ("join", "joini") else 0)), ("meas", a.secs)):
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
                res = dict(cell=name, cores=cores, arm=label, round=rnd, clients=nclients, stmts=n,
                           bad=sum(o[1] for o in outs), win_s=t1 - t0, qps=n / (t1 - t0),
                           cpu_s=cpu_s, cpu_us_per_stmt=cpu_s * 1e6 / n,
                           p0=lat[0], p25=pct(.25), p50=pct(.5), p90=pct(.9), p99=pct(.99),
                           p999=pct(.999), pmax=lat[-1], reads=reads, note=note,
                           load=open("/proc/loadavg").read().split()[0],
                           busy=subprocess.run(["pgrep", "-c", "-f", "cc1plus|cmake --build|ctest"],
                                               capture_output=True, text=True).stdout.strip(),
                           meta={k: v for k, v in delta.items() if v != 0})
        return res
    finally:
        stop(p, c)


SET = {}
UNSET = {"env": {"KDS_DL_NO_COLLECTOR": "1"}}
CELLS = {
    # name: (kind, cores, [(label, arm), (label, arm)], rounds)
    "select1": ("select", 1, [("set", SET), ("unset", UNSET)], 4),
    "select8": ("select", 8, [("set", SET), ("unset", UNSET)], 4),
    "insert1": ("insert", 1, [("set", SET)], 2),
    "insert8": ("insert", 8, [("set", SET)], 2),
    "storm8": ("select", 8, [("reader", {"reader": True}), ("quiet", SET)], 3),
    "tick8": ("select", 8, [("on", {"conf": {"cabin_optimizer": "on",
                                              "cabin_optimizer_snapshot_interval_ms": "100"}}),
                            ("off", SET)], 3),
    "evict8": ("evict", 8, [("set", SET), ("unset", UNSET)], 3),
    "pool8": ("select", 8, [("counted", SET), ("skipped", {"env": {"KDS_DL_NO_POOL_COUNT": "1"}})], 3),
    "join8": ("join", 8, [("counted", SET), ("skipped", {"env": {"KDS_DL_NO_PLAN_COUNT": "1"}})], 3),
    "join8i": ("joini", 8, [("counted", SET), ("skipped", {"env": {"KDS_DL_NO_PLAN_COUNT": "1"}})], 3),
    "cabin8": ("cabin", 8, [("counted", SET), ("skipped", {"env": {"KDS_DL_NO_CABIN_COUNT": "1"}})], 4),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--tools", required=True)
    ap.add_argument("--wd", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--status", required=True)
    ap.add_argument("--cells", default=",".join(CELLS))
    ap.add_argument("--rounds", type=int, default=0, help="override every cell's rounds")
    ap.add_argument("--secs", type=float, default=8)
    ap.add_argument("--warm", type=float, default=2)
    ap.add_argument("--clients8", type=int, default=16)
    ap.add_argument("--port", type=int, default=27300)
    a = ap.parse_args()
    sys.path.insert(0, a.tools)
    global kwp
    import kwp
    os.makedirs(a.wd, exist_ok=True)
    port = a.port
    for cn in a.cells.split(","):
        kind, cores, arms, rounds = CELLS[cn]
        rounds = a.rounds or rounds
        for rnd in range(1, rounds + 1):
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
