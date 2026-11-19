#!/usr/bin/env python3
"""DA-S0 premise: access_statistics on vs off, interleaved, one server lifetime per arm per round.
  da_s0.py --bin COPY --wd DIR --out OUT.jsonl --status FILE --rounds N --cells select1,select8,fk1
select1: 1 serial client, server pinned to CPU 2.  select8: cores=8, --clients8 closed-loop
client processes.  fk1: cores=1 INSERT into a child relation with a foreign key, 1 client.
"""
import argparse, json, os, random, subprocess, sys, time, multiprocessing as mp
sys.path.insert(0, "/home/louis/ckdbs/.claude/worktrees/da-access-stats-memory-resident/tools")
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


def start(a, label, cores, port, tag):
    data = os.path.join(a.wd, f"{tag}.db")
    conf = os.path.join(a.wd, f"{tag}.conf")
    if os.path.exists(data):
        os.remove(data)
    subprocess.run(["rm", "-rf", data + ".wal"])
    with open(conf, "w") as f:
        f.write(f"data_file = {data}\nport = {port}\ncores = {cores}\ndurability = relaxed\n"
                f"buffer_pool_frames = 65536\nlog_file = {tag}.log\nlog_dir = {a.wd}\nlog_level = warn\n"
                f"access_statistics = {label}\n")
    err = open(os.path.join(a.wd, f"{tag}.stderr"), "w")
    cmd = [a.bin, "--config", conf]
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
        else:
            k = base + tid * 10_000_000 + n + 1
            t, _, _, _ = c.query(f"INSERT INTO ch VALUES ({k}, {rnd.randint(1, 1000)}, {k})")
            if not t.startswith("INSERT"):
                bad += 1
        lat.append(int((pc() - t0) * 1e6))
        n += 1
    q.put((n, bad, lat))
    c.close()


def cell(a, kind, cores, label, rnd, port):
    tag = f"{kind}{cores}-{label}-r{rnd}"
    p, c = start(a, label, cores, port, tag)
    try:
        if kind == "select":
            must(c, "CREATE TABLE t (id int64, v int64, w int64) BTREE", "CREATE")
            for lo in range(1, ROWS + 1, 500):
                must(c, "INSERT INTO t VALUES " + ",".join(
                    f"({i},{i},{i})" for i in range(lo, min(ROWS, lo + 499) + 1)), "INSERT")
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
            base = 0 if phase == "warm" else 5_000_000_000
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
                res = dict(cell=kind, cores=cores, arm=label, round=rnd, clients=nclients, stmts=n,
                           bad=sum(o[1] for o in outs), win_s=t1 - t0, qps=n / (t1 - t0),
                           cpu_s=cpu_s, cpu_us_per_stmt=cpu_s * 1e6 / n,
                           p0=lat[0], p25=pct(.25), p50=pct(.5), p90=pct(.9), p99=pct(.99),
                           load=open("/proc/loadavg").read().split()[0])
        return res
    finally:
        stop(p, c)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--wd", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--status", required=True)
    ap.add_argument("--rounds", type=int, default=6)
    ap.add_argument("--cells", default="select1,select8,fk1")
    ap.add_argument("--secs", type=float, default=8)
    ap.add_argument("--warm", type=float, default=2)
    ap.add_argument("--clients8", type=int, default=16)
    a = ap.parse_args()
    os.makedirs(a.wd, exist_ok=True)
    port = 27100
    for rnd in range(1, a.rounds + 1):
        for cn in a.cells.split(","):
            kind, cores = ("select", int(cn[6:])) if cn.startswith("select") else ("fk", 1)
            order = ["on", "off"] if rnd % 2 else ["off", "on"]
            for label in order:
                status(a.status, f"round {rnd}/{a.rounds} {cn} {label}")
                port += 1
                r = cell(a, kind, cores, label, rnd, port)
                with open(a.out, "a") as f:
                    f.write(json.dumps(r) + "\n")
    status(a.status, "DONE")


if __name__ == "__main__":
    main()
