#!/usr/bin/env python3
"""CS-S0's premise cells: what `BEGIN; <k> tuple key SELECTs; COMMIT` costs a
client over the same k SELECTs in autocommit.

`instructions/v3.2.0/workorder-cs-read-only-transaction.md` section 5 (CS-S0)
and CS-Q1. One binary (a copy, hashed), one server per (durability, cores)
configuration with a fresh data file, one preloaded BTREE relation
`cs_t (id int64, a int64, b int64)` of --rows rows, and per cell three arms:

  W   BEGIN (text), k x `SELECT * FROM cs_t WHERE id = <random>`, COMMIT
  A   the same k SELECTs, each in autocommit
  A2  a second A arm: the noise control. |A - A2| and A's repeat-to-repeat
      spread are the floor under any W - A delta.

The unit is one group (one W transaction, or k autocommit reads). Each cell
is --seconds long per repeat, --reps repeats, the arm order rotated per
repeat; each session is its own process (a GIL would make the client the
bottleneck). Per cell it records the group latency percentiles, groups/s,
and `wal_append_lsn`'s delta (the stream's byte position, `SHOW META`) per
group. Transaction ids spent are not exposed; `--id-probe` measures them
with sys.lock_waits' holder_trx (see probe_id).

Usage: cs_s0_premise.py --bin B --workdir D --out OUT.json [--durability
strict,group,relaxed] [--cores 1,4] [--conns 1,8] [--k 1,4,16] [--reps 2]
[--seconds 2.5] [--heartbeat F] [--status F]
"""
import argparse, json, multiprocessing as mp, os, random, shutil, subprocess, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
import sys_meta  # noqa: E402
from bench_common import nearest_rank  # noqa: E402

TABLE = "cs_t"


def host_state():
    try:
        load = open("/proc/loadavg").read().strip()
    except OSError:
        load = "unavailable"
    try:
        busy = subprocess.run(["pgrep", "-c", "cc1plus"], capture_output=True, text=True,
                              timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        busy = "?"
    return {"loadavg": load, "cc1plus": busy}


def meta(conn):
    _f, rows, tag, _n = conn.execute("SHOW META")
    return sys_meta.from_rows(rows, tag)


def pcts(xs):
    s = sorted(xs)
    if not s:
        return None
    us = lambda v: round(v * 1e6, 1)
    return {"n": len(s), "p0_us": us(s[0]), "p25_us": us(nearest_rank(s, 25)),
            "p50_us": us(nearest_rank(s, 50)), "p75_us": us(nearest_rank(s, 75)),
            "p99_us": us(nearest_rank(s, 99)), "max_us": us(s[-1])}


def worker(index, a, port, arm, k, t0, t1, cpus, pipe):
    out = {"lat": [], "err": None}
    try:
        if cpus:
            os.sched_setaffinity(0, cpus)
        rng = random.Random(1000 * index + k)
        conn = kwp.Connection("127.0.0.1", port, timeout=30.0)
        n = a.rows
        while time.time() < t0:
            time.sleep(0.001)
        lat = out["lat"]
        while time.time() < t1:
            keys = [rng.randint(1, n) for _ in range(k)]
            s = time.perf_counter()
            if arm == "W":
                conn.execute("BEGIN")
            for key in keys:
                _f, rows, _t, _c = conn.execute(f"SELECT * FROM {TABLE} WHERE id = {key}")
                if len(rows) != 1:
                    raise RuntimeError(f"id {key} returned {len(rows)} rows")
            if arm == "W":
                conn.execute("COMMIT")
            lat.append(time.perf_counter() - s)
        conn.close()
    except Exception as e:  # noqa: BLE001 - the cell's finding
        out["err"] = repr(e)
    pipe.send(out)
    pipe.close()


def run_cell(a, port, admin, arm, k, conns, client_cpus):
    m0 = meta(admin)
    t0 = time.time() + 0.5
    t1 = t0 + a.seconds
    procs, pipes = [], []
    for i in range(conns):
        parent, child = mp.Pipe(duplex=False)
        p = mp.Process(target=worker, args=(i, a, port, arm, k, t0, t1, client_cpus, child))
        p.start()
        child.close()
        procs.append(p)
        pipes.append(parent)
    res = [pp.recv() for pp in pipes]
    for p in procs:
        p.join()
    m1 = meta(admin)
    lat = [x for r in res for x in r["lat"]]
    groups = len(lat)
    wal = int(m1["wal_append_lsn"]) - int(m0["wal_append_lsn"])
    return {"arm": arm, "k": k, "conns": conns, "groups": groups,
            "gps": round(groups / a.seconds, 1), "wal_bytes_per_group": round(wal / max(1, groups), 2),
            "lat": pcts(lat), "err": [r["err"] for r in res if r["err"]],
            "syncs": int(m1.get("wal_syncs", 0)) - int(m0.get("wal_syncs", 0))}


def probe_id(port):
    """The id of an explicit transaction, from sys.lock_waits.holder_trx: one
    session opens BEGIN and updates row 1, a second sends an autocommit UPDATE
    of the same row that must park on it, the first commits 150 ms later.
    Returns the holder's id, or None where no wait row appeared."""
    h = kwp.Connection("127.0.0.1", port, timeout=30.0)
    w = kwp.Connection("127.0.0.1", port, timeout=30.0)
    adm = kwp.Connection("127.0.0.1", port, timeout=30.0)
    h.execute("BEGIN")
    h.execute(f"UPDATE {TABLE} SET a = a WHERE id = 1")
    th = threading.Thread(target=lambda: w.execute(f"UPDATE {TABLE} SET a = a WHERE id = 1"))
    th.start()
    time.sleep(0.15)
    h.execute("COMMIT")
    th.join()
    fields, rows, _t, _n = adm.execute("SELECT * FROM sys.lock_waits")
    names = [f["name"] for f in fields]
    rend = [kwp.renderer_for(f) for f in fields]
    best = None
    for row in rows:
        rec = {n: (None if v is None else r(v)) for n, r, v in zip(names, rend, row)}
        if rec.get("holder_trx") is not None:
            seq = int(rec["seq"])
            if best is None or seq > best[0]:
                best = (seq, int(rec["holder_trx"]))
    for c in (h, w, adm):
        c.close()
    return None if best is None else best[1]


def start_server(a, binary, port, dur, cores, tag, pin):
    data = os.path.join(a.workdir, f"cs-{tag}.db")
    shutil.rmtree(data + ".wal", ignore_errors=True)
    if os.path.exists(data):
        os.remove(data)
    conf = os.path.join(a.workdir, f"cs-{tag}.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {data}\nport = {port}\ncores = {cores}\ndurability = {dur}\n"
                f"buffer_pool_frames = 65536\nlog_file = cs-{tag}.log\nlog_dir = {a.workdir}\n"
                f"log_level = warn\n")
    err = open(os.path.join(a.workdir, f"cs-{tag}.stderr"), "w")
    proc = subprocess.Popen(["taskset", "-c", pin, binary, "--config", conf], stdout=err,
                            stderr=subprocess.STDOUT)
    deadline = time.time() + 30
    while True:
        try:
            conn = kwp.Connection("127.0.0.1", port, timeout=120)
            return proc, conn, err
        except OSError:
            if time.time() > deadline or proc.poll() is not None:
                raise SystemExit(f"server {tag} did not listen on {port}")
            time.sleep(0.1)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--bin", required=True)
    p.add_argument("--workdir", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--port", type=int, default=15611)
    p.add_argument("--durability", default="strict,group,relaxed")
    p.add_argument("--cores", default="1,4")
    p.add_argument("--conns", default="1,8")
    p.add_argument("--k", default="1,4,16")
    p.add_argument("--rows", type=int, default=10000)
    p.add_argument("--reps", type=int, default=2)
    p.add_argument("--seconds", type=float, default=2.5)
    p.add_argument("--id-probe", action="store_true")
    p.add_argument("--heartbeat")
    p.add_argument("--status")
    a = p.parse_args()
    os.makedirs(a.workdir, exist_ok=True)

    def beat(line):
        if a.heartbeat:
            open(a.heartbeat, "w").write(str(time.time()))
        if a.status:
            open(a.status, "a").write(time.strftime("%H:%M:%S ", time.gmtime()) + line + "\n")
        print(line, flush=True)

    ks = [int(x) for x in a.k.split(",")]
    cells, probes = [], []
    for cores in [int(x) for x in a.cores.split(",")]:
        for dur in a.durability.split(","):
            tag = f"{dur}-c{cores}"
            pin = "4" if cores == 1 else "4-7"
            client_cpus = {0, 1, 2, 3}
            proc, admin, err = start_server(a, a.bin, a.port, dur, cores, tag, pin)
            try:
                admin.execute(f"CREATE TABLE {TABLE} (id int64, a int64, b int64) BTREE")
                for lo in range(1, a.rows + 1, 500):
                    hi = min(a.rows, lo + 499)
                    admin.execute(f"INSERT INTO {TABLE} VALUES " +
                                  ",".join(f"({i},{i},{i})" for i in range(lo, hi + 1)))
                _f, rows, _t, _n = admin.execute(f"SELECT * FROM {TABLE} WHERE id = 77")
                assert len(rows) == 1
                # warm up: the pool, the plan memo
                run_cell(argparse.Namespace(rows=a.rows, seconds=0.5), a.port, admin, "A", 4, 1, client_cpus)
                for conns in [int(x) for x in a.conns.split(",")]:
                    for k in ks:
                        for rep in range(a.reps):
                            order = ["W", "A", "A2"]
                            sh = rep % 3
                            order = order[sh:] + order[:sh]
                            if rep % 2:
                                order = order[::-1]
                            for arm in order:
                                beat(f"{tag} conns {conns} k {k} rep {rep} {arm}")
                                c = run_cell(a, a.port, admin, "A" if arm == "A2" else arm, k, conns,
                                             client_cpus)
                                c.update(arm=arm, rep=rep, dur=dur, cores=cores, host=host_state())
                                cells.append(c)
                                json.dump({"args": vars(a), "cells": cells, "probes": probes},
                                          open(a.out, "w"), indent=1)
                if a.id_probe:
                    # ids: probe, N groups of an arm on one connection, probe.
                    for arm, k in (("probe-only", 0), ("A", 16), ("W", 1), ("W", 16)):
                        i0 = probe_id(a.port)
                        groups = 0
                        if arm != "probe-only":
                            c = run_cell(argparse.Namespace(rows=a.rows, seconds=1.0), a.port, admin,
                                         arm, k, 1, client_cpus)
                            groups = c["groups"]
                        i1 = probe_id(a.port)
                        probes.append({"dur": dur, "cores": cores, "arm": arm, "k": k, "groups": groups,
                                       "id_before": i0, "id_after": i1})
                        beat(f"probe {tag} {arm} k{k}: {i0} -> {i1} over {groups} groups")
                    json.dump({"args": vars(a), "cells": cells, "probes": probes}, open(a.out, "w"), indent=1)
            finally:
                admin.close()
                proc.terminate()
                try:
                    proc.wait(30)
                except subprocess.TimeoutExpired:
                    proc.kill()
                err.close()
    beat("driver finished")


if __name__ == "__main__":
    main()
