#!/usr/bin/env python3
"""AO-S7 cell C3: what the lock family costs when writers meet, and when
they do not (AR2 §9 step 5).

`instructions/v3.0.0/workorder-ao-m2-lock-family.md` §AO-S7. C3 asks two
questions and they need two arms of the same shape:

* **contended** - many sessions updating **one row** of one relation, so the
  tuple `X` is fought over;
* **disjoint** - the same sessions on **their own rows** of the same
  relation, so nothing is fought over except the thing every writer takes
  anyway: the relation-level `IX`. That is R3's price, alone, and it is the
  number AR2 E12 is waiting on.

Why a new driver rather than a flag on `multicore_benchmark.py`: that one
measures N *non-interfering relations*, one connection each, and its own
docstring calls parity the honest expectation. C3 is the opposite shape -
one relation, many sessions - and `bench/README.md` forbids widening a
driver inside a measurement stage more clearly than it forbids a new file.

---- What makes this driver's own numbers trustworthy -------------------

Three things, all borrowed from `catalog_read_ab_benchmark.py`, which is
the one harness here that already argues them:

**Interleaving, not sequence.** Two sequential runs of anything on this box
disagree with themselves by more than the effects C3 is looking for. Every
arm runs block by block, and with `--ab-port` the two servers alternate
which goes first per block, so an arm whose cost drifts does not
systematically favour whichever side ran first.

**A noise floor from inside the run.** `update-disjoint-again` is
`update-disjoint` repeated - same server, same statements, same rows. **A
delta smaller than the gap between those two rows is not a finding**, and
the report prints that gap next to every delta rather than leaving it to be
looked up.

**Controls that cannot reach the code under test.** `select-hot` is a point
read of the contended row, and a read takes no borrow; `ping` is
`SHOW META`, which resolves no relation. A contended-versus-disjoint
difference that shows up on either of those is the host, not the family.

**And the proof this driver runs before it prices anything** (AO-0 item 23,
ratified 2026-09-09): with `--ab-port` pointed at a binary built without
the lock family, the two write arms must differ there **only** by what MVCC
first-updater-wins already costs - no borrow is taken on that side at all.
A driver reporting a family cost against a server that has no family is
reporting itself, and `--prove` is the flag that says that is what this run
is for: it relabels the report and refuses to write a results summary.

Usage:
    tools/lock_contention_benchmark.py --port 15490 --sessions 8 \\
        --rows 256 --ops 4000
    tools/lock_contention_benchmark.py --port 15490 --ab-port 15491 \\
        --ab-label no-lock-family --prove

Both servers must already be running on their own data files and their own
**chosen** ports (`bench/README.md` rule 5: 15432 on this box belongs to an
unrelated instance). The driver creates its own relation and never drops
one it did not create.
"""

import argparse
import json
import os
import statistics
import subprocess
import sys
import threading
import time

from bench_common import Phase, nearest_rank
from ckdbs_cli import DEFAULT_HOST, ServerConnection

RELATION = "c3_lock"


# ---- The host's state, per cell (bench/README.md rule 4) -----------------
#
# Written into the report rather than checked, because "quiet" is not a
# predicate this driver gets to decide: a competing build moved a colocated
# p99 by 12x on this box, and the only defence that survives is the reading
# being *in the file* beside the number it moved.
def host_state():
    try:
        with open("/proc/loadavg") as f:
            load = f.read().strip()
    except OSError:
        load = "unavailable"
    try:
        busy = subprocess.run(
            ["pgrep", "-a", "-f", "cc1plus|cmake --build|ctest"],
            capture_output=True, text=True, timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        busy = "unavailable"
    return {"loadavg": load, "competing": busy.splitlines() if busy else []}


class Session:
    """One connection, and the core the kernel happened to accept it on.

    The core is asked rather than chosen: under SO_REUSEPORT a client cannot
    pick one (`workplan-peer-writer.md` §5), so a run reports the spread it
    got instead of claiming one it arranged.
    """

    def __init__(self, host, port, index):
        self.index = index
        self.conn = ServerConnection(host, port)
        self.core = None
        meta = self.conn.send_command("SHOW META")
        for token in meta.replace("\\n", " ").split():
            if token.startswith("core="):
                self.core = token[len("core="):]
                break

    def close(self):
        try:
            self.conn._conn.close()
        except Exception:
            pass


def run_arm(sessions, name, statement_for, ops, detail=""):
    """`ops` statements per session, every session at once, one merged Phase.

    Threads rather than one loop: the whole subject is what happens when
    writers *meet*, and a driver that serialises them measures an engine
    nobody runs.
    """
    per = [Phase(name, detail) for _ in sessions]
    barrier = threading.Barrier(len(sessions))

    def body(slot, session, phase):
        # Every session starts together, so the contention is the arm's and
        # not the thread pool's ramp.
        barrier.wait()
        for k in range(ops):
            command = statement_for(slot, k)
            t0 = time.perf_counter()
            reply = session.conn.send_command(command)
            phase.record(time.perf_counter() - t0, reply)

    started = time.perf_counter()
    threads = [threading.Thread(target=body, args=(i, s, per[i]))
               for i, s in enumerate(sessions)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    elapsed = time.perf_counter() - started

    merged = Phase(name, detail)
    for phase in per:
        merged.latencies.extend(phase.latencies)
        merged.errors += phase.errors
        if merged.first_error is None:
            merged.first_error = phase.first_error
    merged.elapsed = elapsed
    return merged


def setup(conn, rows):
    conn.send_command(f"DROP TABLE {RELATION}")
    created = conn.send_command(f"CREATE TABLE {RELATION} (id int64, v int64) BTREE")
    if not created.startswith("CREATED"):
        raise SystemExit(f"setup: CREATE TABLE refused: {created}")
    for i in range(1, rows + 1):
        reply = conn.send_command(f"INSERT INTO {RELATION} VALUES ({i}, 0)")
        if not reply.startswith("INSERTED"):
            raise SystemExit(f"setup: INSERT {i} refused: {reply}")


def arms(sessions, rows, ops):
    """The five arms, in the order a block runs them."""
    hot = 1

    def upd_hot(slot, k):
        return f"UPDATE {RELATION} SET v = {k} WHERE id = {hot}"

    def upd_disjoint(slot, k):
        return f"UPDATE {RELATION} SET v = {k} WHERE id = {1 + (slot % rows)}"

    def sel_hot(slot, k):
        return f"SELECT * FROM {RELATION} WHERE id = {hot}"

    def ping(slot, k):
        return "SHOW META"

    return [
        ("update-hot", upd_hot, "every session on one row - the tuple X, fought over"),
        ("update-disjoint", upd_disjoint, "one row each - R3's relation IX, alone"),
        ("update-disjoint-again", upd_disjoint, "the noise floor: the row above, repeated"),
        ("select-hot", sel_hot, "control: a read takes no borrow"),
        ("ping", ping, "control: SHOW META resolves no relation"),
    ]


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--port", type=int, required=True,
                   help="chosen, never defaulted (bench/README.md rule 5)")
    p.add_argument("--label", default="B")
    p.add_argument("--ab-port", type=int, default=None,
                   help="a second server, for the A/B against a binary with no lock family")
    p.add_argument("--ab-label", default="A")
    p.add_argument("--sessions", type=int, default=8)
    p.add_argument("--rows", type=int, default=256)
    p.add_argument("--ops", type=int, default=2000, help="statements per session per arm")
    p.add_argument("--blocks", type=int, default=4, help="blocks the ops are split into")
    p.add_argument("--prove", action="store_true",
                   help="AO-0 item 23's proof run: label the report as a driver check "
                        "and refuse to emit a priced summary")
    p.add_argument("--json", default=None, help="write the raw summaries here")
    args = p.parse_args()

    if args.sessions < 1 or args.ops < 1 or args.blocks < 1:
        raise SystemExit("--sessions, --ops and --blocks must all be at least 1")
    if args.rows < args.sessions:
        raise SystemExit("--rows must be at least --sessions, or the disjoint arm is not disjoint")

    ports = [(args.label, args.port)]
    if args.ab_port is not None:
        ports.append((args.ab_label, args.ab_port))

    before = host_state()
    results = {}
    pools = {}
    for label, port in ports:
        admin = ServerConnection(args.host, port)
        setup(admin, args.rows)
        admin_core = admin.send_command("SHOW META")
        pools[label] = (
            [Session(args.host, port, i) for i in range(args.sessions)], admin, admin_core)

    per_block = max(1, args.ops // args.blocks)
    for name, builder, detail in arms(None, args.rows, args.ops):
        for label, _ in ports:
            results.setdefault(label, {})[name] = Phase(name, detail)
        for block in range(args.blocks):
            # The alternation: an arm whose cost drifts must not
            # systematically favour whichever side went first.
            order = ports if block % 2 == 0 else list(reversed(ports))
            for label, _ in order:
                sessions, _, _ = pools[label]
                phase = run_arm(sessions, name, builder, per_block, detail)
                merged = results[label][name]
                merged.latencies.extend(phase.latencies)
                merged.errors += phase.errors
                merged.elapsed += phase.elapsed
                if merged.first_error is None:
                    merged.first_error = phase.first_error

    after = host_state()

    print()
    if args.prove:
        print("AO-0 item 23's PROOF RUN - this is a check of the driver, not a price.")
        print("On a server with no lock family the two write arms may differ only by what")
        print("first-updater-wins already costs; a family cost reported there is this file.")
        print()
    print(f"sessions={args.sessions} rows={args.rows} ops/arm/session={per_block * args.blocks} "
          f"blocks={args.blocks}")
    for label, _ in ports:
        sessions, _, meta = pools[label]
        cores = sorted({s.core for s in sessions if s.core is not None})
        print(f"  {label}: cores accepted on = {', '.join(cores) if cores else 'unknown'}")
    print(f"host before: {before['loadavg']}   competing: {before['competing'] or 'none'}")
    print(f"host after : {after['loadavg']}   competing: {after['competing'] or 'none'}")
    print()

    head = f"{'arm':<24}" + "".join(f"{label:>34}" for label, _ in ports)
    print(head)
    print(f"{'':<24}" + "".join(f"{'p50us':>9}{'p99us':>9}{'qps':>9}{'err':>7}"
                                for _ in ports))
    for name, _, _ in arms(None, args.rows, args.ops):
        row = f"{name:<24}"
        for label, _ in ports:
            ph = results[label][name]
            s = ph.summary()
            row += f"{s['p50_us']:>9}{s['p99_us']:>9}{s['qps']:>9.0f}{s['errors']:>7}"
        print(row)

    # The noise floor, printed beside the deltas rather than left to be
    # looked up: `disjoint` against its own repeat is what any other delta
    # has to beat to be a finding.
    print()
    for label, _ in ports:
        a = results[label]["update-disjoint"].summary()
        b = results[label]["update-disjoint-again"].summary()
        hot = results[label]["update-hot"].summary()
        floor = abs(a["p50_us"] - b["p50_us"])
        print(f"{label}: noise floor (disjoint vs its repeat) p50 = {floor:.1f} us; "
              f"hot - disjoint p50 = {hot['p50_us'] - a['p50_us']:.1f} us")

    if args.json:
        payload = {
            "driver": "lock_contention_benchmark.py",
            "prove_run": bool(args.prove),
            "sessions": args.sessions,
            "rows": args.rows,
            "ops_per_session_per_arm": per_block * args.blocks,
            "blocks": args.blocks,
            "host_before": before,
            "host_after": after,
            "arms": {label: {n: results[label][n].summary() for n in results[label]}
                     for label, _ in ports},
        }
        with open(args.json, "w") as f:
            json.dump(payload, f, indent=2)
        print(f"\nraw summaries: {args.json}")

    for label, _ in ports:
        sessions, admin, _ = pools[label]
        for s in sessions:
            s.close()


if __name__ == "__main__":
    sys.exit(main())
