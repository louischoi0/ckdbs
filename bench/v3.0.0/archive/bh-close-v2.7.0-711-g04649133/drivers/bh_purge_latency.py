#!/usr/bin/env python3
"""BH close, part 2b: the latency from a DELETE's commit to a PURGE that succeeds.
Scratch driver (bench/README.md rule 5 is broken: tools/ has no PURGE driver).

Mode `readers` (default): a relation t(id,v,w) BTREE of --keys rows; --readers
processes run autocommit `SELECT * FROM t WHERE id = <random 1..N>` back to back and
--writers processes run autocommit `UPDATE t SET v = .. WHERE id = <random in the upper
half>`. The main connection then, for k = 1..--samples (ascending), times
    DELETE FROM t WHERE id = k          (reply = the commit, relaxed)
    PURGE  FROM t WHERE id = k          (retried on TXN_CONFLICT until PURGED 1)
and records, per key, total = (successful PURGE's reply) - (DELETE's reply), the first
attempt's own latency, and the number of TXN_CONFLICT refusals (each costs the 1 s bound).

Mode `snapshot`: no background load. For each hold H (ms) a second connection BEGINs and
reads once (opening its snapshot); the main connection DELETEs key k; a timer COMMITs the
second connection H ms after the DELETE's reply; the main connection runs PURGE (retried).
Shows how long a PURGE waits for a snapshot older than its DELETE, and what the 1 s
bound does to a longer one.

Verification: every DELETE must reply `DELETED 1`, every successful PURGE `PURGED 1`; after
the run all the sampled keys are INSERTed again and every INSERT must be placed.
"""
import argparse, json, multiprocessing as mp, os, random, shutil, subprocess, sys, threading, time

sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/bh-purge-key/tools")
from bench_common import Phase  # noqa
from ckdbs_cli import ServerConnection  # noqa
import bb_overhead_benchmark as bb  # noqa

BATCH = 500


def bgworker(kind, port, n, cpu, stop, q, seed):
    os.sched_setaffinity(0, {cpu})
    rnd = random.Random(seed)
    c = ServerConnection("127.0.0.1", port, timeout=30.0)
    ph = Phase(kind)
    t_start = time.perf_counter()
    i = 0
    while not stop.is_set():
        i += 1
        if kind == "reader":
            sql = f"SELECT * FROM t WHERE id = {rnd.randint(1, n)}"
        else:
            sql = f"UPDATE t SET v = {i} WHERE id = {rnd.randint(n // 2 + 1, n)}"
        t = time.perf_counter(); r = c.send_command(sql); ph.record(time.perf_counter() - t, r)
    ph.elapsed = time.perf_counter() - t_start
    q.put((kind, ph.summary()))


def start_server(a, tag):
    wd = os.path.join(a.workdir, tag)
    os.makedirs(wd, exist_ok=True)
    data = os.path.join(wd, "s.db"); conf = os.path.join(wd, "s.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {data}\nport = {a.port}\ncores = {a.cores}\ndurability = {a.durability}\n"
                f"buffer_pool_frames = {a.frames}\nlog_file = s.log\nlog_dir = {wd}\nlog_level = warn\n")
    cmd = ["taskset", "-c", str(a.pin_server), a.bin, "--config", conf]
    err = open(os.path.join(wd, "s.stderr"), "w")
    proc = subprocess.Popen(cmd, stdout=err, stderr=subprocess.STDOUT)
    deadline = time.time() + 30
    while True:
        try:
            conn = ServerConnection("127.0.0.1", a.port, timeout=30.0)
            return wd, proc, conn
        except OSError:
            if time.time() > deadline:
                raise SystemExit("server did not listen")
            time.sleep(0.1)


def purge_until(conn, k, cap=6):
    """Returns (total_s, first_attempt_s, refusals, final_reply)."""
    t_begin = time.perf_counter(); first = None; refusals = 0
    while True:
        t = time.perf_counter(); r = conn.send_command(f"PURGE FROM t WHERE id = {k}"); dt = time.perf_counter() - t
        if first is None:
            first = dt
        if r.startswith("ERR TXN_CONFLICT"):
            refusals += 1
            if refusals >= cap:
                return time.perf_counter() - t_begin, first, refusals, r
            continue
        return time.perf_counter() - t_begin, first, refusals, r


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--mode", default="readers", choices=["readers", "snapshot"])
    p.add_argument("--keys", type=int, default=20000)
    p.add_argument("--samples", type=int, default=2000)
    p.add_argument("--readers", type=int, default=0)
    p.add_argument("--writers", type=int, default=0)
    p.add_argument("--bg-cpus", default="4,6,7", help="cpus for background workers, round-robin")
    p.add_argument("--main-cpu", type=int, default=5)
    p.add_argument("--holds-ms", default="0,50,200,500,900,1100")
    p.add_argument("--reps", type=int, default=5)
    p.add_argument("--cores", type=int, default=2)
    p.add_argument("--durability", default="relaxed")
    p.add_argument("--frames", type=int, default=65536)
    p.add_argument("--bin", required=True)
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--pin-server", required=True)
    p.add_argument("--workdir", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--label", required=True)
    p.add_argument("--max-load", type=float, default=1.0)
    a = p.parse_args()
    os.sched_setaffinity(0, {a.main_cpu})
    wait = bb.wait_for_load(a.max_load, 300)
    before = bb.host_state()
    wd, proc, c = start_server(a, a.label)
    out = {"label": a.label, "mode": a.mode, "cores": a.cores, "durability": a.durability, "keys": a.keys,
           "readers": a.readers, "writers": a.writers, "wait_s": round(wait, 1), "host_before": before}
    stop = mp.get_context("spawn").Event()
    workers = []
    try:
        mount = bb.meta_all(c); out["mount_ckpt_us"] = mount.get("recovery_checkpoint_us")
        bb.must(c, "CREATE TABLE t (id int64, v int64, w int64) BTREE", "CREATED")
        for lo in range(1, a.keys + 1, BATCH):
            hi = min(a.keys, lo + BATCH - 1)
            bb.must(c, "INSERT INTO t VALUES " + ",".join(f"({i}, {i}, {i})" for i in range(lo, hi + 1)), "INSERTED")
        if a.mode == "readers":
            ctx = mp.get_context("spawn"); q = ctx.Queue()
            cpus = [int(x) for x in a.bg_cpus.split(",")]
            for i in range(a.readers + a.writers):
                kind = "reader" if i < a.readers else "writer"
                w = ctx.Process(target=bgworker, args=(kind, a.port, a.keys, cpus[i % len(cpus)], stop, q, 1000 + i))
                w.start(); workers.append(w)
            time.sleep(1.5)
            dph, tot, first = Phase("delete"), [], []
            refusals = 0; multi = 0; bad = 0
            t_run = time.perf_counter()
            for k in range(1, a.samples + 1):
                t = time.perf_counter(); r = c.send_command(f"DELETE FROM t WHERE id = {k}"); dph.record(time.perf_counter() - t, r)
                if r.strip() != "DELETED 1":
                    bad += 1
                total, f1, ref, r = purge_until(c, k)
                refusals += ref
                multi += 1 if ref else 0
                if r.strip() != "PURGED 1":
                    bad += 1
                    if bad < 5:
                        print("  bad reply:", r[:160], flush=True)
                tot.append(total); first.append(f1)
            run_s = time.perf_counter() - t_run
            stop.set()
            bgs = []
            for _ in workers:
                bgs.append(q.get(timeout=60))
            for w in workers:
                w.join(30)
            ph_t = Phase("delete-to-purge"); ph_t.latencies = tot; ph_t.elapsed = run_s
            ph_f = Phase("purge-first-attempt"); ph_f.latencies = first; ph_f.elapsed = run_s
            placed = 0; sep_refused = 0; other_refused = 0
            for k in range(1, a.samples + 1):
                r = c.send_command(f"INSERT INTO t VALUES ({k}, 0, 0)")
                if r.startswith("INSERTED"):
                    placed += 1
                elif "is already present in this internal node" in r:
                    sep_refused += 1
                else:
                    other_refused += 1
            out.update({"samples": a.samples, "delete": dph.summary(), "delete_to_purge": ph_t.summary(),
                        "purge_first_attempt": ph_f.summary(), "refusals": refusals, "keys_with_refusal": multi,
                        "bad": bad, "reinserted": placed, "reinsert_refused_separator": sep_refused, "reinsert_refused_other": other_refused, "run_s": round(run_s, 3),
                        "background": {"reader": [s for k, s in bgs if k == "reader"],
                                       "writer": [s for k, s in bgs if k == "writer"]}})
            ts = ph_t.summary()
            print(f"{a.label}: R={a.readers} W={a.writers} c{a.cores} total p0/25/50/95/99/max = {ts['p0_us']}/{ts['p25_us']}/{ts['p50_us']}/"
                  f"{ts['p95_us']}/{ts['p99_us']}/{ts['max_us']} us refusals={refusals} bad={bad} reins={placed}/{a.samples} sep_refused={sep_refused} other_refused={other_refused}", flush=True)
        else:
            rows = []
            c2 = ServerConnection("127.0.0.1", a.port, timeout=30.0)
            k = 0
            for hold in [int(x) for x in a.holds_ms.split(",")]:
                for rep in range(a.reps):
                    k += 1
                    bb.must(c2, "BEGIN", "BEGIN")
                    c2.send_command("SELECT COUNT(*) FROM t")
                    r = c.send_command(f"DELETE FROM t WHERE id = {k}")
                    assert r.strip() == "DELETED 1", r
                    t0 = time.perf_counter()
                    done = threading.Timer(hold / 1000.0, lambda: c2.send_command("COMMIT")) if hold > 0 else None
                    if done is None:
                        c2.send_command("COMMIT")
                    else:
                        done.start()
                    total, f1, ref, r = purge_until(c, k)
                    if done is not None:
                        done.join()
                    rows.append({"hold_ms": hold, "rep": rep, "total_ms": round(total * 1e3, 2),
                                 "first_attempt_ms": round(f1 * 1e3, 2), "refusals": ref, "reply": r[:40]})
                    print(f"  hold={hold}ms rep={rep}: total={total*1e3:.1f}ms first={f1*1e3:.1f}ms refusals={ref} {r[:30]}", flush=True)
            out["snapshot_rows"] = rows
        out["host_after"] = bb.host_state()
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, f"{a.label}.json"), "w") as f:
            json.dump(out, f, indent=1)
    finally:
        stop.set()
        for w in workers:
            if w.is_alive():
                w.terminate()
        try:
            c.close()
        except Exception:
            pass
        proc.terminate()
        try:
            proc.wait(20)
        except subprocess.TimeoutExpired:
            proc.kill(); proc.wait()
        shutil.rmtree(wd, ignore_errors=True)


if __name__ == "__main__":
    main()
