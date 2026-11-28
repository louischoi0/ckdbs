#!/usr/bin/env python3
"""EM-S1's G1 measurement (`instructions/v3.2.0/workorder-em-front-end-reuse-without-quiet-wrong.md`
section 4): the spike's saving, interleaved A/B with an A2 floor.

Every arm given - A (the base), A2 (a byte copy of A, the floor) and the
spikes - runs every cell in a fresh server, the arms' order rotating per
round; `--rounds` must be a multiple of the arm count, so each arm takes
each position equally. A cell is `tools/em_s0_premise.py`'s: server CPU per statement over a
block, with the EM-S0 spike's parts where a build carries it. The cells are
the text and prepared paths of the point shapes and the join, the
query-layer `SELECT` (EM-R7), the qualified `ns1.t2` and `t2` under `USE`, the
qualified name again with a view in the instance, and the join under a
two-variant plan.

    em_s1_ab.py --arm A=BIN --arm A2=BIN --arm B=BIN --workdir D --out OUT.json
                [--rounds 8] [--k 2000] [--port 15885] [--server-cpu 2]
                [--client-cpu 5] [--heartbeat F]
"""
import argparse, hashlib, json, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dk_close import Server, pin, host_state  # noqa: E402
import em_s0_premise as s0  # noqa: E402

CELLS = [("base", "point", "text"), ("base", "point", "prepared"),
         ("base", "update", "text"), ("base", "update", "prepared"),
         ("base", "insert", "text"), ("base", "delete", "text"),
         ("base", "join3", "text"), ("base", "join3", "prepared"),
         ("base", "qlayer", "text"), ("base", "nsq", "text"), ("base", "nsu", "text"),
         ("view", "nsq", "text"), ("plan", "join3", "text"), ("plan", "join3", "prepared")]


def run_arm(binary, workdir, port, server_cpu, k, n, seed, beat):
    srv = Server(binary, workdir, "em-s1", port, 1, [server_cpu])
    pin(srv.proc.pid, server_cpu)
    pid = srv.proc.pid
    out = {}
    keys = s0.Keys(k * 2 + 2000)
    try:
        c = srv.conn()
        s0.setup(c, n, k * 2 + 2000, 10)
        for s, (text, holes, paths) in s0.SHAPES.items():
            if "prepared" in paths:
                c.prepare(s, text.format(*(["?"] * holes)))
        phase = None
        for ph, shape, path in CELLS:
            if ph != phase:
                if ph == "view":
                    c.execute("CREATE VIEW em_v AS SELECT id, v FROM t")
                if ph == "plan":
                    c.execute(s0.PLAN)
                phase = ph
            beat(f"{binary} {ph} {shape} {path}")
            s0.cell(c, pid, shape, path, 300, n, 99, keys)  # warm-up
            out[f"{ph}/{shape}/{path}"] = s0.cell(c, pid, shape, path, k, n, seed, keys)
        c.close()
    finally:
        srv.stop()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", action="append", required=True, help="NAME=BINARY")
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--rounds", type=int, default=8)
    ap.add_argument("--k", type=int, default=2000)
    ap.add_argument("--n", type=int, default=10000)
    ap.add_argument("--port", type=int, default=15885)
    ap.add_argument("--server-cpu", type=int, default=2)
    ap.add_argument("--client-cpu", type=int, default=5)
    ap.add_argument("--heartbeat")
    a = ap.parse_args()

    def beat(line):
        if a.heartbeat:
            with open(a.heartbeat, "w") as f:
                f.write(f"{time.time():.0f} {line}\n")

    os.sched_setaffinity(0, {a.client_cpu})
    arms = [tuple(x.split("=", 1)) for x in a.arm]
    if a.rounds % len(arms):
        sys.exit(f"--rounds {a.rounds} is not a multiple of {len(arms)} arms")
    meta = {"arms": {}, "rounds": a.rounds, "k": a.k, "n": a.n, "host_before": host_state(),
            "started": time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime())}
    for name, binary in arms:
        with open(binary, "rb") as f:
            meta["arms"][name] = {"bin": binary, "sha256": hashlib.sha256(f.read()).hexdigest()}
    rounds = {name: [] for name, _ in arms}
    hosts = []
    for r in range(a.rounds):
        order = arms[r % len(arms):] + arms[:r % len(arms)]
        for name, binary in order:
            hosts.append({"round": r, "arm": name, **host_state()})
            rounds[name].append(run_arm(binary, a.workdir, a.port, a.server_cpu, a.k, a.n,
                                        1000 + r, beat))
    meta["host_after"] = host_state()
    meta["finished"] = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime())
    summary = {}
    for key in rounds[arms[0][0]][0]:
        row = {}
        for name, _ in arms:
            cells = [x[key] for x in rounds[name]]
            row[name] = {"cpu_us": round(s0.med([c["cpu_us"] for c in cells]), 3),
                         "cpu_us_min": round(min(c["cpu_us"] for c in cells), 3),
                         "cpu_us_max": round(max(c["cpu_us"] for c in cells), 3)}
            for part in s0.PARTS:
                if part + "_us" in cells[0]:
                    row[name][part + "_us"] = round(s0.med([c[part + "_us"] for c in cells]), 3)
        summary[key] = row
    with open(a.out, "w") as f:
        json.dump({"meta": meta, "hosts": hosts, "summary": summary, "rounds": rounds}, f, indent=1)
    beat("done")
    for key, row in summary.items():
        print(key, {n: row[n]["cpu_us"] for n in row})


if __name__ == "__main__":
    main()
