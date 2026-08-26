#!/usr/bin/env python3
"""T1a - transaction-wrapped bulk insert: what one commit per `--batch` rows
does to the per-core group-commit law.

`bench/v2.1.0` §6 measured every writer core running at 965-1,071 commits/s,
the volume's single-stream `fdatasync` rate, and explained the whole matrix
with `1000 x writer_cores / (470 x sessions)`. That law is about **commits**,
and the harness that produced it issues one autocommit statement per row - so
one commit per row. A transaction of N rows issues one commit per N rows and
steps outside the law entirely.

This probe runs the same relations and the same rows with each session's
INSERTs wrapped in explicit transactions of `--batch` rows, and reports the
`cores = 1` against `cores = N` ratio per batch size. It measures; it decides
nothing, and it proposes no constant.

Three things it does that a naive driver would get wrong, each learned from
`bench/v2.1.0` §2:

* **The Keystone pk is implicit.** `INSERT INTO t (cols) VALUES (...)` is
  refused engine-wide (`tools/multicore_benchmark.py:288`), so every insert
  here names values only.
* **A peer's first INSERT is refused retryably** until its row-id refill
  grant lands (PW1b), and the trx-id and extent leases refuse the same way
  when spent. Inside an explicit transaction those refusals **poison** the
  transaction (`command_dispatcher.cpp`'s write-capability refusals), so the
  retry unit is the whole batch: ROLLBACK, then run it again. Batch retries
  are counted and reported, never folded into the row denominator.
* **Rows that landed, not rows intended**, divide the throughput, and every
  relation's final `COUNT(*)` is checked against what was asked for.

The commit latency is recorded as its own phase, because it is the one this
cell exists to look at: a batch's `fdatasync` is paid there, and the insert
percentiles alone would hide it.

Usage:
    bench/txn_batch_probe.py --server build-release/kds_server \
        --workdir ~/mcbench/t1a --cores 4 --batch 100 --rows 2000
"""

import argparse
import collections
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

# One statement's worth of values; the pk column is engine-issued and absent.
INSERT_FMT = "INSERT INTO {t} VALUES ('u{i}', {b})"


class BatchWorker(threading.Thread):
    """One relation's rows, in transactions of `batch`.

    A batch is atomic to the engine and therefore atomic to this worker: a
    retryable refusal anywhere inside it rolls the whole batch back and runs
    it again, because a poisoned transaction admits nothing but ROLLBACK
    (manual/sql/sql.md). That makes the retry unit the batch, not the row,
    and it is why `batch_retries` is reported beside the row count rather
    than inside it.
    """

    def __init__(self, conn, table, rows, batch, deadline_s=20.0):
        super().__init__()
        self.conn = conn
        self.table = table
        self.rows = rows
        self.batch = batch
        self.deadline_s = deadline_s
        self.insert_lat = []
        self.commit_lat = []
        self.inserted = 0
        self.batches = 0
        self.batch_retries = 0
        self.errors = 0
        self.first_error = None
        self.seconds = 0.0
        self.start_barrier = None

    def _fail(self, reply):
        self.errors += 1
        if self.first_error is None:
            self.first_error = reply

    def _run_batch(self, first_i, n):
        """One transaction of `n` rows. Returns rows committed (0 on a
        give-up). The latencies of a rolled-back attempt are discarded: they
        are not a statement anybody's row was written by, and keeping them
        would put a retry's cost into a percentile that claims to describe
        committed work. The retry count is what carries that cost."""
        end = time.time() + self.deadline_s
        while True:
            attempt_inserts = []
            r = self.conn.cmd("BEGIN")
            if r.startswith("ERR"):
                self._fail(r)
                return 0
            failed = None
            for k in range(n):
                i = first_i + k
                t0 = time.perf_counter()
                r = self.conn.cmd(INSERT_FMT.format(t=self.table, i=i, b=i * 10))
                dt = time.perf_counter() - t0
                if r.startswith("ERR"):
                    failed = r
                    break
                attempt_inserts.append(dt)
            if failed is None:
                t0 = time.perf_counter()
                r = self.conn.cmd("COMMIT")
                cdt = time.perf_counter() - t0
                if not r.startswith("ERR"):
                    self.insert_lat.extend(attempt_inserts)
                    self.commit_lat.append(cdt)
                    return n
                failed = r
            # Failed, or committed with an error: unwind and decide.
            rb = self.conn.cmd("ROLLBACK")
            if rb.startswith("ERR") and "no transaction is open" not in rb:
                self._fail(rb)
                return 0
            if is_retryable(failed) and time.time() < end:
                self.batch_retries += 1
                time.sleep(0.0005)
                continue
            self._fail(failed)
            return 0

    def run(self):
        if self.start_barrier is not None:
            self.start_barrier.wait()
        t0 = time.perf_counter()
        i = 0
        while i < self.rows:
            n = min(self.batch, self.rows - i)
            self.inserted += self._run_batch(i, n)
            self.batches += 1
            i += n
        self.seconds = time.perf_counter() - t0
        try:
            self.count_reply = self.conn.cmd(f"SELECT COUNT(*) FROM {self.table}")
        except OSError as e:      # a closed socket is a finding, not a crash
            self.count_reply = f"ERR {e}"
        self.conn.close()


def pct(values, p):
    return nearest_rank(sorted(values), p) * 1e6 if values else None


def count_of(reply):
    """The integer out of `count(*)\\n<n>` as the wire escapes it. None when
    the reply is not a count at all - never a silent 0, which would read as
    'every row lost'."""
    try:
        return int(reply.replace("\\n", "\n").split("\n")[-1].split(",")[-1])
    except (ValueError, AttributeError):
        return None


def run_config(args, cores, placement, listeners, tag, port):
    workdir = os.path.join(args.workdir, tag)
    os.makedirs(workdir, exist_ok=True)
    conf = os.path.join(workdir, f"{tag}.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {os.path.join(workdir, tag + '.db')}\n"
                f"port = {port}\ncores = {cores}\nplacement = {placement}\n"
                f"peer_listeners = {'on' if listeners else 'off'}\n"
                f"log_file = {tag}.log\nlog_dir = {workdir}\nlog_level = warn\n")
    stderr_path = os.path.join(workdir, f"{tag}.stderr")
    with open(stderr_path, "w") as err:
        proc = subprocess.Popen([args.server, "--config", conf],
                                stdout=err, stderr=subprocess.STDOUT)
    result = dict(tag=tag, cores=cores, placement=placement,
                  peer_listeners=listeners, batch=args.batch, rows=args.rows,
                  tables=args.tables)
    try:
        wait_for_port(port, stderr_path)
        # DDL is core 0's (PW4) and under peer listeners the kernel picks the
        # accepting core, so the setup session is found by asking.
        if listeners:
            got, _ = collect_connections(port, {0: 1}, args.max_connects)
            setup = got[0][0]
        else:
            setup = Conn(port)
        names = [f"t1a{i}" for i in range(args.tables)]
        owners = {}
        for name in names:
            r = setup.cmd(f"CREATE TABLE {name} "
                          f"(id int64, owner varchar, balance int64) BTREE")
            if r.startswith("ERR"):
                raise RuntimeError(f"{name}: {r}")
            owners[name] = int(field(setup.cmd(f"DESCRIBE {name}"), "owner_core"))
        result["owner_cores"] = owners

        if listeners:
            needed = collections.Counter(owners.values())
            per_core, attempts = collect_connections(port, needed, args.max_connects)
            writers = {n: per_core[owners[n]].pop() for n in names}
            result["connect_attempts"] = attempts
        else:
            writers = {n: Conn(port) for n in names}
        setup.close()

        # Warm-up, outside the measured window and outside the transactions:
        # one autocommit row per relation, retried, which pays the peer's
        # first-INSERT row-id refill (PW1b) and the extent lease a btree's
        # first page needs. Without it the first *batch* pays them, and a
        # poisoned batch's rollback-and-retry would price the refill as if it
        # were the transaction's own cost.
        warmup_retries = 0
        for name in names:
            end = time.time() + args.retry_deadline
            while True:
                r = writers[name].cmd(f"INSERT INTO {name} VALUES ('warm', 0)")
                if not r.startswith("ERR"):
                    break
                if is_retryable(r) and time.time() < end:
                    warmup_retries += 1
                    time.sleep(0.0005)
                    continue
                raise RuntimeError(f"{name} warm-up: {r}")
        result["warmup_retries"] = warmup_retries

        barrier = threading.Barrier(len(names))
        workers = []
        for name in names:
            w = BatchWorker(writers[name], name, args.rows, args.batch,
                            args.retry_deadline)
            w.start_barrier = barrier
            workers.append(w)
        t0 = time.perf_counter()
        for w in workers:
            w.start()
        for w in workers:
            w.join()
        wall = time.perf_counter() - t0

        inserted = sum(w.inserted for w in workers)
        result.update(
            wall_s=round(wall, 4),
            inserted=inserted,
            # Rows that landed over the wall clock of the whole set: a lost
            # row lowers this, it never flatters the arm that lost it.
            inserts_per_second=round(inserted / wall, 1) if wall else 0.0,
            commits=sum(len(w.commit_lat) for w in workers),
            commits_per_second=round(sum(len(w.commit_lat) for w in workers) / wall, 1)
            if wall else 0.0,
            batch_retries=sum(w.batch_retries for w in workers),
            errors=sum(w.errors for w in workers),
            first_error=next((w.first_error for w in workers if w.first_error), None),
        )
        ins = [x for w in workers for x in w.insert_lat]
        com = [x for w in workers for x in w.commit_lat]
        result["insert_p50_us"] = pct(ins, 50)
        result["insert_p99_us"] = pct(ins, 99)
        result["commit_p50_us"] = pct(com, 50)
        result["commit_p99_us"] = pct(com, 99)

        expected = args.rows + 1        # the warm-up row
        verify = {}
        for w in workers:
            n = count_of(getattr(w, "count_reply", ""))
            if n != expected:
                verify[w.table] = f"expected {expected} got {getattr(w, 'count_reply', '')!r}"
        result["verify"] = verify or "rows as expected"

        stop = Conn(port)
        stop.cmd("STOP")
        stop.close()
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
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="build-release/kds_server")
    ap.add_argument("--workdir", required=True, help="on a block device, never tmpfs")
    ap.add_argument("--cores", type=int, default=4)
    ap.add_argument("--tables", type=int, default=0,
                    help="relations (default: 2 per writer core, which is what "
                         "holds per-core load constant across core counts)")
    ap.add_argument("--rows", type=int, default=2000)
    ap.add_argument("--batch", type=int, default=100)
    ap.add_argument("--port", type=int, default=16100)
    ap.add_argument("--max-connects", type=int, default=512)
    ap.add_argument("--retry-deadline", type=float, default=20.0)
    ap.add_argument("--json", default="")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    if args.tables == 0:
        args.tables = 2 * max(1, args.cores - 1)

    os.makedirs(args.workdir, exist_ok=True)
    check_host(args.workdir, args.force)

    out = dict(cores=args.cores, tables=args.tables, rows=args.rows,
               batch=args.batch)
    out["multi"] = run_config(args, args.cores, "rotate", True, "multi", args.port)
    out["single"] = run_config(args, 1, "creating", False, "single", args.port + 2)
    m, s = out["multi"], out["single"]
    out["ips_ratio"] = (round(m["inserts_per_second"] / s["inserts_per_second"], 4)
                        if s["inserts_per_second"] else None)
    print(json.dumps(out, indent=2))
    if args.json:
        with open(args.json, "w") as fh:
            json.dump(out, fh, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
