#!/usr/bin/env python3
"""DQ-S5's diagnosis, part 1: is M1's join residue in the compile?

`bench/v3.5.0/results-dq-close-v3.5.0-101-g7e31d272b.md` measured the
all-served Cabin join (c) and the tuple-key join (d) +0.2 to +0.5 us dearer
on B. DQ's compile runs instructions on both shapes: the walk arm asks
`HasUnindexedEqualityFilter` a second time inside `BuildAnnotationOf`, and a
correlated Cabin step computes `BuildKeyOf` for its fallback. No compiled
chain is cached (CX-S6), so every statement pays its compile.

This splits the statement's server cost from `sys.statement_stats`, which
the server keeps per shape: `compile_us` (lex, parse, fingerprint and
compile, summed over calls) and `cpu_us` (over `timed_calls`). One server
per binary, up at once, `cores = 1`, `relaxed`, `join_order = written`, the
relations of `dq_close.py` (its `prepare`), the Cabin converged first. Per
round, every arm runs the same `--k` statements of each shape, rotated; the
view is read after the last round. A third arm (`A2`, A's binary again) is
the floor.

    dq_compile_probe.py --binA A --binB B --workdir D --out OUT.json
                        [--rounds 8] [--k 2000] [--heartbeat F]
"""
import argparse, json, os, random, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dq_close  # noqa: E402
from dq_s0_premise import quiet_gate  # noqa: E402
from dp_close import rotated  # noqa: E402

SHAPES = {"b_build": dq_close.JOIN_B, "c_cabin": dq_close.JOIN_C,
          "d_tuplekey": dq_close.JOIN_D}


def shape_stats(c):
    """Per shape, (calls, compile_us, timed_calls, cpu_us) from the view."""
    fields, rows, _, _ = c.execute(
        "SELECT relations, calls, compile_us, timed_calls, cpu_us FROM sys.statement_stats")
    out = {}
    for r in rows:
        rels = (r[0] or b"").decode()
        vals = [int.from_bytes(x, "little", signed=True) if x else 0 for x in r[1:]]
        for name, rel in (("b_build", "innb"), ("c_cabin", "innc"), ("d_tuplekey", "innd")):
            if rel in rels:
                prev = out.get(name, [0, 0, 0, 0])
                out[name] = [p + v for p, v in zip(prev, vals)]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binA", required=True)
    ap.add_argument("--binB", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--rounds", type=int, default=8)
    ap.add_argument("--k", type=int, default=2000)
    ap.add_argument("--port", type=int, default=15861)
    ap.add_argument("--cpus", default="1,2,3")
    ap.add_argument("--heartbeat", default=None)
    a = ap.parse_args()
    os.sched_setaffinity(0, {0})

    def beat(line):
        stamp = time.strftime("%H:%M:%S", time.gmtime())
        print(f"{stamp} {line}", flush=True)
        if a.heartbeat:
            with open(a.heartbeat, "a") as f:
                f.write(f"{stamp} {line}\n")

    cpus = [int(x) for x in a.cpus.split(",")]
    arms = {"A": a.binA, "A2": a.binA, "B": a.binB}
    servers, conns = {}, {}
    for i, (arm, binary) in enumerate(arms.items()):
        servers[arm] = dq_close.Server(binary, a.workdir, f"probe-{arm}", a.port + i, cpus[i])
        conns[arm] = servers[arm].conn()
        dq_close.prepare(conns[arm])
        dq_close.converge(conns[arm])
    held = 0.0
    try:
        for rnd in range(a.rounds):
            held += quiet_gate(beat)
            for arm in rotated(list(arms), rnd):
                for shape, sql in SHAPES.items():
                    for _ in range(a.k):
                        conns[arm].execute(sql)
            beat(f"round {rnd + 1} of {a.rounds}")
        stats = {arm: shape_stats(conns[arm]) for arm in arms}
    finally:
        for s in servers.values():
            s.stop()
    out = {"args": vars(a), "quiet_hold_s": held, "stats": stats, "per_call": {}}
    for arm, by in stats.items():
        for shape, (calls, comp, timed, cpu) in by.items():
            out["per_call"].setdefault(shape, {})[arm] = {
                "calls": calls, "compile_us": round(comp / max(calls, 1), 3),
                "cpu_us": round(cpu / max(timed, 1), 3)}
    with open(a.out, "w") as f:
        json.dump(out, f, indent=1)
    for shape, by in out["per_call"].items():
        beat(f"{shape}: " + "  ".join(f"{arm} compile={v['compile_us']}us cpu={v['cpu_us']}us"
                                       for arm, v in by.items()))
    beat("done")


if __name__ == "__main__":
    main()
