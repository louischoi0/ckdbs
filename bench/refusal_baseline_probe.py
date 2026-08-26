#!/usr/bin/env python3
"""T5 - the before-shipping reading of the cross-core write refusal counters.

`docs/spec/crosscore.md` §6 specifies a per-core counter keyed (home core, target
core, relation) and calls it *"the input the future placement/2PC decision
will be made from"*. One instrument, two eras: read **now**, it says how
often today's engine refuses a write because the session is on the wrong
core; read **after** statement shipping lands, the same counter reports only
the residue shipping cannot convert - a genuine multi-core transaction, which
is what 2PC would have to be designed for.

The reading needs a workload that does *not* route around the restriction.
Every driver in `bench/` hunts for a session on the relation's owner core
(`tools/multicore_benchmark.py`'s `collect_connections`) because otherwise it
could not write at all - so every one of them reports zero refusals by
construction. This one deliberately takes the sessions the kernel gives it
and writes wherever the workload says, which is what an application that has
never heard of core placement does.

It reports:

  * the refusal rate over write statements, and the refusal's class by
    message (CC3's cross-core refusal, PW1b's retryable lease refill, the
    owner-core grant/index windows);
  * `SHOW META`'s counters read from **every core**, since the counter is
    core-local and a total needs all of them;
  * the counted total against the driver's own count of CC3 refusals, which
    is what makes the stated undercount checkable rather than asserted.

Usage:
    bench/refusal_baseline_probe.py --server build-release/kds_server \\
        --workdir ~/mcbench2/t5 --cores 4 --sessions 8 --tables 6 --rows 200
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
from multicore_benchmark import (  # noqa: E402
    Conn, check_host, collect_connections, field, is_retryable, wait_for_port,
)

# The refusal classes a write can meet on this engine, by the phrase each
# message carries (core_affinity.cpp). Kept as text because that is what a
# client sees; the engine-side counter is read separately from SHOW META and
# the two are compared in the report.
CLASSES = {
    "cross_core_cc3": "writes are bound to core",
    "rights_pending": "write rights are not held here",
    "index_build_window": "takes no writes until that CREATE INDEX ends",
    "lease_refill": "retry after the refill grant lands",
    "lease_exhausted": "a refill must be granted before it can allocate again",
    "peer_ddl": "takes no DDL",
    # Post-shipping classes (SS-B5's residue reading). The first is the read
    # half of the same restriction; the second and third are shipping's own
    # bounds, and both are `UnknownOutcome` rather than a refusal - a
    # statement that ran and could not be reported.
    "cross_core_read": "cross-core reads need the step pipeline",
    "shipped_reply_overlong": "its reply is",
    "shipped_unknown": "unknown outcome",
}


def classify(reply):
    for name, phrase in CLASSES.items():
        if phrase in reply:
            return name
    return "other"


class Writer(threading.Thread):
    """One session, writing round-robin over every relation - including the
    ones its core does not own, which is the point."""

    def __init__(self, conn, core, tables, rows, barrier):
        super().__init__()
        self.conn = conn
        self.core = core
        self.tables = tables
        self.rows = rows
        self.barrier = barrier
        self.attempts = 0
        self.ok = 0
        self.by_class = {}
        self.first_other = None

    def run(self):
        self.barrier.wait()
        for i in range(self.rows):
            table = self.tables[i % len(self.tables)]
            r = self.conn.cmd(f"INSERT INTO {table} VALUES ('c{self.core}', {i})")
            self.attempts += 1
            if not r.startswith("ERR"):
                self.ok += 1
                continue
            name = classify(r)
            self.by_class[name] = self.by_class.get(name, 0) + 1
            if name == "other" and self.first_other is None:
                self.first_other = r


def residue_phase(conns, names, owners, args):
    """The shapes shipping declines, run deliberately so the residue is a
    distribution rather than a total.

    Each `(conn, core)` runs each shape `--residue-reps` times against a
    relation its own core does not own, and the reply is classified. Six
    shapes, each naming what puts it outside D1:

      autocommit_write   the control - in scope, must convert.
      autocommit_read    the read half of the same scope.
      in_explicit_txn    D1's first exclusion: BEGIN, then a foreign write.
      two_owner_read     D1's second: a join whose two relations have two
                         owners, which `SoleForeignOwner` declines.
      subquery_write     a write whose predicate names a second relation;
                         the fork skips it whatever the owners are
                         (`AnySubqueryPredicate`, command_dispatcher.cpp).
      overlong_read      in scope and shippable, and the answer does not
                         fit the ring's 992 bytes.

    Nothing here is retried: a residue statement's *class* is the
    measurement and a retry would only re-collect it.
    """
    # Two relations with different owners, for the shapes that need one.
    pairs = {}
    for a in names:
        for b in names:
            if a != b and owners[a] != owners[b]:
                pairs[(owners[a], owners[b])] = (a, b)
    by_shape = {}
    samples = {}
    for conn, core in conns:
        foreign = [n for n in names if owners[n] != core]
        if not foreign:
            continue
        t = foreign[0]
        other = next((n for n in foreign if owners[n] != owners[t]), None)
        stmts = [
            ("autocommit_write", [f"INSERT INTO {t} VALUES ('res', 1)"]),
            ("autocommit_read", [f"SELECT * FROM {t} LIMIT 2"]),
            ("in_explicit_txn", ["BEGIN", f"INSERT INTO {t} VALUES ('res', 2)",
                                 "ROLLBACK"]),
            ("overlong_read", [f"SELECT * FROM {t} LIMIT {args.residue_limit}"]),
        ]
        if other is not None:
            stmts.append(("two_owner_read",
                          [f"SELECT {t}.balance FROM {t} JOIN {other} "
                           f"ON {other}.id = {t}.id"]))
            stmts.append(("subquery_write",
                          [f"UPDATE {t} SET balance = 0 WHERE balance IN "
                           f"(SELECT balance FROM {other} WHERE id = 999999999)"]))
        for shape, lines in stmts:
            for _ in range(args.residue_reps):
                verdict = None
                for line in lines:
                    r = conn.cmd(line)
                    # The verdict is the first non-OK reply in the sequence:
                    # a BEGIN that succeeds and an INSERT that refuses is one
                    # refused statement, not one of each.
                    if r.startswith("ERR") and verdict is None:
                        verdict = r
                if verdict is None:
                    name = "accepted"
                else:
                    name = classify(verdict)
                    samples.setdefault(f"{shape}/{name}", verdict[:220])
                by_shape.setdefault(shape, {})
                by_shape[shape][name] = by_shape[shape].get(name, 0) + 1
    return dict(by_shape=by_shape, samples=samples,
                reps_per_session=args.residue_reps,
                overlong_limit=args.residue_limit)


def meta_counters(meta):
    out = {}
    for tok in meta.split():
        if tok.startswith("cross_core_write_refusal") and "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="build-release/kds_server")
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--cores", type=int, default=4)
    ap.add_argument("--tables", type=int, default=6)
    ap.add_argument("--sessions", type=int, default=8)
    ap.add_argument("--rows", type=int, default=200,
                    help="write statements per session")
    ap.add_argument("--port", type=int, default=18600)
    ap.add_argument("--max-connects", type=int, default=512)
    ap.add_argument("--json", default="")
    ap.add_argument("--residue", action="store_true",
                    help="after the unchanged run, exercise the shapes shipping declines "
                         "and report their classes (SS-B5's residue reading)")
    ap.add_argument("--residue-reps", type=int, default=5,
                    help="times each session runs each residue shape")
    ap.add_argument("--residue-limit", type=int, default=200,
                    help="LIMIT of the overlong_read shape; the reply must exceed the "
                         "ring's 992-byte payload for the cell to mean anything")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    check_host(args.workdir, args.force)
    workdir = os.path.join(args.workdir, f"c{args.cores}-s{args.sessions}")
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
    out = dict(cores=args.cores, tables=args.tables, sessions=args.sessions,
               rows_per_session=args.rows)
    try:
        wait_for_port(args.port, stderr_path)
        got, _ = collect_connections(args.port, {0: 1}, args.max_connects)
        setup = got[0][0]
        names = [f"r{i}" for i in range(args.tables)]
        owners = {}
        for n in names:
            r = setup.cmd(f"CREATE TABLE {n} (id int64, owner varchar, balance int64) BTREE")
            if r.startswith("ERR"):
                raise RuntimeError(f"{n}: {r}")
            owners[n] = int(field(setup.cmd(f"DESCRIBE {n}"), "owner_core"))
        out["owner_cores"] = owners

        # Sessions as the kernel gives them - no hunting. That is the whole
        # difference between this probe and every other driver here.
        conns = []
        for _ in range(args.sessions):
            c = Conn(args.port)
            conns.append((c, int(field(c.cmd("SHOW META"), "core"))))
        out["session_cores"] = [core for _c, core in conns]

        barrier = threading.Barrier(args.sessions)
        writers = [Writer(c, core, names, args.rows, barrier) for c, core in conns]
        t0 = time.perf_counter()
        for w in writers:
            w.start()
        for w in writers:
            w.join()
        out["wall_s"] = round(time.perf_counter() - t0, 4)

        attempts = sum(w.attempts for w in writers)
        ok = sum(w.ok for w in writers)
        by_class = {}
        for w in writers:
            for k, v in w.by_class.items():
                by_class[k] = by_class.get(k, 0) + v
        out.update(attempts=attempts, accepted=ok, refused=attempts - ok,
                   refusal_rate=round((attempts - ok) / attempts, 4) if attempts else None,
                   by_class=by_class,
                   first_other=next((w.first_other for w in writers if w.first_other), None))

        # ---- The residue, by shape (SS-B5's second reading) -------------
        #
        # The workload above is autocommit, single-relation and one
        # statement, which is exactly D1's shipping scope - so after
        # shipping it converts whole and leaves nothing to distribute.
        # The population a 2PC decision has to be designed from is the one
        # shipping deliberately does **not** carry, and it does not appear
        # in a workload that never asks for it. `--residue` asks: each
        # unrouted session runs each out-of-scope shape and the reply is
        # classified. This is an *added* shape mix and is reported apart
        # from the unchanged run above, never folded into its rate.
        if args.residue:
            out["residue"] = residue_phase(conns, names, owners, args)

        # The engine's own counters, from every core: the counter is
        # core-local, so a total needs one reading per core.
        per_core, _ = collect_connections(args.port, {c: 1 for c in range(args.cores)},
                                          args.max_connects)
        engine = {}
        for core, cs in sorted(per_core.items()):
            engine[f"core{core}"] = meta_counters(cs[0].cmd("SHOW META"))
            cs[0].close()
        out["engine_counters"] = engine
        counted = sum(int(v.get("cross_core_write_refusals", 0))
                      for v in engine.values())
        out["engine_counted_total"] = counted
        out["driver_cc3_refusals"] = by_class.get("cross_core_cc3", 0)
        # The check the undercount comment invites: the two must agree on
        # CC3's class, and any gap is a class the counter cannot see.
        out["counter_agrees_with_driver"] = (counted == by_class.get("cross_core_cc3", 0))

        for c, _core in conns:
            c.close()
        setup.cmd("STOP")
        setup.close()
    finally:
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.terminate()
            try:
                proc.wait(timeout=15)
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
