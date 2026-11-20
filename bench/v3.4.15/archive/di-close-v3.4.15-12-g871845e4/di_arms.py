#!/usr/bin/env python3
"""DI arms: any number of binaries, interleaved, one server lifetime per arm per round.
DA's close driver (bench/v3.4.14/archive/da-close-v3.4.14-32-g89c26252/da_close.py) with
the two arms made a list and a keyed INSERT cell added.
  di_arms.py --bins A=/path/kds_A,B=/path/kds_B --tools DIR --wd DIR --out OUT.jsonl \
             --status FILE --rounds N --cells select1,select8,insert1,insert8,fk1
selectN: SELECT * FROM t WHERE id = <random> over 10,000 rows.  insertN: INSERT INTO t
VALUES (<key>, ...) with the key named, into an empty relation.  fk1: cores=1 INSERT into a
child relation with a foreign key.  cores=1: 1 serial client, server pinned to CPU 2.
cores=8: --clients8 closed-loop client processes, unpinned.  The arm order rotates by one
each round.
"""
import argparse, json, os, random, subprocess, sys, time, multiprocessing as mp
for _i, _v in enumerate(sys.argv):
    if _v == "--tools":
        sys.path.insert(0, sys.argv[_i + 1])
import kwp

ROWS = 10000


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


def start(a, label, cores, port, tag, binary, stats):
    data = os.path.join(a.wd, f"{tag}.db")
    conf = os.path.join(a.wd, f"{tag}.conf")
    if os.path.exists(data):
        os.remove(data)
    subprocess.run(["rm", "-rf", data + ".wal"])
    with open(conf, "w") as f:
        f.write(f"data_file = {data}\nport = {port}\ncores = {cores}\ndurability = relaxed\n"
                f"buffer_pool_frames = 65536\nlog_file = {tag}.log\nlog_dir = {a.wd}\nlog_level = warn\n"
                f"access_statistics = {stats}\n")
    err = open(os.path.join(a.wd, f"{tag}.stderr"), "w")
    cmd = [binary, "--config", conf]
    if cores == 1:
        cmd = ["taskset", "-c", "2"] + cmd
    p = subprocess.Popen(cmd, stdout=err, stderr=subprocess.STDOUT)
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
            c.query(f"SELECT * FROM t WHERE id = {rnd.randint(1, ROWS)}")
        elif kind == "insert":
            k = base + tid * 10_000_000 + n + 1
            t, _, _, _ = c.query(f"INSERT INTO t VALUES ({k}, {k}, {k})")
            if not t.startswith("INSERT"):
                bad += 1
        else:
            k = base + tid * 10_000_000 + n + 1
            t, _, _, _ = c.query(f"INSERT INTO ch VALUES ({k}, {rnd.randint(1, 1000)}, {k})")
            if not t.startswith("INSERT"):
                bad += 1
        lat.append(int((pc() - t0) * 1e6))
        n += 1
    q.put((n, bad, lat))
    c.close()


def cell(a, kind, cores, label, rnd, port, binary, stats, cn):
    tag = f"{cn}-{label}-r{rnd}"
    p, c = start(a, label, cores, port, tag, binary, stats)
    try:
        if kind == "select":
            must(c, "CREATE TABLE t (id int64, v int64, w int64) BTREE", "CREATE")
            for lo in range(1, ROWS + 1, 500):
                must(c, "INSERT INTO t VALUES " + ",".join(
                    f"({i},{i},{i})" for i in range(lo, min(ROWS, lo + 499) + 1)), "INSERT")
            nclients = 1 if cores == 1 else a.clients8
        elif kind == "insert":
            must(c, "CREATE TABLE t (id int64, v int64, w int64) BTREE", "CREATE")
            nclients = 1 if cores == 1 else a.clients8
        else:
            must(c, "CREATE TABLE par (id int64, v int64) BTREE", "CREATE")
            must(c, "INSERT INTO par VALUES " + ",".join(f"({i},{i})" for i in range(1, 1001)), "INSERT")
            must(c, "CREATE TABLE ch (id int64, pid int64 REFERENCES par, v int64) BTREE", "CREATE")
            nclients = 1
        res = {}
        for phase, secs in (("warm", a.warm), ("meas", a.secs)):
            q = mp.Queue()
            b = mp.Barrier(nclients + 1)
            base = 1_000_000_000 if phase == "warm" else 5_000_000_000
            ps = [mp.Process(target=client, args=(port, kind, t, secs, q, b, base, cores))
                  for t in range(nclients)]
            for x in ps:
                x.start()
            b.wait()
            t0 = time.time()
            c0 = cpu_ticks(p.pid)
            outs = [q.get() for _ in ps]
            t1 = time.time()
            c1 = cpu_ticks(p.pid)
            for x in ps:
                x.join()
            if phase == "meas":
                n = sum(o[0] for o in outs)
                lat = sorted(l for o in outs for l in o[2])
                cpu_s = (c1 - c0) / os.sysconf("SC_CLK_TCK")

                def pct(f):
                    return lat[min(len(lat) - 1, int(f * len(lat)))]
                res = dict(cn=cn, cell=kind, cores=cores, arm=label, round=rnd, clients=nclients, stmts=n,
                           bad=sum(o[1] for o in outs), win_s=t1 - t0, qps=n / (t1 - t0),
                           cpu_s=cpu_s, cpu_us_per_stmt=cpu_s * 1e6 / n,
                           p0=lat[0], p25=pct(.25), p50=pct(.5), p90=pct(.9), p99=pct(.99),
                           load=open("/proc/loadavg").read().split()[0])
        return res
    finally:
        stop(p, c)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bins", required=True)
    ap.add_argument("--tools", required=True)
    ap.add_argument("--wd", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--status", required=True)
    ap.add_argument("--rounds", type=int, default=6)
    ap.add_argument("--cells", default="select1,select8,fk1,select8off")
    ap.add_argument("--secs", type=float, default=8)
    ap.add_argument("--warm", type=float, default=2)
    ap.add_argument("--clients8", type=int, default=16)
    ap.add_argument("--start-round", type=int, default=1, help="DI close: resume after an orphaned run")
    a = ap.parse_args()
    os.makedirs(a.wd, exist_ok=True)
    bins = [x.split("=", 1) for x in a.bins.split(",")]
    port = 27300
    for rnd in range(a.start_round, a.rounds + 1):
        for cn in a.cells.split(","):
            stats = "off" if cn.endswith("off") else "on"
            base = cn.replace("off", "")
            kind, cores = (("select", int(base[6:])) if base.startswith("select") else
                           ("insert", int(base[6:])) if base.startswith("insert") else ("fk", 1))
            k = (rnd - 1) % len(bins)
            for label, binary in bins[k:] + bins[:k]:
                status(a.status, f"round {rnd}/{a.rounds} {cn} {label}")
                port += 1
                r = cell(a, kind, cores, label, rnd, port, binary, stats, cn)
                with open(a.out, "a") as f:
                    f.write(json.dumps(r) + "\n")
    status(a.status, "DONE")


if __name__ == "__main__":
    main()
