#!/usr/bin/env python3
"""SS-B - the statement-shipping measurement, cell by cell.

Sweeps `bench/single_relation_probe.py` over the SS-B cells of
`instructions/v2.2.0/measurement-after-s5.md`, five reps each, and reports a
median with its spread. Every A/B cell runs its two arms in a **fixed order**
- the local (`--seat owner`) arm first, the shipped (`--seat foreign`) arm
second - so the harness's ordering bias falls in one direction and can be
divided out. The `null*` cells are that divisor: both arms identical, so the
ratio they return is the bias and nothing else.

Cells:

  null1   `cores = 1` against `cores = 1`, the order's null cell verbatim.
  null4   `cores = 4`, both arms seated on the owner. The literal shape of
          every B1/B2 ratio with the one thing under test held fixed.
  b1      one relation per writer core, one session each: shipped against
          seated. Memo claim 2's cell.
  b2      S = 2, 4, 8, 14 sessions on **one** owner's relation, shipped
          against seated. Memo claims 1 and 3.
  b3      1, 2 and 4 relations on the **same** owner at fixed S. Runs only
          if b2 departs from the 590 x S law (the order's condition).
  b4      K = 1, 4, 16 sessions on one arrival core: the parked-waiter
          population, priced off that core's polls/polled_us.
  b6      one arrival core, one session, >= 20,000 shipped statements: the
          abandoned transaction's lease-block cost.

Usage:
    bench/run_ssb.py --server ~/ssb/bin/kds_server --workdir ~/ssb/run \
        --archive bench/v2.2.0/archive/ssb-v2.2.0-11-g982e133 --cells null1,b1,b2
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from run_benchv2 import median  # noqa: E402

PROBE = os.path.join(HERE, "single_relation_probe.py")

# Processes that make a cell fiction if they run beside it. This box is
# shared between worktrees: another agent's `cmake --build` or `kds_tests`
# has already cost this suite one whole sweep (a cell measured beside one
# ran at half rate with nothing in the driver's output to show for it), so
# the gate is a process check and not only a load average.
COMPETITORS = ("cc1plus", "ld", "kds_tests", "cc1", "as")


def competitors_running():
    """Names of the competing processes alive right now, empty if none."""
    out = []
    for name in COMPETITORS:
        if subprocess.run(["pgrep", "-x", name], capture_output=True).returncode == 0:
            out.append(name)
    return out


def gate(limit, timeout_s=900.0):
    """Blocks until no competitor runs and the 1-minute load is under
    `limit`. Returns (loadavg, waited_s) - both recorded per arm, because a
    cell that started at the timeout rather than at the condition must be
    readable as such afterwards."""
    start = time.time()
    while time.time() - start < timeout_s:
        if not competitors_running() and os.getloadavg()[0] <= limit:
            break
        time.sleep(5)
    return round(os.getloadavg()[0], 2), round(time.time() - start, 1)


def arm(cores, sessions, seat, relations=1, rows=1000, same_owner=False,
        arrival_core=None, trace=False, single=False, durability="group"):
    a = ["--arm", "single" if single else "multi", "--cores", str(cores),
         "--sessions", str(sessions), "--relations", str(relations),
         "--rows", str(rows), "--seat", seat, "--durability", durability]
    if same_owner:
        a.append("--same-owner")
    if arrival_core is not None:
        a += ["--arrival-core", str(arrival_core)]
    if trace:
        a.append("--trace-latencies")
    return a


def cells(rows):
    """(name, arm_a, arm_b) - arm_b None for a single-arm cell."""
    out = []
    # The null cells. Both arms identical; the ratio is the harness.
    out.append(("null1", arm(1, 4, "owner", rows=rows, single=True),
                arm(1, 4, "owner", rows=rows, single=True)))
    out.append(("null4", arm(4, 4, "owner", rows=rows),
                arm(4, 4, "owner", rows=rows)))
    # B1: one relation per writer core, one session each.
    out.append(("b1", arm(4, 3, "owner", relations=3, rows=rows),
                arm(4, 3, "foreign", relations=3, rows=rows)))
    # B2: the R2 curve.
    for s in (2, 4, 8, 14):
        out.append((f"b2-s{s}", arm(4, s, "owner", rows=rows),
                    arm(4, s, "foreign", rows=rows)))
    # The row-set sweep every cell in this suite owes (`ck-tester` rule 9):
    # S = 4 against a relation of ~200, ~1,000 and ~10,000 rows, so a fixed
    # cost of shipping can be told from one that grows with the tree. The
    # relation takes `sessions x rows + 1` rows, which is where the three
    # per-session counts come from.
    for rows_each, total in ((50, 200), (250, 1000), (2500, 10000)):
        out.append((f"sz-{total}", arm(4, 4, "owner", rows=rows_each),
                    arm(4, 4, "foreign", rows=rows_each)))
    # B3: the tail page. Same S, 1/2/4 relations on one owner.
    for r in (1, 2, 4):
        out.append((f"b3-r{r}", arm(4, 8, "owner", relations=r, rows=rows,
                                    same_owner=True),
                    arm(4, 8, "foreign", relations=r, rows=rows,
                        same_owner=True)))
    # B4: K parked waiters on **one** arrival core. The instrument is that
    # core's poll block; the seated arm rides along so the p50 has something
    # to be read against, and the pair keeps the a/b sense of every other
    # cell (a = seated, b = shipped).
    for k in (1, 4, 16):
        out.append((f"b4-k{k}", arm(4, k, "owner", rows=rows),
                    arm(4, k, "foreign", rows=rows, arrival_core=-1)))
    # The sync control: the S = 1 cell again with the device sync taken out
    # of **both** arms. If shipping's S = 1 loss is a second commit cycle it
    # collapses here; if it is the wire and the waiter it does not. Nothing
    # measured under `relaxed` is a durability claim - this cell exists only
    # to attribute the S = 1 gap.
    for d in ("group", "relaxed"):
        out.append((f"sync-{d}", arm(4, 1, "owner", rows=rows, durability=d),
                    arm(4, 1, "foreign", rows=rows, arrival_core=-1, durability=d)))
    # B6: one session, one arrival core, past six trx-id lease blocks
    # (`kTrxIdBlockSize` = 4096, txn/trx_id.hpp). The row count is fixed
    # rather than taking `--rows`: the cell's whole point is crossing the
    # blocks, and a smaller sweep-wide `--rows` would quietly stop short of
    # them. One session, not K, so a step at a boundary is not smeared
    # across sessions that did not cross it.
    out.append(("b6", arm(4, 1, "owner", rows=25000, trace=True),
                arm(4, 1, "foreign", rows=25000, arrival_core=-1, trace=True)))
    return dict((n, (a, b)) for n, a, b in out)


def run_arm(args, name, side, argv, rep, port):
    workdir = os.path.join(args.workdir, f"{name}-{side}-r{rep}")
    os.makedirs(workdir, exist_ok=True)
    cmd = [sys.executable, PROBE, "--server", args.server, "--workdir", workdir,
           "--port", str(port)] + argv
    # Up to `--contention-retries + 1` goes: a competitor that appears
    # *during* the arm is not visible to the gate before it, and the honest
    # answer to a contended cell is to run it again rather than to average
    # it in. A cell that never gets a clean go is reported contended, never
    # silently kept.
    for attempt in range(args.contention_retries + 1):
        load, waited = gate(args.quiet_load)
        t0 = time.time()
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout)
            stdout, stderr, rc, timed_out = proc.stdout, proc.stderr, proc.returncode, False
        except subprocess.TimeoutExpired as e:
            stdout = (e.stdout.decode() if isinstance(e.stdout, bytes) else e.stdout) or ""
            stderr = (e.stderr.decode() if isinstance(e.stderr, bytes) else e.stderr) or ""
            rc, timed_out = None, True
        elapsed = time.time() - t0
        contended = competitors_running()
        if not contended:
            break
        print(f"    {name}-{side}-r{rep}: contended by {contended}, "
              f"attempt {attempt + 1}", flush=True)
        shutil.rmtree(workdir, ignore_errors=True)
        os.makedirs(workdir, exist_ok=True)
        time.sleep(args.settle)
    parsed = None
    if stdout.strip():
        try:
            parsed = json.loads(stdout)
        except json.JSONDecodeError:
            parsed = None
    rec = dict(cell=name, side=side, rep=rep, returncode=rc, timed_out=timed_out,
               elapsed_s=round(elapsed, 2), invocation=" ".join(cmd), result=parsed,
               loadavg_at_start=load, gate_wait_s=waited, contended=contended,
               attempts=attempt + 1,
               stderr=stderr[-4000:] if stderr.strip() else "")
    if args.archive:
        base = os.path.join(args.archive, f"{name}-{side}-r{rep}")
        os.makedirs(args.archive, exist_ok=True)
        with open(base + ".json", "w") as fh:
            json.dump(rec, fh, indent=2)
    shutil.rmtree(workdir, ignore_errors=True)
    time.sleep(args.settle)
    return rec


def ips(rec):
    r = rec.get("result") or {}
    return r.get("inserts_per_second")


def summarize(name, recs_a, recs_b):
    def col(recs, key):
        return [(r.get("result") or {}).get(key) for r in recs]

    row = dict(cell=name, reps=len(recs_a),
               contended=[f"{r['side']}-r{r['rep']}:{r['contended']}"
                          for r in list(recs_a) + list(recs_b) if r.get("contended")],
               gate_wait_max_s=max([r.get("gate_wait_s") or 0
                                    for r in list(recs_a) + list(recs_b)], default=0))
    for side, recs in (("a", recs_a), ("b", recs_b)):
        if not recs:
            continue
        row[f"{side}_ips_median"] = median(col(recs, "inserts_per_second"))
        row[f"{side}_ips_min"] = min([v for v in col(recs, "inserts_per_second")
                                      if v is not None], default=None)
        row[f"{side}_ips_max"] = max([v for v in col(recs, "inserts_per_second")
                                      if v is not None], default=None)
        for p in ("p0", "p25", "p50", "p95", "p99"):
            row[f"{side}_{p}_us"] = median(col(recs, f"insert_{p}_us"))
        row[f"{side}_attempted"] = sum(v or 0 for v in col(recs, "attempted"))
        row[f"{side}_executed"] = sum(v or 0 for v in col(recs, "executed"))
        row[f"{side}_refused"] = sum(v or 0 for v in col(recs, "refused"))
        row[f"{side}_retries"] = sum(v or 0 for v in col(recs, "retries"))
        row[f"{side}_verify_bad"] = [v for v in col(recs, "verify")
                                     if v and "as expected" not in v]
        row[f"{side}_classes"] = {}
        for c in col(recs, "refusal_classes"):
            for k, v in (c or {}).items():
                row[f"{side}_classes"][k] = row[f"{side}_classes"].get(k, 0) + v
    if recs_b:
        ratios = [b / a for a, b in
                  ((ips(x), ips(y)) for x, y in zip(recs_a, recs_b))
                  if a and b]
        row["ratio_per_rep"] = [round(x, 4) for x in ratios]
        row["ratio_median"] = round(median(ratios), 4) if ratios else None
        row["ratio_min"] = round(min(ratios), 4) if ratios else None
        row["ratio_max"] = round(max(ratios), 4) if ratios else None
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="build-release/kds_server")
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--archive", default="")
    ap.add_argument("--cells", default="null1,null4,b1,b2,b4")
    ap.add_argument("--rows", type=int, default=1000)
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--port", type=int, default=16400)
    ap.add_argument("--timeout", type=float, default=1200.0)
    ap.add_argument("--quiet-load", type=float, default=2.5,
                    help="1-minute loadavg the gate waits for; the harness's own "
                         "decay sits near 2 between arms, so a lower bar only waits "
                         "out the previous cell")
    ap.add_argument("--contention-retries", type=int, default=2)
    ap.add_argument("--settle", type=float, default=2.0)
    args = ap.parse_args()
    args.server = os.path.abspath(args.server)
    os.makedirs(args.workdir, exist_ok=True)
    if args.archive:
        args.archive = os.path.abspath(args.archive)
        os.makedirs(args.archive, exist_ok=True)

    table = cells(args.rows)
    wanted = []
    for tok in args.cells.split(","):
        tok = tok.strip()
        if not tok:
            continue
        if tok in table:
            wanted.append(tok)
        else:
            hits = [n for n in table if n.startswith(tok + "-")]
            if not hits:
                sys.exit(f"no such cell: {tok} (have {', '.join(table)})")
            wanted += hits

    port = args.port
    summary, runs = {}, []
    for name in wanted:
        arm_a, arm_b = table[name]
        recs_a, recs_b = [], []
        for rep in range(1, args.reps + 1):
            ra = run_arm(args, name, "a", arm_a, rep, port)
            port += 4
            recs_a.append(ra)
            runs.append(ra)
            if arm_b is not None:
                rb = run_arm(args, name, "b", arm_b, rep, port)
                port += 4
                recs_b.append(rb)
                runs.append(rb)
            print(f"  {name} r{rep}: a={ips(ra)} "
                  f"b={ips(recs_b[-1]) if recs_b else '-'}", flush=True)
        row = summarize(name, recs_a, recs_b)
        summary[name] = row
        print(f"== {name}: a={row.get('a_ips_median')} b={row.get('b_ips_median')} "
              f"ratio={row.get('ratio_median')} "
              f"({row.get('ratio_min')}..{row.get('ratio_max')}) "
              f"refused a={row.get('a_refused')} b={row.get('b_refused')}", flush=True)

    if args.archive:
        with open(os.path.join(args.archive, "summary.json"), "w") as fh:
            json.dump(dict(summary=summary, runs=runs), fh, indent=2)
        print(f"\narchived to {args.archive}")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
