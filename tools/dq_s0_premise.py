#!/usr/bin/env python3
"""DQ-S0's premise: what a join on a cabined inner column costs before its keys converge.

`instructions/v3.2.0/workorder-dq-cabin-miss-build.md` section 5 (DQ-S0 (iii)).
One binary, `cores = 1`, `relaxed`, every page resident (the relations are
made in a 65,536-frame pool and never leave it), `join_order = written` so the
written outer side drives, and `join_build_max_rows` raised above the largest
inner relation so the build is not declined by its cap (the default, 65,536,
would decline a 100,000-row build and walk per outer row).

Per inner size N (`--sizes`) and per outer key distribution (`--dists`), each
round starts one fresh server per arm (a fresh Cabin store each), in an order
rotated and reversed per round:

    A      the inner column carries a declared Cabin (CREATE CABIN ON inn(k))
    Agate  as A, and a second session holds a write in flight for the whole
           cell (BEGIN; INSERT ...; renewed every --hold-renew-s), so the
           banking gate declines every recording (cabin.md section 6a)
    B      no Cabin: the statement-local build answers the inner step

and runs the same `--stmts` statements in sequence on it:

    ANALYZE SELECT o.id, i.w FROM outr AS o JOIN inn AS i ON i.k = o.k

or, with `--shape exists`, the correlated EXISTS over the same relations
(`SHAPES`). The outer relation holds 1,000 rows (`--outer`); every inner key k is
distinct (one inner row a key), drawn as a permutation of 1..N. The outer
keys:

    unique   1,000 distinct keys: no key repeats inside one statement
    repeat   100 distinct keys, each 10 times, shuffled: every key repeats
    skewed   500 rows over 20 hot keys (25 each), 500 distinct cold keys,
             shuffled

Per statement: server CPU (`/proc/<pid>/task/*/schedstat`, every thread,
compile and run together, read once no thread is runnable:
`settled_cpu_ns`), client wall, and the inner step's ANALYZE
counters (`opens`, `examined`, `cabin_hits`, `cabin_misses`,
`cabin_recorded`, `inner_built`, `build_rows`, `build_probes`). Walks of the
inner relation are `examined / N`.

    dq_s0_premise.py --bin B --workdir D --out OUT.json [--sizes 10000,100000]
                     [--dists unique,repeat,skewed] [--rounds 3] [--stmts 5]
                     [--port 15991] [--heartbeat F]

The binary must be a copy (bench/README.md rule 3); the driver hashes it.
"""
import argparse, hashlib, json, os, random, re, shutil, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
from dk_close import cpu_ns, host_state  # noqa: E402
from dp_close import rotated  # noqa: E402

ARMS = ["A", "Agate", "B"]
# The join DQ-S0 measured, and (`--shape exists`, DQ-S3) the correlated
# EXISTS over the same relations, whose inner step is a sub-chain's.
SHAPES = {
    "join": "SELECT o.id, i.w FROM outr AS o JOIN inn AS i ON i.k = o.k",
    "exists": "SELECT o.id FROM outr AS o WHERE EXISTS "
              "(SELECT i.id FROM inn AS i WHERE i.k = o.k)",
}
COUNTERS = ["opens", "examined", "matched", "cabin_hits", "cabin_misses", "cabin_recorded",
            "inner_built", "build_rows", "build_probes"]


def settled_cpu_ns(pid, limit_s=0.05):
    """`cpu_ns` once no server thread is runnable. A thread's schedstat
    runtime is folded in when it leaves the CPU or at a tick (4 ms at this
    host's HZ=250), so a read taken while the server still runs - the reply
    sent, its tail not yet off the CPU - misses up to a tick: DQ-S0 read
    0.00 ms for 1 ms statements. Bounded: a thread that never sleeps is read
    as it stands after `limit_s`."""
    deadline = time.perf_counter() + limit_s
    while time.perf_counter() < deadline:
        running = False
        for t in os.listdir(f"/proc/{pid}/task"):
            try:
                with open(f"/proc/{pid}/task/{t}/stat") as f:
                    running = f.read().rpartition(")")[2].split()[0] == "R"
            except (OSError, IndexError):
                continue
            if running:
                break
        if not running:
            break
        time.sleep(0.0002)
    return cpu_ns(pid)


def thread_cpu(pid):
    """CPU ns per server thread, keyed by thread id: every server thread has
    the binary's name as its comm, so a comm key folds them into one."""
    out = {}
    for t in os.listdir(f"/proc/{pid}/task"):
        try:
            with open(f"/proc/{pid}/task/{t}/schedstat") as f:
                out[t] = int(f.read().split()[0])
        except (OSError, ValueError):
            continue
    return out


def faults(pid):
    """The process's (minor, major) page faults so far, /proc/<pid>/stat
    fields 10 and 12."""
    with open(f"/proc/{pid}/stat") as f:
        rest = f.read().rpartition(")")[2].split()
    return int(rest[7]), int(rest[9])


class Server:
    def __init__(self, binary, workdir, tag, port, cpu, inner_rows, budget=None,
                 raise_cap=True, frames=65536, build_cap=None):
        self.dir = os.path.join(workdir, tag)
        shutil.rmtree(self.dir, ignore_errors=True)
        os.makedirs(self.dir)
        self.port = port
        conf = os.path.join(self.dir, "dq.conf")
        with open(conf, "w") as f:
            f.write(f"data_file = {self.dir}/dq.db\nport = {port}\ncores = 1\n"
                    f"durability = relaxed\nbuffer_pool_frames = {frames}\n"
                    f"log_file = dq.log\nlog_dir = {self.dir}\nlog_level = warn\n"
                    f"reactor_cpus = {cpu}\njoin_order = written\n"
                    + (f"join_build_max_rows = {build_cap}\n" if build_cap is not None else
                       f"join_build_max_rows = {max(65536, 2 * inner_rows)}\n"
                       if raise_cap else "")
                    + (f"max_rows_touched = {budget}\n" if budget else ""))
        self.err = open(os.path.join(self.dir, "dq.stderr"), "w")
        self.proc = subprocess.Popen([binary, "--config", conf], stdout=self.err,
                                     stderr=subprocess.STDOUT)
        deadline = time.time() + 30
        while True:
            try:
                kwp.Connection("127.0.0.1", port, timeout=600).close()
                break
            except OSError:
                if time.time() > deadline or self.proc.poll() is not None:
                    raise SystemExit(f"server {tag} did not listen on {port}")
                time.sleep(0.1)
        # Every server thread on the one CPU, so CPU time is one core's.
        for t in os.listdir(f"/proc/{self.proc.pid}/task"):
            try:
                os.sched_setaffinity(int(t), {cpu})
            except OSError:
                pass

    def conn(self):
        return kwp.Connection("127.0.0.1", self.port, timeout=600)

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(20)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.err.close()
        shutil.rmtree(self.dir, ignore_errors=True)


def outer_keys(dist, n_outer, n_inner, rng):
    if dist == "unique":
        return rng.sample(range(1, n_inner + 1), n_outer)
    if dist == "repeat":
        keys = rng.sample(range(1, n_inner + 1), n_outer // 10)
        out = [k for k in keys for _ in range(10)]
    elif dist == "skewed":
        pool = rng.sample(range(1, n_inner + 1), 20 + n_outer // 2)
        hot, cold = pool[:20], pool[20:]
        out = [k for k in hot for _ in range((n_outer // 2) // 20)] + cold
    else:
        raise ValueError(dist)
    rng.shuffle(out)
    return out


def load(c, table, rows):
    for i in range(0, len(rows), 500):
        vals = ",".join("(" + ",".join(str(x) for x in r) + ")" for r in rows[i:i + 500])
        c.execute(f"INSERT INTO {table} VALUES {vals}")


def predicted(keys, stmts):
    """The n = 2 rule over the statement sequence, with the gate open: per
    statement, (first touches, recording touches, served). A key's first
    touch sights it, its second records it, every later one is served."""
    seen = {}
    out = []
    for _ in range(stmts):
        first = rec = served = 0
        for k in keys:
            s = seen.get(k, 0)
            if s == 0:
                first += 1
            elif s == 1:
                rec += 1
            else:
                served += 1
            seen[k] = s + 1
        out.append({"first": first, "record": rec, "served": served})
    return out


def inner_counters(text):
    """The inner step's ANALYZE counters: the step line naming `inn`."""
    for line in text.splitlines():
        if line.startswith("step ") and " inn " in line and " opens=" in line:
            got = {"line": line.strip()}
            for name in COUNTERS:
                m = re.search(r"\b" + name + r"=(\d+)", line)
                got[name] = int(m.group(1)) if m else 0
            return got
    return {"line": None}


def plan_text(c, shape):
    rows = c.execute("ANALYZE " + SHAPES[shape])[1]
    return "\n".join(" ".join((f or b"").decode() for f in r) for r in rows)


def cell(a, binary, arm, n, dist, rnd, beat):
    rng = random.Random(1000 * n + 17 * len(dist) + rnd)
    port = a.port + (rnd % 2)
    srv = Server(binary, a.workdir, f"{arm}-{n}-{dist}-{rnd}", port, a.cpu, n,
                 a.max_rows_touched, raise_cap=not a.engine_build_cap, build_cap=a.build_cap)
    try:
        c = srv.conn()
        c.execute("CREATE TABLE inn (id int64, k int64, w int64) BTREE")
        perm = list(range(1, n + 1))
        rng.shuffle(perm)
        load(c, "inn", [(i + 1, perm[i], i) for i in range(n)])
        c.execute("CREATE TABLE outr (id int64, k int64) BTREE")
        keys = outer_keys(dist, a.outer, n, rng)
        load(c, "outr", [(i + 1, keys[i]) for i in range(len(keys))])
        if arm in ("A", "Agate"):
            c.execute("CREATE CABIN ON inn(k)")
        holder = None
        held_at = 0.0
        if arm == "Agate":
            holder = srv.conn()
            holder.execute("CREATE TABLE hold (id int64, v int64) BTREE")

        def hold():
            nonlocal held_at
            if holder is None:
                return
            if held_at and time.time() - held_at < a.hold_renew_s:
                return
            if held_at:
                holder.execute("ROLLBACK")
            holder.execute("BEGIN")
            holder.execute("INSERT INTO hold (v) VALUES (1)")
            held_at = time.time()

        stmts = []
        for s in range(a.stmts):
            hold()
            if a.gap_s:
                time.sleep(a.gap_s)  # outside the timed window
            th0 = thread_cpu(srv.proc.pid)
            f0 = faults(srv.proc.pid)
            t0 = settled_cpu_ns(srv.proc.pid)
            w0 = time.perf_counter()
            try:
                text = plan_text(c, a.shape)
            except kwp.KwpError as e:
                # A refusal is a result of the cell, not an end of the run:
                # the statement's CPU up to the refusal is still the cost.
                wall = time.perf_counter() - w0
                cpu_us = (settled_cpu_ns(srv.proc.pid) - t0) / 1000.0
                stmts.append({"refused": str(e)[:160], "cpu_us": round(cpu_us, 1),
                              "wall_us": round(wall * 1e6, 1)})
                beat(f"{arm} n={n} {dist} r{rnd} s{s + 1} cpu_ms={cpu_us / 1000:.1f} "
                     f"REFUSED {str(e)[:60]}")
                continue
            wall = time.perf_counter() - w0
            cpu_us = (settled_cpu_ns(srv.proc.pid) - t0) / 1000.0
            got = inner_counters(text)
            if got.get("line") is None:
                raise SystemExit(f"no inner step line in ANALYZE output: {text[:600]}")
            got["walks"] = round(got.get("examined", 0) / n, 2)
            got["cpu_us"] = round(cpu_us, 1)
            th1 = thread_cpu(srv.proc.pid)
            got["threads_us"] = {k: round((th1[k] - th0.get(k, 0)) / 1000.0, 1) for k in th1}
            f1 = faults(srv.proc.pid)
            got["minflt"], got["majflt"] = f1[0] - f0[0], f1[1] - f0[1]
            got["wall_us"] = round(wall * 1e6, 1)
            stmts.append(got)
            beat(f"{arm} n={n} {dist} r{rnd} s{s + 1} cpu_ms={cpu_us / 1000:.1f} "
                 f"walks={got['walks']} hits={got['cabin_hits']} misses={got['cabin_misses']} "
                 f"rec={got['cabin_recorded']} built={got['inner_built']}")
        if holder is not None:
            holder.execute("ROLLBACK")
            holder.close()
        c.close()
        return {"stmts": stmts, "predicted": predicted(keys, a.stmts),
                "distinct_keys": len(set(keys))}
    finally:
        srv.stop()


BUSY = ("cc1plus", "ld", "ctest", "kds_tests", "cmake", "ninja", "make")


def quiet_gate(beat, limit_s=1800):
    """Holds a cell while another session builds or runs a suite, read from
    /proc/*/comm (never from argv, which a waiting shell's words can match).
    Returns the seconds held; the hold is outside the stage's budget."""
    t0 = time.time()
    noted = False
    while time.time() - t0 < limit_s:
        busy = []
        for p in os.listdir("/proc"):
            if not p.isdigit():
                continue
            try:
                with open(f"/proc/{p}/comm") as f:
                    comm = f.read().strip()
            except OSError:
                continue
            if comm in BUSY:
                busy.append(comm)
        if not busy:
            return round(time.time() - t0, 1)
        if not noted or time.time() - noted > 50:
            beat(f"quiet gate holding {round(time.time() - t0)} s: {sorted(set(busy))}")
            noted = time.time()
        time.sleep(5)
    return round(time.time() - t0, 1)






def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--sizes", default="10000,100000")
    ap.add_argument("--dists", default="unique,repeat,skewed")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--stmts", type=int, default=5)
    ap.add_argument("--outer", type=int, default=1000)
    ap.add_argument("--port", type=int, default=15991)
    ap.add_argument("--cpu", type=int, default=2, help="the server's one CPU")
    ap.add_argument("--driver-cpus", default="0,4")
    ap.add_argument("--hold-renew-s", type=float, default=40.0)
    ap.add_argument("--heartbeat", default=None)
    ap.add_argument("--shape", default="join", choices=sorted(SHAPES))
    ap.add_argument("--gap-s", type=float, default=0.0,
                    help="sleep this many seconds before each statement, outside the timed "
                         "window (DQ close: tests whether periodic background work lands in "
                         "a different statement's window); per-statement CPU by thread comm "
                         "is recorded as threads_us")
    ap.add_argument("--build-cap", type=int, default=None,
                    help="write join_build_max_rows = N (0 takes the build away); overrides "
                         "--engine-build-cap and the raised default")
    ap.add_argument("--engine-build-cap", action="store_true",
                    help="leave join_build_max_rows at the engine default (65,536) instead "
                         "of raising it above the inner relation")
    ap.add_argument("--max-rows-touched", type=int, default=None,
                    help="the server's max_rows_touched; the engine default when absent")
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
    t_start = time.time()
    held_s = 0.0
    for n in [int(x) for x in a.sizes.split(",")]:
        for dist in a.dists.split(","):
            for rnd in range(a.rounds):
                for arm in rotated(ARMS, rnd):
                    held = quiet_gate(beat)
                    held_s += held
                    res = cell(a, a.bin, arm, n, dist, rnd, beat)
                    out["cells"].append({"arm": arm, "n": n, "dist": dist, "round": rnd,
                                         "host": host_state(), "quiet_hold_s": held, **res})
                    out["quiet_hold_s"] = round(held_s, 1)
                    with open(a.out, "w") as f:
                        json.dump(out, f, indent=1)
    out["host_after"] = host_state()
    out["elapsed_s"] = round(time.time() - t_start, 1)
    with open(a.out, "w") as f:
        json.dump(out, f, indent=1)
    beat(f"done elapsed_s={out['elapsed_s']}")


if __name__ == "__main__":
    main()
