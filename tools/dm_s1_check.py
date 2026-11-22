#!/usr/bin/env python3
"""DM-S1's check of the cold update cell: rows affected and the pool's drain.

For each arm (`DM_MEMO_ENTRIES`), a server on a copy of DM-S1's loaded cold
directory runs the point update closed loop and counts statements whose
`rows_affected` is not 1 (a key the memo sent to a leaf that does not hold
it would answer 0), refusals by message, and the pool's counters across the
window (`SHOW META`'s `pool_*`).

    dm_s1_check.py --bin B --base <cold_base dir> --workdir D --out OUT.json
"""
import argparse, hashlib, json, os, random, shutil, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dm_s0_premise as s0  # noqa: E402
import kwp  # noqa: E402
import sys_meta  # noqa: E402

POOL = ["pool_misses", "pool_hits", "pool_reclaimed_inline", "pool_reclaimed_background",
        "pool_batches_inline", "pool_batches_background", "pool_dirty_queued",
        "pool_dirty_drained", "pool_refused", "pool_resident",
        "contention_checkpoint_pages_written", "contention_checkpoint_runs"]


def pool(conn):
    _, rows, tag, _ = conn.execute("SHOW META")
    meta = sys_meta.from_rows(rows, tag)
    return {k: float(meta[k]) if meta.get(k) not in (None, "") else None for k in POOL}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--bin", required=True)
    p.add_argument("--base", required=True)
    p.add_argument("--workdir", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--port", type=int, default=15781)
    p.add_argument("--arms", default="0,16384")
    p.add_argument("--rows", type=int, default=1_000_000)
    p.add_argument("--frames", type=int, default=2048)
    p.add_argument("--sessions", type=int, default=4)
    p.add_argument("--seconds", type=float, default=6.0)
    a = p.parse_args()
    # bench/README.md rules 3 and 4: the binary's hash and the host per cell.
    report = {"bin": a.bin, "sha256": hashlib.sha256(open(a.bin, "rb").read()).hexdigest(),
              "started": time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime()),
              "args": vars(a)}
    out = report["arms"] = []
    for arm in [int(x) for x in a.arms.split(",")]:
        host_before = s0.host_state()
        d = os.path.join(a.workdir, "chk")
        shutil.rmtree(d, ignore_errors=True)
        shutil.copytree(a.base, d)
        env = dict(os.environ)
        env["DM_MEMO_ENTRIES"] = str(arm)
        srv = s0.Server(a, 1, "chk", a.frames, fresh=False, env=env)
        try:
            admin = srv.conn()
            p0 = pool(admin)
            stop = threading.Event()
            res = {"arm": arm, "ok": 0, "not_one": 0, "errors": {}}
            lock = threading.Lock()

            def run(i):
                c = srv.conn()
                rng = random.Random(900 + i)
                while not stop.is_set():
                    k = 2 * rng.randrange(1, a.rows + 1)
                    try:
                        _, _, _, n = c.execute(f"UPDATE t1 SET w = w + 1 WHERE id = {k}")
                        with lock:
                            if n == 1:
                                res["ok"] += 1
                            else:
                                res["not_one"] += 1
                    except kwp.KwpError as e:
                        key = f"{e.code}:{' '.join(str(e.message).split()[:6])}"
                        with lock:
                            res["errors"][key] = res["errors"].get(key, 0) + 1
                c.close()

            th = [threading.Thread(target=run, args=(i,)) for i in range(a.sessions)]
            for t in th:
                t.start()
            time.sleep(a.seconds)
            stop.set()
            for t in th:
                t.join()
            p1 = pool(admin)
            res["pool"] = {k: (p1[k] - p0[k]) if p1[k] is not None and p0[k] is not None else None
                           for k in POOL}
            res["pool"]["pool_resident_end"] = p1["pool_resident"]
            res["host_before"] = host_before
            admin.close()
            out.append(res)
            print(json.dumps(res), flush=True)
        finally:
            srv.stop()
    report["ended"] = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime())
    with open(a.out, "w") as f:
        json.dump(report, f, indent=1)


if __name__ == "__main__":
    main()
