#!/usr/bin/env python3
"""BH close, part 2a: the cost of a PURGE - keys freed per second. Scratch driver
(rule 5 of bench/README.md is broken: tools/ has no PURGE driver; this one is
shaped on bh_overhead_ab.py / tools/bb_overhead_benchmark.py).

One fresh server + data file per (cell, run). A relation p(id int64, v int64, w int64)
BTREE is loaded with --keys rows (named ascending keys 1..N, 500-row INSERTs). Then:

  window W == 1 : N x `DELETE FROM p WHERE id = k`, then N x `PURGE FROM p WHERE id = k`
  window W  > 1 : N/W x `DELETE FROM p WHERE id BETWEEN a AND a+W-1`, then the same PURGEs

Each phase is timed per statement (Phase.summary: p0/p25/p50/p95/p99) and as keys/s
(N / phase elapsed). Verification (always on):
  * before the PURGE phase, INSERT of the first key must be refused (the key is bound);
  * every PURGE reply must be `PURGED <W>`;
  * after it, SELECT COUNT(*) is 0, and all N keys are INSERTed again, one statement per key
    (named): placed, refused with the internal-node separator error (see the results file), or
    refused otherwise are counted apart; then COUNT(*) is read.
"""
import argparse, json, os, shutil, subprocess, sys, time

sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/bh-purge-key/tools")
from bench_common import Phase  # noqa
from ckdbs_cli import ServerConnection  # noqa
import bb_overhead_benchmark as bb  # noqa

BATCH = 500


class Srv:
    def __init__(self, binary, workdir, port, pin, tag, frames, cores, durability):
        os.makedirs(workdir, exist_ok=True)
        data = os.path.join(workdir, f"{tag}.db")
        conf = os.path.join(workdir, f"{tag}.conf")
        self.files = [data, os.path.join(workdir, f"{tag}.log")]
        with open(conf, "w") as f:
            f.write(f"data_file = {data}\nport = {port}\ncores = {cores}\ndurability = {durability}\n"
                    f"buffer_pool_frames = {frames}\nlog_file = {tag}.log\nlog_dir = {workdir}\nlog_level = warn\n")
        self.err = os.path.join(workdir, f"{tag}.stderr")
        cmd = (["taskset", "-c", str(pin)] if pin is not None else []) + [binary, "--config", conf]
        with open(self.err, "w") as e:
            self.proc = subprocess.Popen(cmd, stdout=e, stderr=subprocess.STDOUT)
        deadline = time.time() + 30
        while True:
            try:
                self.conn = ServerConnection("127.0.0.1", port, timeout=300.0)
                break
            except OSError:
                if time.time() > deadline:
                    raise SystemExit("server did not listen")
                time.sleep(0.1)
        self.mount = bb.meta_all(self.conn)

    def stop(self):
        try:
            self.conn.close()
        except Exception:
            pass
        self.proc.terminate()
        try:
            self.proc.wait(20)
        except subprocess.TimeoutExpired:
            self.proc.kill(); self.proc.wait()
        for f in self.files + [self.err]:
            try:
                os.remove(f)
            except OSError:
                pass


def run(args, n, window, run_i):
    tag = f"w{window}-n{n}-r{run_i}"
    wd = os.path.join(args.workdir, tag)
    wait = bb.wait_for_load(args.max_load, args.max_wait)
    before = bb.host_state()
    s = Srv(args.bin, wd, args.port, args.pin_server, tag, args.frames, args.cores, args.durability)
    try:
        c = s.conn
        bb.must(c, "CREATE TABLE p (id int64, v int64, w int64) BTREE", "CREATED")
        for lo in range(1, n + 1, BATCH):
            hi = min(n, lo + BATCH - 1)
            bb.must(c, "INSERT INTO p VALUES " + ",".join(f"({i}, {i}, {i})" for i in range(lo, hi + 1)), "INSERTED")
        assert c.send_command("SELECT COUNT(*) FROM p").replace("\\n", " ").split()[-1] == str(n)
        if window == 1:
            stmts = [(f"DELETE FROM p WHERE id = {k}", f"PURGE FROM p WHERE id = {k}", 1) for k in range(1, n + 1)]
        else:
            stmts = [(f"DELETE FROM p WHERE id BETWEEN {a} AND {a + window - 1}",
                      f"PURGE FROM p WHERE id BETWEEN {a} AND {a + window - 1}", window)
                     for a in range(1, n + 1, window)]
        dph, pph = Phase("delete"), Phase("purge")
        t0 = time.perf_counter()
        for d, _, _ in stmts:
            t = time.perf_counter(); r = c.send_command(d); dph.record(time.perf_counter() - t, r)
        dph.elapsed = time.perf_counter() - t0
        probe = c.send_command(f"INSERT INTO p VALUES (1, 0, 0)")
        bound_refused = probe.startswith("ERR")
        bad = 0
        t0 = time.perf_counter()
        for _, p, w in stmts:
            t = time.perf_counter(); r = c.send_command(p); pph.record(time.perf_counter() - t, r)
            if r.strip() != f"PURGED {w}":
                bad += 1
                if bad < 4:
                    print("  PURGE reply:", r[:200], flush=True)
        pph.elapsed = time.perf_counter() - t0
        live = c.send_command("SELECT COUNT(*) FROM p").replace("\\n", " ").split()[-1]
        t0 = time.perf_counter(); placed = 0; sep_refused = []; other_refused = []
        for k in range(1, n + 1):
            r = c.send_command(f"INSERT INTO p VALUES ({k}, {k}, {k})")
            if r.startswith("INSERTED"):
                placed += 1
            elif "is already present in this internal node" in r:
                sep_refused.append(k)
            else:
                other_refused.append((k, r[:100]))
        reins_s = time.perf_counter() - t0
        after_count = c.send_command("SELECT COUNT(*) FROM p").replace("\\n", " ").split()[-1]
        meta = bb.meta_all(c)
        after = bb.host_state()
        ds, ps = dph.summary(), pph.summary()
        out = {"tag": tag, "window": window, "keys": n, "run": run_i, "cores": args.cores,
               "durability": args.durability, "wait_s": round(wait, 1), "host_before": before, "host_after": after,
               "mount_ckpt_us": s.mount.get("recovery_checkpoint_us"),
               "delete": ds, "purge": ps,
               "delete_keys_per_s": round(n / dph.elapsed, 1), "purge_keys_per_s": round(n / pph.elapsed, 1),
               "bound_probe_refused": bound_refused, "bad_purge_replies": bad, "live_after_purge": live,
               "reinserted_keys": placed, "reinsert_refused_separator": len(sep_refused), "reinsert_refused_separator_keys": sep_refused[:40], "reinsert_refused_other": other_refused[:5], "reinsert_s": round(reins_s, 3), "count_after_reinsert": after_count}
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, f"{tag}-c{args.cores}-{args.durability}.json"), "w") as f:
            json.dump(out, f, indent=1)
        print(f"{tag} c{args.cores} {args.durability} load {before['loadavg'].split()[0]} "
              f"del {out['delete_keys_per_s']:.0f}/s purge {out['purge_keys_per_s']:.0f}/s p50={ps['p50_us']}us "
              f"errs d={ds['errors']} p={ps['errors']} bad={bad} bound_refused={bound_refused} live={live} reins={placed}/{n} sep_refused={len(sep_refused)} other_refused={len(other_refused)} after={after_count}",
              flush=True)
    finally:
        s.stop()
        shutil.rmtree(wd, ignore_errors=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--cells", required=True, help="comma list of window:keys, e.g. 1:200,1:10000,1000:30000")
    p.add_argument("--runs", type=int, default=5)
    p.add_argument("--cores", type=int, default=1)
    p.add_argument("--durability", default="relaxed")
    p.add_argument("--frames", type=int, default=65536)
    p.add_argument("--bin", required=True)
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--pin-server", default=None)
    p.add_argument("--workdir", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--max-load", type=float, default=1.0)
    p.add_argument("--max-wait", type=float, default=300)
    a = p.parse_args()
    for cell in a.cells.split(","):
        w, n = (int(x) for x in cell.split(":"))
        for r in range(a.runs):
            run(a, n, w, r)


if __name__ == "__main__":
    sys.exit(main())
