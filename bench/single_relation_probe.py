#!/usr/bin/env python3
"""T1b / SS-B - N sessions against R relations, and where those sessions sit.

`bench/v2.1.0` §10 states what its matrix cannot see: *"the case that
motivates the stride-forest proposal - many writers contending on one
relation's ascending key - is not exercised at all"*. Every cell there runs N
non-interfering relations, one session each.

This runs the opposite shape. One relation (or R of them), N sessions, every
session inserting rows whose Keystone pk the engine issues - so every insert
lands at the same ascending tail.

**`--seat` is the SS-B extension** (2026-08-26). T1b could only seat its
sessions on the relation's owner, because a session elsewhere was refused
(`crosscore.md` CC3) and DML shipping was unbuilt. Statement shipping makes
the other seat measurable, and the pair is the A/B every SS-B write cell is:

  owner    every session sits on the relation's owner core. The local arm,
           byte-for-byte the T1b shape this file shipped with.
  foreign  every session sits on a core that does **not** own the relation
           it writes, so every statement is shipped to the owner, executed
           there and answered back. The arrival cores are all the other
           cores, core 0 included - which is what an application that has
           never heard of core placement gets.

`--relations R` runs R relations with the sessions dealt round-robin over
them (session i writes relation `i % R`); `--same-owner` keeps creating
relations until R of them share one owner core, which is what separates
per-page serialization from per-core serialization with the sync, the
reactor and the wire held constant (SS-B3).

Two arms, chosen by `--arm`:

  multi   `cores = N`, `placement = rotate`, peer listeners on.
  single  `cores = 1`. The relation and every session are core 0's. The
          control that says how much of the multi arm's number is the peer
          write path rather than the contention. `--seat foreign` is
          meaningless here and is refused.

One session count per invocation, one fresh server per invocation: rows
accumulate in the relation and a second run on the same file would measure a
taller btree. The orchestrator sweeps.

Usage:
    bench/single_relation_probe.py --server build-release/kds_server \
        --workdir ~/mcbench/t1b --arm multi --cores 4 --sessions 4 --rows 2000
    bench/single_relation_probe.py --server build-release/kds_server \
        --workdir ~/ssb/b2 --arm multi --cores 4 --sessions 8 --rows 500 \
        --seat foreign
"""

import argparse
import collections
import json
import os
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, HERE)
from bench_common import nearest_rank  # noqa: E402
from multicore_benchmark import (  # noqa: E402
    Conn, check_host, collect_connections, field, is_retryable, wait_for_port,
)
from refusal_baseline_probe import classify  # noqa: E402


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
    """Per-logical-CPU busy fraction over the window. A reactor core is
    pinned to the like-numbered CPU (`expeditor.cpp:1006` PinToCore), so
    `cpu1` is reactor core 1 and the mapping needs no inference."""
    out = {}
    for cpu, a in after.items():
        b = before.get(cpu)
        if not b:
            continue
        delta = [x - y for x, y in zip(a, b)]
        total = sum(delta)
        idle = delta[3] + delta[4]
        out[cpu] = round((total - idle) / total, 4) if total else 0.0
    return dict(sorted(out.items(), key=lambda kv: int(kv[0][3:])))


class Inserter(threading.Thread):
    """One session's autocommit INSERTs into its relation.

    The latency recorded is the whole wait including retries - what the
    client experienced - and the retries are counted beside it, because a
    peer's lease refusals (PW1b and the trx-id/extent leases when spent) are
    a cost the percentiles would otherwise hide inside the tail.

    `attempted` is statements the client meant to run, `sent` is wire sends
    (attempted + retries), `executed` is statements answered without ERR and
    `refused` is statements that ended in one. SS-B requires all three per
    cell and requires that a refusal never become a denominator, so they are
    counted apart rather than derived.
    """

    def __init__(self, conn, table, rows, tag, barrier, deadline_s=20.0):
        super().__init__()
        self.conn = conn
        self.table = table
        self.rows = rows
        self.tag = tag
        self.barrier = barrier
        self.deadline_s = deadline_s
        self.lat = []
        self.attempted = 0
        self.sent = 0
        self.inserted = 0
        self.retries = 0
        self.errors = 0
        self.by_class = {}
        self.first_error = None

    def run(self):
        self.barrier.wait()
        for i in range(self.rows):
            stmt = f"INSERT INTO {self.table} VALUES ('{self.tag}', {i})"
            self.attempted += 1
            t0 = time.perf_counter()
            end = time.time() + self.deadline_s
            while True:
                r = self.conn.cmd(stmt)
                self.sent += 1
                if not r.startswith("ERR"):
                    self.inserted += 1
                    break
                if is_retryable(r) and time.time() < end:
                    self.retries += 1
                    continue
                self.errors += 1
                name = classify(r)
                self.by_class[name] = self.by_class.get(name, 0) + 1
                if self.first_error is None:
                    self.first_error = r
                break
            self.lat.append(time.perf_counter() - t0)


def pct(values, p):
    return round(nearest_rank(sorted(values), p) * 1e6, 1) if values else None


def count_of(reply):
    try:
        return int(reply.replace("\\n", "\n").split("\n")[-1].split(",")[-1])
    except (ValueError, AttributeError):
        return None


def meta_fields(meta, prefixes):
    """Every `key=value` on a SHOW META line whose key starts with one of
    `prefixes`, as text - the counters SS-B reads are integers but a missing
    field must read as absent, never as zero."""
    out = {}
    for tok in meta.split():
        if "=" not in tok:
            continue
        k, v = tok.split("=", 1)
        if any(k.startswith(p) for p in prefixes):
            out[k] = v
    return out


META_PREFIXES = ("shipped_", "cross_core_write_refusal", "sched_", "core",
                 "rowid_refill_", "trxid_refill_", "extent_refill_")


def run_once(args, port):
    multi = args.arm == "multi"
    cores = args.cores if multi else 1
    tag = f"{args.arm}-c{cores}-s{args.sessions}-r{args.relations}-{args.seat}"
    workdir = os.path.join(args.workdir, tag)
    os.makedirs(workdir, exist_ok=True)
    conf = os.path.join(workdir, "probe.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {os.path.join(workdir, 'probe.db')}\n"
                f"port = {port}\ncores = {cores}\n"
                f"placement = {'rotate' if multi else 'creating'}\n"
                f"peer_listeners = {'on' if multi else 'off'}\n"
                f"durability = {args.durability}\n"
                f"wal_drain_interval_us = {args.wal_drain_interval_us}\n"
                + (f"relaxed_flush_interval_us = {args.relaxed_flush_interval_us}\n"
                   if args.relaxed_flush_interval_us >= 0 else "")
                + f"log_file = probe.log\nlog_dir = {workdir}\nlog_level = warn\n")
    stderr_path = os.path.join(workdir, "probe.stderr")
    with open(stderr_path, "w") as err:
        proc = subprocess.Popen([args.server, "--config", conf],
                                stdout=err, stderr=subprocess.STDOUT)
    out = dict(arm=args.arm, cores=cores, sessions=args.sessions, rows=args.rows,
               relations=args.relations, seat=args.seat,
               same_owner=bool(args.same_owner),
               rows_total=args.rows * args.sessions)
    try:
        wait_for_port(port, stderr_path)
        if multi:
            got, _ = collect_connections(port, {0: 1}, args.max_connects)
            setup = got[0][0]
        else:
            setup = Conn(port)

        # Relations. `--same-owner` keeps making them until R share one
        # owner; the surplus stay empty and are named in the output so a
        # reader can see the shape rather than infer it.
        names, owners, created = [], {}, []
        seq = 0
        while len(names) < args.relations:
            name = f"hot{seq}"
            seq += 1
            r = setup.cmd(f"CREATE TABLE {name} "
                          f"(id int64, owner varchar, balance int64) BTREE")
            if r.startswith("ERR"):
                raise RuntimeError(f"CREATE TABLE {name}: {r}")
            own = int(field(setup.cmd(f"DESCRIBE {name}"), "owner_core"))
            created.append(name)
            if args.same_owner and names and own != owners[names[0]]:
                continue
            names.append(name)
            owners[name] = own
        out["relations_created"] = created
        out["relations_used"] = names
        out["owner_cores"] = owners
        owner_set = sorted(set(owners.values()))
        out["owner_core"] = owners[names[0]]

        # Which core each session sits on. `owner` is T1b's seat; `foreign`
        # is every other core, dealt round-robin, which is what makes the
        # statement ship.
        rel_of = [names[i % len(names)] for i in range(args.sessions)]
        seats = []
        for i in range(args.sessions):
            own = owners[rel_of[i]]
            if args.seat == "owner":
                seats.append(own)
            elif args.arrival_core is not None:
                # Every session on **one** arrival core: SS-B4 needs K parked
                # waiters on a core it can then read `polls`/`polled_us` off,
                # and a K spread over three cores is not that population.
                #
                # `-1` means "the lowest peer core that is not the owner":
                # rotation picks the owner by creation sequence and a fixed
                # number would sometimes name it. Core 0 is excluded because
                # it carries the listener, the catalog and the lease grants,
                # and a poll block read off it would be measuring those.
                if args.arrival_core < 0:
                    pick = next(c for c in range(1, cores) if c != own)
                elif args.arrival_core == own:
                    raise RuntimeError(f"--arrival-core {args.arrival_core} owns {rel_of[i]}")
                else:
                    pick = args.arrival_core
                seats.append(pick)
            else:
                elsewhere = [c for c in range(cores) if c != own]
                if not elsewhere:
                    raise RuntimeError("--seat foreign needs a core that is not the owner")
                seats.append(elsewhere[i % len(elsewhere)])
        out["session_seats"] = seats
        out["session_relations"] = rel_of

        if multi:
            per_core, attempts = collect_connections(
                port, dict(collections.Counter(seats)), args.max_connects)
            pool = {c: list(v) for c, v in per_core.items()}
            conns = [pool[s].pop() for s in seats]
            out["connect_attempts"] = attempts
        else:
            conns = [Conn(port) for _ in range(args.sessions)]
        setup.close()

        # The first INSERT on a peer pays the row-id refill and the btree's
        # first extent (PW1b); both refuse retryably until the grant lands.
        # Paid here, before the window, so the sweep's session-1 point is not
        # a refill measurement. One per relation, from a session that writes it.
        warm_retries = 0
        for name in names:
            i = next(k for k in range(args.sessions) if rel_of[k] == name)
            end = time.time() + args.retry_deadline
            while True:
                r = conns[i].cmd(f"INSERT INTO {name} VALUES ('warm', 0)")
                if not r.startswith("ERR"):
                    break
                if is_retryable(r) and time.time() < end:
                    warm_retries += 1
                    time.sleep(0.0005)
                    continue
                raise RuntimeError(f"warm-up on {name}: {r}")
        out["warmup_retries"] = warm_retries

        barrier = threading.Barrier(args.sessions)
        workers = [Inserter(conns[i], rel_of[i], args.rows, f"s{i}", barrier,
                            args.retry_deadline)
                   for i in range(args.sessions)]
        cpu_before = read_cpu_jiffies()
        t0 = time.perf_counter()
        for w in workers:
            w.start()
        for w in workers:
            w.join()
        wall = time.perf_counter() - t0
        out["cpu_busy"] = busy_between(cpu_before, read_cpu_jiffies())

        inserted = sum(w.inserted for w in workers)
        lat = [x for w in workers for x in w.lat]
        by_class = {}
        for w in workers:
            for k, v in w.by_class.items():
                by_class[k] = by_class.get(k, 0) + v
        attempted = sum(w.attempted for w in workers)
        out.update(
            wall_s=round(wall, 4),
            attempted=attempted,
            executed=inserted,
            refused=attempted - inserted,
            refusal_rate=round((attempted - inserted) / attempted, 6) if attempted else None,
            refusal_classes=by_class,
            sent=sum(w.sent for w in workers),
            inserted=inserted,
            inserts_per_second=round(inserted / wall, 1) if wall else 0.0,
            retries=sum(w.retries for w in workers),
            errors=sum(w.errors for w in workers),
            first_error=next((w.first_error for w in workers if w.first_error), None),
            insert_p50_us=pct(lat, 50), insert_p99_us=pct(lat, 99),
            insert_p0_us=round(min(lat) * 1e6, 1) if lat else None,
            insert_p25_us=pct(lat, 25),
            insert_p75_us=pct(lat, 75),
            insert_p95_us=pct(lat, 95),
            insert_mean_us=round(sum(lat) / len(lat) * 1e6, 1) if lat else None,
        )

        if args.trace_latencies:
            # Arrival order, per session, microseconds. SS-B6 needs the
            # *series*, not its percentiles: a step at a lease-block boundary
            # is invisible to every summary statistic.
            out["latency_trace_us"] = [[round(x * 1e6, 1) for x in w.lat]
                                       for w in workers]

        # Rows in = rows out, per relation, counted from the engine. A cell
        # that loses a row is a blocking finding, so the expectation is
        # spelled per relation rather than summed.
        verify, verify_ok = {}, True
        for name in names:
            expect = sum(w.inserted for w in workers if w.table == name) + 1
            reply = conns[0].cmd(f"SELECT COUNT(*) FROM {name}")
            got = count_of(reply)
            verify[name] = dict(expected=expect, got=got, ok=(got == expect))
            verify_ok = verify_ok and got == expect
        out["verify_per_relation"] = verify
        out["verify"] = ("rows as expected" if verify_ok
                         else "MISMATCH: " + json.dumps(verify))

        # Every core's counters: the ship counters are arrival-core-local and
        # the executed counters owner-local, so a total needs one reading per
        # core - the same reason `refusal_baseline_probe.py` reads them all.
        if multi:
            reader, _ = collect_connections(port, {c: 1 for c in range(cores)},
                                            args.max_connects)
            per = {}
            for core, cs in sorted(reader.items()):
                meta = cs[0].cmd("SHOW META")
                per[f"core{core}"] = meta_fields(meta, META_PREFIXES)
                if core == owners[names[0]]:
                    out["meta"] = meta
                cs[0].close()
            out["meta_by_core"] = per
        out["owner_cores_distinct"] = owner_set

        for c in conns:
            c.close()
        stop = Conn(port)
        stop.cmd("STOP")
        stop.close()
    finally:
        try:
            proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            proc.terminate()
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=10)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="build-release/kds_server")
    ap.add_argument("--workdir", required=True, help="on a block device, never tmpfs")
    ap.add_argument("--arm", choices=("multi", "single"), default="multi")
    ap.add_argument("--cores", type=int, default=4)
    ap.add_argument("--sessions", type=int, default=1)
    ap.add_argument("--relations", type=int, default=1,
                    help="relations the sessions are dealt over, round-robin")
    ap.add_argument("--same-owner", action="store_true",
                    help="keep creating relations until --relations of them share an owner")
    ap.add_argument("--seat", choices=("owner", "foreign"), default="owner",
                    help="owner: T1b's local arm. foreign: every statement ships.")
    ap.add_argument("--durability", choices=("group", "relaxed", "strict"),
                    default="group",
                    help="the server's durability class. `relaxed` is the control that "
                         "takes the device sync out of both arms, which is how a cost "
                         "that is a sync is told from one that is not.")
    ap.add_argument("--wal-drain-interval-us", type=int, default=1000,
                    help="the system-group WAL drain timer. It is also the longest an "
                         "idle reactor blocks (`IdleTimeoutMs` caps the 10 ms idle block "
                         "at the next timer, sched/scheduler.cpp:196-214), so it bounds "
                         "how long a ring message waits on a core with nothing to do.")
    ap.add_argument("--relaxed-flush-interval-us", type=int, default=-1,
                    help="the `relaxed` loss window, and the period of the sync that "
                         "enforces it. That sync runs on the reactor thread, so it is "
                         "also the period of `relaxed`'s latency tail "
                         "(expeditor.hpp: one ~2.2 ms statement every 12 ms at the "
                         "10 ms default). 0 disables the sync outright (expeditor.hpp), "
                         "which is the discriminator: a tail that survives it is not this "
                         "sync. -1, the default, leaves the engine's own value - what "
                         "every cell before RW-B cell 2 measured.")
    ap.add_argument("--arrival-core", type=int, default=None,
                    help="with --seat foreign, put every session on this one core")
    ap.add_argument("--trace-latencies", action="store_true",
                    help="emit every statement's latency in arrival order, per session "
                         "(SS-B6 reads the lease-block boundary out of it)")
    ap.add_argument("--rows", type=int, default=2000,
                    help="rows per session; the relation takes sessions x rows")
    ap.add_argument("--port", type=int, default=16200)
    ap.add_argument("--max-connects", type=int, default=512)
    ap.add_argument("--retry-deadline", type=float, default=20.0)
    ap.add_argument("--json", default="")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    if args.arm == "single" and args.seat == "foreign":
        sys.exit("--seat foreign has no meaning at cores = 1: there is no foreign core")

    os.makedirs(args.workdir, exist_ok=True)
    check_host(args.workdir, args.force)
    out = run_once(args, args.port)
    print(json.dumps(out, indent=2))
    if args.json:
        with open(args.json, "w") as fh:
            json.dump(out, fh, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
