#!/usr/bin/env python3
"""T4 - what a sustained parked-coroutine population costs.

Statement shipping parks a waiter on the **arrival** core for every shipped
statement while the **owner** core executes it - the 6b-2/6b-3
`IndexBuildClient` shape. `bench/v2.1.0` §8 shows what parked coroutines do
to a reactor today: `Scheduler::IdleTimeoutMs` returns 0 while any ready
queue is non-empty (`src/sched/scheduler.cpp:196-199`), so a reactor holding
a parked task spins instead of blocking, and the trx-id refill leg spans
19,000-24,000 reactor iterations under load. Shipping would turn a rare
parked coroutine into a steady-state population, so the population's price is
owed **before** a line of shipping exists.

**The parking primitive, and why this one.** Nothing on the wire says "park",
and the candidates are narrower than they look:

  * a **lease refill** parks a peer's coroutine, but only until the grant
    lands - transient, and not controllable in number;
  * a **cross-core SELECT** would park the reading core for the length of the
    scan, and does not: from a client the shapes tried here are refused
    outright (*"cross-core reads need the step pipeline, which is not
    built"*), so the P4d pipeline is not reachable this way;
  * `CREATE INDEX` on a **peer-owned** relation, which core 0's session ships
    to the owner and then waits for (PW1c-6b). The work happens on the owner
    core rather than on the parked one, which is exactly what makes it a
    measurement of the *waiting* - and it is the 6b-2/6b-3 shape statement
    shipping will have.

**One build per parker, and the reason is a defect this probe found.** A
first version looped `CREATE INDEX`/`DROP INDEX` to hold the population
indefinitely. On `cores = 4` that drove the instance into
`ERR page id not found` on **every** subsequent write to an unrelated
peer-owned relation, permanently, after ~58 builds - while the identical
churn on `cores = 1` ran 400 builds clean and grew the file from 32 MB to
70 MB. So the population is held for exactly one round of K simultaneous
builds, which keeps every run two orders of magnitude below that threshold,
and the defect is reported rather than worked around silently.

So the probe holds K builds in flight from K sessions on core 0 and measures,
over exactly that window:

  * per-core CPU, against an idle window on the same mount;
  * `SHOW META`'s group accounting on core 0 (`sched_*`, T4's other half):
    polls, time inside polls, and reactor wall time. A spin shows up here
    directly - polls climb while polled time does not;
  * the T1b workload's insert p50/p99 on a peer core, with and without the
    parked population, which is what says whether shipping's waiters cost
    the statements anything.

DDL is core 0's alone (PW4), so the parked population can only be held on
core 0. That is not a limitation of the probe but of what the engine can be
asked to do from a client, and it is stated in the results rather than
worked around: the shape measured is *arrival core parks, owner core works*,
which is the shipping shape.

Usage:
    bench/parked_coroutine_probe.py --server build-release/kds_server \\
        --workdir ~/mcbench2/t4 --cores 4 --parked 4 --sessions 2
"""

import argparse
import json
import os
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from bench_common import nearest_rank  # noqa: E402
from multicore_benchmark import (  # noqa: E402
    Conn, check_host, collect_connections, field, is_retryable, wait_for_port,
)


def read_cpu_jiffies():
    out = {}
    with open("/proc/stat") as fh:
        for line in fh:
            if not line.startswith("cpu") or line.startswith("cpu "):
                continue
            parts = line.split()
            out[parts[0]] = [int(v) for v in parts[1:9]]
    return out


def busy_between(before, after):
    out = {}
    for cpu, a in after.items():
        b = before.get(cpu)
        if not b:
            continue
        d = [x - y for x, y in zip(a, b)]
        total = sum(d)
        out[cpu] = round((total - d[3] - d[4]) / total, 4) if total else 0.0
    return dict(sorted(out.items(), key=lambda kv: int(kv[0][3:])))


def sched_fields(meta):
    """The `sched_*` block off SHOW META as a dict, or {} on a build that does
    not print it. Absence is reported, never defaulted to zero: a zero would
    read as a reactor that did nothing."""
    out = {}
    for tok in meta.split():
        if tok.startswith("sched_") and "=" in tok:
            k, v = tok.split("=", 1)
            try:
                out[k] = int(v)
            except ValueError:
                pass
    return out


def sched_delta(before, after):
    return {k: after[k] - before.get(k, 0) for k in after}


def pct(values, p):
    return round(nearest_rank(sorted(values), p) * 1e6, 1) if values else None


def batched_load(conn, table, rows, batch=1000, deadline_s=30.0):
    """Fills `table` through explicit transactions - the fast path measured in
    T1a. A retryable refusal poisons the transaction, so the retry unit is
    the batch (txn_batch_probe.py's rule)."""
    done = 0
    while done < rows:
        n = min(batch, rows - done)
        end = time.time() + deadline_s
        while True:
            failed = None
            r = conn.cmd("BEGIN")
            if r.startswith("ERR"):
                raise RuntimeError(f"BEGIN: {r}")
            for i in range(n):
                r = conn.cmd(f"INSERT INTO {table} VALUES ('l', {done + i})")
                if r.startswith("ERR"):
                    failed = r
                    break
            if failed is None:
                r = conn.cmd("COMMIT")
                if not r.startswith("ERR"):
                    break
                failed = r
            conn.cmd("ROLLBACK")
            if is_retryable(failed) and time.time() < end:
                time.sleep(0.0005)
                continue
            raise RuntimeError(f"load {table}: {failed}")
        done += n
    return done


class Parker(threading.Thread):
    """One core-0 session holding **one** shipped CREATE INDEX in flight. The
    session's coroutine is parked on core 0 for the length of the build,
    which happens on the owner core.

    Exactly one build, and no DROP: see the module docstring for the defect
    that makes a loop unsafe. The relation's row count is what sets the
    window, so `--build-rows` is the knob for how long the population is
    held."""

    def __init__(self, conn, table, index_name, barrier, deadline_s=30.0):
        super().__init__()
        self.conn = conn
        self.table = table
        self.index_name = index_name
        self.barrier = barrier
        self.deadline_s = deadline_s
        self.build_s = None
        self.attempts = 0
        self.retries = 0
        self.reply = None
        self.error = None

    def run(self):
        self.barrier.wait()
        # A build on a peer whose extent lease is spent is refused
        # **retryably** - the owner asks core 0 for a refill on its drain
        # tick and the retry then builds. Retried here for the same reason
        # every INSERT in this directory is: the refusal is the contract,
        # not the thing being measured. A refused attempt allocates nothing,
        # so retrying does not walk toward the defect the docstring names.
        end = time.time() + self.deadline_s
        while True:
            self.attempts += 1
            t0 = time.perf_counter()
            r = self.conn.cmd(f"CREATE INDEX {self.index_name}_{self.attempts} "
                              f"ON {self.table} (owner)")
            dt = time.perf_counter() - t0
            self.reply = r[:160]
            if not r.startswith("ERR"):
                self.build_s = dt
                return
            if is_retryable(r) and time.time() < end:
                self.retries += 1
                time.sleep(0.002)
                continue
            self.error = r[:160]
            self.build_s = dt
            return


class Inserter(threading.Thread):
    """The T1b workload: autocommit INSERTs into the hot relation from a
    session on its owner core, for a fixed window."""

    def __init__(self, conn, table, tag, stop_event, deadline_s=20.0):
        super().__init__()
        self.conn = conn
        self.table = table
        self.tag = tag
        self.stop_event = stop_event
        self.deadline_s = deadline_s
        self.lat = []
        self.inserted = 0
        self.retries = 0
        self.errors = 0
        self.first_error = None

    def run(self):
        i = 0
        while not self.stop_event.is_set():
            stmt = f"INSERT INTO hot VALUES ('{self.tag}', {i})"
            i += 1
            t0 = time.perf_counter()
            end = time.time() + self.deadline_s
            while True:
                r = self.conn.cmd(stmt)
                if not r.startswith("ERR"):
                    self.inserted += 1
                    break
                if is_retryable(r) and time.time() < end:
                    self.retries += 1
                    continue
                self.errors += 1
                if self.first_error is None:
                    self.first_error = r
                break
            self.lat.append(time.perf_counter() - t0)


def workload_window(conns, seconds, parkers=None, park_barrier=None):
    """Runs the inserters and returns their aggregate.

    With `parkers`, the window is *their* window: the inserters start, the
    parkers are released, and everything stops when the last build returns.
    That is what makes the CPU sample and the `sched_*` delta describe the
    period the coroutines were actually parked, rather than a fixed slice
    that includes time before and after."""
    stop = threading.Event()
    workers = [Inserter(c, "hot", f"s{i}", stop) for i, c in enumerate(conns)]
    before = read_cpu_jiffies()
    t0 = time.perf_counter()
    for w in workers:
        w.start()
    if parkers:
        for p in parkers:
            p.start()
        park_barrier.wait()          # every build leaves at the same instant
        for p in parkers:
            p.join()
    else:
        time.sleep(seconds)
    stop.set()
    for w in workers:
        w.join()
    wall = time.perf_counter() - t0
    busy = busy_between(before, read_cpu_jiffies())
    lat = [x for w in workers for x in w.lat]
    inserted = sum(w.inserted for w in workers)
    return dict(seconds=round(wall, 4), inserted=inserted,
                ips=round(inserted / wall, 1) if wall else 0.0,
                p50_us=pct(lat, 50), p99_us=pct(lat, 99),
                p0_us=round(min(lat) * 1e6, 1) if lat else None,
                p25_us=pct(lat, 25), p75_us=pct(lat, 75),
                retries=sum(w.retries for w in workers),
                errors=sum(w.errors for w in workers),
                first_error=next((w.first_error for w in workers if w.first_error), None),
                busy=busy)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="build-release/kds_server")
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--cores", type=int, default=4)
    ap.add_argument("--parked", type=int, default=4,
                    help="K: coroutines held parked on core 0")
    ap.add_argument("--sessions", type=int, default=2,
                    help="T1b workload sessions on the hot relation's owner core")
    ap.add_argument("--build-rows", type=int, default=100000,
                    help="rows per parked relation; the build's length IS the "
                         "window the population is held for, since each parker "
                         "issues exactly one build")
    ap.add_argument("--seconds", type=float, default=8.0)
    ap.add_argument("--idle-seconds", type=float, default=5.0)
    ap.add_argument("--port", type=int, default=17800)
    ap.add_argument("--max-connects", type=int, default=512)
    ap.add_argument("--json", default="")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    check_host(args.workdir, args.force)
    workdir = os.path.join(args.workdir, f"c{args.cores}-k{args.parked}-s{args.sessions}")
    os.makedirs(workdir, exist_ok=True)
    conf = os.path.join(workdir, "s.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {os.path.join(workdir, 's.db')}\nport = {args.port}\n"
                f"cores = {args.cores}\nplacement = rotate\npeer_listeners = on\n"
                f"log_file = s.log\nlog_dir = {workdir}\nlog_level = warn\n")
    stderr_path = os.path.join(workdir, "s.stderr")
    with open(stderr_path, "w") as err:
        proc = subprocess.Popen([os.path.abspath(args.server), "--config", conf],
                                stdout=err, stderr=subprocess.STDOUT)
    out = dict(cores=args.cores, parked=args.parked, sessions=args.sessions,
               build_rows=args.build_rows, seconds=args.seconds)
    try:
        wait_for_port(args.port, stderr_path)
        got, _ = collect_connections(args.port, {0: 1}, args.max_connects)
        setup = got[0][0]

        # The hot relation first, so its owner is known before the parked
        # ones are placed; rotation assigns by creation sequence.
        r = setup.cmd("CREATE TABLE hot (id int64, owner varchar, balance int64) BTREE")
        if r.startswith("ERR"):
            raise RuntimeError(f"CREATE TABLE hot: {r}")
        hot_owner = int(field(setup.cmd("DESCRIBE hot"), "owner_core"))
        # **The parked relations must not share the hot relation's owner.**
        # A build runs on the owner core; if that is the core the workload is
        # also on, the probe would price the owner's *work* and call it the
        # cost of waiting. Rotation assigns by creation sequence, so
        # relations are created until `--parked` of them have landed on some
        # other core, and the ones that landed on the hot owner are left
        # unused rather than deleted (DROP TABLE orphans pages).
        parked_tables = []
        created = 0
        skipped = 0
        while len(parked_tables) < args.parked:
            if created >= args.parked * 8:
                raise RuntimeError(
                    f"after {created} relations only {len(parked_tables)} landed off "
                    f"core {hot_owner}; rotation cannot supply this shape")
            name = f"pk{created}"
            created += 1
            r = setup.cmd(f"CREATE TABLE {name} (id int64, owner varchar, balance int64) BTREE")
            if r.startswith("ERR"):
                raise RuntimeError(f"{name}: {r}")
            owner = int(field(setup.cmd(f"DESCRIBE {name}"), "owner_core"))
            if owner == hot_owner:
                skipped += 1
                continue
            parked_tables.append((name, owner))
        out["hot_owner_core"] = hot_owner
        out["parked_tables"] = parked_tables
        out["relations_skipped_on_hot_owner"] = skipped

        # Load every parked relation from a session on its own owner core:
        # rows must be there for the build to take time, and only the owner
        # may write them.
        for name, owner in parked_tables:
            per_core, _ = collect_connections(args.port, {owner: 1}, args.max_connects)
            c = per_core[owner][0]
            # The first INSERT pays the refill; batched_load retries it.
            batched_load(c, name, args.build_rows)
            c.close()

        # The workload sessions, on the hot relation's owner core.
        per_core, _ = collect_connections(args.port, {hot_owner: args.sessions},
                                          args.max_connects)
        hot_conns = per_core[hot_owner]
        end = time.time() + 20
        while True:
            r = hot_conns[0].cmd("INSERT INTO hot VALUES ('warm', 0)")
            if not r.startswith("ERR"):
                break
            if is_retryable(r) and time.time() < end:
                time.sleep(0.0005)
                continue
            raise RuntimeError(f"hot warm-up: {r}")

        # An idle window on the same mount, with nothing parked: the floor
        # every busy number below is read against.
        before = read_cpu_jiffies()
        meta0 = sched_fields(setup.cmd("SHOW META"))
        time.sleep(args.idle_seconds)
        out["idle_busy"] = busy_between(before, read_cpu_jiffies())
        out["idle_sched_delta"] = sched_delta(meta0, sched_fields(setup.cmd("SHOW META")))

        # Arm 1: the workload alone. Run **twice**, before and after the
        # parked arm, because §7a of the results file measured this harness
        # favouring whichever arm runs later by ~10% - and an unpaired
        # before/after would credit that to the parked population.
        meta_a = sched_fields(setup.cmd("SHOW META"))
        out["workload_alone"] = workload_window(hot_conns, args.seconds)
        out["workload_alone"]["sched_delta"] = sched_delta(
            meta_a, sched_fields(setup.cmd("SHOW META")))

        # Arm 2: K builds in flight from K core-0 sessions, with the same
        # workload running underneath. The window ends when the last build
        # returns, so every number below describes the parked period.
        park_conns = []
        for _ in parked_tables:
            p_got, _ = collect_connections(args.port, {0: 1}, args.max_connects)
            park_conns.append(p_got[0][0])
        park_barrier = threading.Barrier(len(parked_tables) + 1)
        parkers = [Parker(c, name, f"ix{i}", park_barrier)
                   for i, ((name, _owner), c) in enumerate(zip(parked_tables, park_conns))]

        meta_c = sched_fields(setup.cmd("SHOW META"))
        out["workload_with_parked"] = workload_window(hot_conns, args.seconds,
                                                      parkers, park_barrier)
        out["workload_with_parked"]["sched_delta"] = sched_delta(
            meta_c, sched_fields(setup.cmd("SHOW META")))

        out["parkers"] = [dict(table=p.table,
                               build_s=round(p.build_s, 4) if p.build_s else None,
                               attempts=p.attempts, retries=p.retries,
                               reply=p.reply, error=p.error)
                          for p in parkers]
        out["parked_window_s"] = out["workload_with_parked"]["seconds"]
        for c in park_conns:
            c.close()

        # Arm 1 again, after the parked window, for the pairing above.
        meta_a2 = sched_fields(setup.cmd("SHOW META"))
        out["workload_alone_after"] = workload_window(hot_conns, args.seconds)
        out["workload_alone_after"]["sched_delta"] = sched_delta(
            meta_a2, sched_fields(setup.cmd("SHOW META")))

        a, a2, b = (out["workload_alone"], out["workload_alone_after"],
                    out["workload_with_parked"])
        # The parked arm against the *mean* of the two unparked arms, which
        # is what removes a monotone drift across the run rather than
        # charging it to the population.
        alone_ips = (a["ips"] + a2["ips"]) / 2 if a["ips"] and a2["ips"] else None
        alone_p50 = ((a["p50_us"] + a2["p50_us"]) / 2
                     if a["p50_us"] and a2["p50_us"] else None)
        out["alone_ips_mean"] = round(alone_ips, 1) if alone_ips else None
        out["alone_p50_mean_us"] = round(alone_p50, 1) if alone_p50 else None
        out["p50_cost"] = round(b["p50_us"] / alone_p50, 4) if alone_p50 else None
        out["ips_cost"] = round(b["ips"] / alone_ips, 4) if alone_ips else None
        # The drift the pairing exists to remove, reported rather than hidden.
        out["alone_drift"] = (round(a2["ips"] / a["ips"], 4)
                              if a["ips"] and a2["ips"] else None)
        for c in hot_conns:
            c.close()
        setup.cmd("STOP")
        setup.close()
    finally:
        try:
            proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            proc.terminate()
            try:
                proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=10)
    print(json.dumps(out, indent=2))
    if args.json:
        with open(args.json, "w") as fh:
            json.dump(out, fh, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
