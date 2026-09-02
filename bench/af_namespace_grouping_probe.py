#!/usr/bin/env python3
"""AF-T5: does supplying the grouping recover rotation's parallelism?

`instructions/v2.8.0/ratification-af-namespace.md` AF-T5 states the
hypothesis this exists to falsify:

    namespace placement recovers rotation's parallelism without rotation's
    crossing cost, because no join in the workload crosses.

**Why the DA2 cell cannot answer it, stated first because AF-T5 asks for that
cell re-run.** DA2's 0.51x comes from `bench/v2.1.0/results-shipping-pretasks`
section 6, whose driver (`tools/multicore_benchmark.py`) runs INSERT /
point-SELECT / UPDATE / DELETE / scan on **independent** relations - there is
no join and no foreign key in it, so nothing in that workload ever crosses a
core. Its collapse is the *denominator*: section 6a says outright that the
multi-core arm is pinned at the device while the single-core arm climbs as
sessions are added. A placement policy cannot move a number that was never
about placement. Re-running that driver under `placement = namespace` would
also change nothing for a second, duller reason: it creates every relation
unqualified, so they all land in `public`, which `AssignOwnerCore` never
rotates.

So this probe measures what AF-4 actually claims, on a workload built to have
the property AF is about: **relations that are wired to each other.**

---- The shape --------------------------------------------------------------

`k` groups. Each group is two relations, joined on every read:

    head_<g>(id int64, tag int64)                 BTREE
    line_<g>(id int64, head_id int64, amt int64)  BTREE

    SELECT h.tag, l.amt FROM head_<g> AS h JOIN line_<g> AS l
        ON l.head_id = h.id WHERE h.id = <i>

One session per group, every session from **one listener on core 0** -
deliberately, so the three arms differ in placement and in nothing else. No
peer listeners, so no kernel accept distribution and no session-core hunting
enters the numbers.

  creating   every relation on core 0. Nothing crosses and nothing runs in
             parallel. The control.
  rotate     relations rotate over the peer cores in creation order, so a
             group's two relations land on **different** cores and every join
             crosses. Blind rotation - the policy DA2 measured.
  namespace  one `CREATE NAMESPACE` per group, both of the group's relations
             created in it, so a group is on one core and different groups are
             on different cores. The same rotation with the grouping supplied.

AF-4's prediction is `namespace > rotate` and `namespace > creating`. What
would falsify AF as an answer to AE-8 is `namespace` no better than
`creating`.

The creation *order* is identical in all three arms - head then line, group by
group - so rotation sees the sequence it always would and the only difference
between the arms is whether a namespace qualifier is written.

**Arms are interleaved** (rep 0: creating, rotate, namespace; rep 1: the same;
...) rather than run one after another, because position in a run carries a
measured ordering bias in this harness family (SS-B finding 10). Each arm gets
a fresh server on its own data file and port, started and stopped inside the
rep.

**Owners are read back from `sys.tables`, never predicted.** Where the
relations went is the thing under test; a prediction that disagreed with the
catalog would be the test asserting its own conclusion. `split_groups` in the
output is how many groups have their two relations on two cores, and it is the
number that says which arm is which.

It measures; it decides nothing. A refusal is reported as a refusal - if a
join across two owner cores is not servable from a core-0 session, that is a
finding and the results file says so rather than dropping the arm.

Usage:
    bench/af_namespace_grouping_probe.py --server build-release/kds_server \
        --workdir ~/mcbench/af --cores 4 --groups 3 --rows 500 --reps 5 \
        --json bench/v2.8.0/archive/af-t5.json
"""

import argparse
import json
import os
import shutil
import statistics
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from multicore_benchmark import (  # noqa: E402
    Conn, DEFAULT_RETRY_DEADLINE_S, check_host, field, is_retryable, start_server,
)

ARMS = ("creating", "rotate", "namespace")
PERCENTILES = (0, 25, 50, 90, 99, 100)


def owner_of(conn, rel):
    """One relation's `owner_core`, from `DESCRIBE`.

    Not from `SELECT owner_core FROM sys.tables`: the view's column list is
    `oid, namespace_oid, name, desc_page_id, clustered_type, next_id`
    (`src/exec/catalog_view.cpp`) and carries no owner. `DESCRIBE` prints
    `owner_core=` in its header line, which is the only client-visible
    spelling of the fact this probe is about - worth knowing, because it
    means an operator cannot query placement, only describe it one relation
    at a time.
    """
    reply = conn.cmd(f"DESCRIBE {rel}")
    if reply.startswith("ERR"):
        raise RuntimeError(f"owner of {rel}: {reply}")
    return field(reply, "owner_core")


def create_groups(conn, groups, arm):
    """The k groups under one arm. Returns {relation: owner_core}."""
    for g in range(groups):
        if arm == "namespace":
            reply = conn.cmd(f"CREATE NAMESPACE g{g}")
            if not reply.startswith("CREATED"):
                raise RuntimeError(f"CREATE NAMESPACE g{g}: {reply}")
        prefix = f"g{g}." if arm == "namespace" else ""
        for rel, cols in ((f"head_{g}", "id int64, tag int64"),
                          (f"line_{g}", "id int64, head_id int64, amt int64")):
            reply = conn.cmd(f"CREATE TABLE {prefix}{rel} ({cols}) BTREE")
            if not reply.startswith("CREATED"):
                raise RuntimeError(f"CREATE TABLE {prefix}{rel}: {reply}")

    owners = {}
    for g in range(groups):
        for rel in (f"head_{g}", f"line_{g}"):
            owners[rel] = owner_of(conn, rel)
    return owners


def retrying(conn, stmt, deadline_s):
    """One statement, retried while the engine says retry, bounded in wall
    clock - `multicore_benchmark.timed`'s contract without its Phase object.

    The row-id lease exhaustion a peer answers on a relation's first INSERT
    (PW1b) is retryable, and letting it into this probe's error list would
    make the two rotating arms read as refusals when they are waits. A
    **non**-retryable refusal comes back as it is: this probe reports
    refusals, it does not spin on them.
    """
    end = time.time() + deadline_s
    backoff = 0.0005
    while True:
        reply = conn.cmd(stmt)
        if not is_retryable(reply) or time.time() >= end:
            return reply
        time.sleep(backoff)
        backoff = min(backoff * 2, 0.05)


def group_worker(port, g, rows, barrier, out, lock, retry_deadline_s):
    """One session, one group: load outside the timed window, then join every
    row inside it. Errors are collected, never retried away - a refusal on
    this path is the finding."""
    latencies = []
    errors = []
    load_s = 0.0
    join_s = 0.0
    conn = Conn(port)
    load_start = time.time()
    try:
        # **The primary key is omitted, and it has to be.** A caller-supplied
        # pk cannot be written on a peer core (`workplan-peer-writer.md`
        # section 7a: admitting one writes the relation's catalog row, which
        # is the system core's page), so a probe that supplied ids would
        # measure that refusal in two of its three arms and nothing else. An
        # omitted key is issued by the owner and comes back 1..rows in issue
        # order, which is what makes `line.head_id = i` name `head.id = i`.
        for i in range(1, rows + 1):
            for stmt in (f"INSERT INTO head_{g} VALUES ({i * 3})",
                         f"INSERT INTO line_{g} VALUES ({i}, {i * 7})"):
                reply = retrying(conn, stmt, retry_deadline_s)
                if reply.startswith("ERR"):
                    errors.append(f"load: {stmt} -> {reply}")
                    if len(errors) > 4:
                        break
            if len(errors) > 4:
                break
        load_s = time.time() - load_start
        barrier.wait()
        join_start = time.perf_counter()
        for i in range(1, rows + 1):
            stmt = (f"SELECT h.tag, l.amt FROM head_{g} AS h JOIN line_{g} AS l "
                    f"ON l.head_id = h.id WHERE h.id = {i}")
            t0 = time.perf_counter()
            reply = retrying(conn, stmt, retry_deadline_s)
            latencies.append(time.perf_counter() - t0)
            if reply.startswith("ERR"):
                errors.append(f"join: {reply}")
                if len(errors) > 4:
                    break
        join_s = time.perf_counter() - join_start
    except Exception as exc:  # reported, never swallowed
        errors.append(f"{type(exc).__name__}: {exc}")
    finally:
        conn.close()
    with lock:
        out["latencies"].extend(latencies)
        out["errors"].extend(errors)
        out["load_s"].append(load_s)
        out["join_s"].append(join_s)


def max_sessions_per_core(owners, groups):
    """The busiest owner core's session count.

    One session drives one group, so a core's session count is how many
    *distinct groups* have a relation on it - not how many relations it
    holds. Under `namespace` a group's two relations sit on one core, so
    seven groups over seven cores give every core exactly one session; under
    `rotate` the same seven groups give every core two, because its two
    relations come from two different groups. That difference is invisible
    in `distinct_owner_cores`, and it is what `group` durability batches
    over.
    """
    per_core = {}
    for g in range(groups):
        for rel in (f"head_{g}", f"line_{g}"):
            per_core.setdefault(owners[rel], set()).add(g)
    return max((len(v) for v in per_core.values()), default=0)


def percentiles(latencies):
    if not latencies:
        return {}
    ordered = sorted(latencies)
    out = {}
    for p in PERCENTILES:
        idx = min(len(ordered) - 1, int(len(ordered) * p / 100))
        out[f"p{p}_us"] = ordered[idx] * 1e6
    return out


def run_arm(args, arm, rep, port):
    tag = f"af-{arm}-{rep}"
    proc = start_server(args.server, args.workdir, tag, args.cores, port, placement=arm,
                        durability=args.durability)
    try:
        setup = Conn(port)
        try:
            owners = create_groups(setup, args.groups, arm)
        finally:
            setup.close()

        out = {"latencies": [], "errors": [], "load_s": [], "join_s": []}
        lock = threading.Lock()
        barrier = threading.Barrier(args.groups)
        threads = [threading.Thread(target=group_worker,
                                    args=(port, g, args.rows, barrier, out, lock,
                                          args.retry_deadline))
                   for g in range(args.groups)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        # **The window is the slowest group's join phase, not the process's
        # wall clock.** The load runs before the barrier and is not in it,
        # and `max` rather than a mean is `bench/v2.1.0` section 6's own
        # reasoning: the run finishes when the busiest core does, and an
        # average over idle neighbours describes nobody.
        wall = max(out["join_s"]) if out["join_s"] else 0.0

        split = sum(1 for g in range(args.groups)
                    if owners[f"head_{g}"] != owners[f"line_{g}"])
        joins = args.groups * args.rows
        return {
            "rep": rep, "arm": arm, "wall_s": wall, "joins": joins,
            "throughput_joins_s": joins / wall if wall > 0 else 0.0,
            # The write half, reported beside the read half rather than
            # folded into it: a load runs before the barrier, so it is not
            # in `wall`, and under two of the three arms it is a shipped
            # write.
            "load_max_s": max(out["load_s"]) if out["load_s"] else 0.0,
            "durability": args.durability or "group (server default)",
            # How many of this arm's sessions land on one owner core -
            # the number the load time turns out to depend on. Under
            # `group` a core with one session has no committer to batch
            # with.
            "max_sessions_per_owner_core": max_sessions_per_core(owners, args.groups),
            "owners": owners,
            "groups_split_across_cores": split,
            "distinct_owner_cores": len(set(owners.values())),
            "errors": out["errors"][:5],
            "error_count": len(out["errors"]),
            **percentiles(out["latencies"]),
        }
    finally:
        proc.terminate()
        proc.wait()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="build-release/kds_server")
    ap.add_argument("--workdir", default=os.path.expanduser("~/mcbench/af"))
    ap.add_argument("--cores", type=int, default=4)
    ap.add_argument("--groups", type=int, default=3)
    ap.add_argument("--rows", type=int, default=500)
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--port", type=int, default=15480)
    ap.add_argument("--json", default="")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--durability", default="",
                    help="durability class for every arm (`strict`/`group`/`relaxed`); "
                         "empty leaves the server default, which is `group`. Why it "
                         "exists: under `group` a commit's fsync is amortised over the "
                         "concurrent committers *on that core*, and a batch of one is a "
                         "batch (kds.conf.sample) - so a placement putting one session on "
                         "a core pays a sync per commit where one putting two pays a sync "
                         "per two. Running an arm under `relaxed` removes the per-commit "
                         "sync and is how that is told apart from anything else.")
    ap.add_argument("--retry-deadline", type=float, default=DEFAULT_RETRY_DEADLINE_S)
    args = ap.parse_args()

    # A fresh tree per invocation, `multicore_benchmark`'s own rule: `cores`
    # is pinned into the superblock at bootstrap and a reused data file
    # would be mounted rather than created, so the arm would run against the
    # *previous* invocation's relations and placement.
    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir, exist_ok=True)
    check_host(args.workdir, args.force)
    print(f"AF-T5: {args.groups} groups x 2 relations, cores={args.cores}, "
          f"rows={args.rows}, reps={args.reps}, arms interleaved")
    print("(the docstring says why the DA2 cell itself cannot answer AF-T5)")

    runs = []
    for rep in range(args.reps):
        for arm_no, arm in enumerate(ARMS):
            r = run_arm(args, arm, rep, args.port + arm_no)
            runs.append(r)
            print(f"  rep {rep} {arm:9s} wall={r['wall_s']:7.3f}s "
                  f"{r['throughput_joins_s']:8.1f} join/s  "
                  f"p50={r.get('p50_us', 0):8.1f}us  load={r['load_max_s']:6.2f}s  "
                  f"split={r['groups_split_across_cores']}/{args.groups} "
                  f"cores={r['distinct_owner_cores']} "
                  f"sess/core={r['max_sessions_per_owner_core']}"
                  + (f"  ERRORS={r['error_count']}" if r["error_count"] else ""))
            for e in r["errors"]:
                print(f"      {e}")

    print()
    header = (f"{'arm':10s} {'median join/s':>14s} {'min':>9s} {'max':>9s} "
              + " ".join(f"{'p' + str(p):>9s}" for p in PERCENTILES)
              + f" {'split':>6s} {'cores':>6s} {'errors':>7s}")
    print(header)
    summary = {}
    for arm in ARMS:
        arm_runs = [r for r in runs if r["arm"] == arm]
        if not arm_runs:
            continue
        rates = [r["throughput_joins_s"] for r in arm_runs]
        s = {
            "median_joins_s": statistics.median(rates),
            "min_joins_s": min(rates),
            "max_joins_s": max(rates),
            "groups_split_across_cores": arm_runs[0]["groups_split_across_cores"],
            "distinct_owner_cores": arm_runs[0]["distinct_owner_cores"],
            "errors": sum(r["error_count"] for r in arm_runs),
        }
        for p in PERCENTILES:
            vals = [r[f"p{p}_us"] for r in arm_runs if f"p{p}_us" in r]
            s[f"p{p}_us"] = statistics.median(vals) if vals else 0.0
        summary[arm] = s
        print(f"{arm:10s} {s['median_joins_s']:14.1f} {s['min_joins_s']:9.1f} "
              f"{s['max_joins_s']:9.1f} "
              + " ".join(f"{s[f'p{p}_us']:9.1f}" for p in PERCENTILES)
              + f" {s['groups_split_across_cores']:6d} {s['distinct_owner_cores']:6d} "
                f"{s['errors']:7d}")

    if "creating" in summary:
        base = summary["creating"]["median_joins_s"]
        print()
        for arm in ("rotate", "namespace"):
            if arm in summary and base > 0:
                print(f"  {arm} / creating = {summary[arm]['median_joins_s'] / base:.3f}x")
        if "rotate" in summary and summary["rotate"]["median_joins_s"] > 0:
            print(f"  namespace / rotate   = "
                  f"{summary['namespace']['median_joins_s'] / summary['rotate']['median_joins_s']:.3f}x")

    if args.json:
        os.makedirs(os.path.dirname(os.path.abspath(args.json)), exist_ok=True)
        with open(args.json, "w") as f:
            json.dump({"config": vars(args), "runs": runs, "summary": summary}, f, indent=2)
        print(f"\nwrote {args.json}")


if __name__ == "__main__":
    main()
