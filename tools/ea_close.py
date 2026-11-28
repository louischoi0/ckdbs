#!/usr/bin/env python3
"""EA's close measurement (`instructions/v3.2.0/workorder-ea-prepared-statement-reuse.md`
section 5, EA-S4): what EA costs a statement that is not prepared (EA-R4) and
what it saves one that is.

Three servers up at once, `cores = 1`, `relaxed` durability, each on its own
data file under `--workdir` and its own CPU, the server-to-CPU mapping rotated
every round: A (the commit EA opened from), A2 (a second copy of the same
binary, the in-run floor) and B (EA's close). Per round, per shape - a point
`SELECT`, a point `UPDATE`, a three-relation join by key - and per path - `text`
(the unnamed statement with its literal) and `prepared` (a named statement's
`?` bound with `int64` values) - every arm runs one block of `--k` statements,
the arm order rotated. The primary number is server CPU per statement
(`/proc/<pid>/task/*/schedstat` over the block, every server thread), the
median over rounds; client wall latency per statement is kept for the
percentile tables.

    ea_close.py --bin-a A --bin-b B --workdir D --out OUT.json [--rounds 24]
                [--k 2000] [--n 10000] [--port 15891] [--cpus 2,3,4]
                [--client-cpu 6] [--heartbeat F]

Both binaries must be copies (bench/README.md rule 3); the driver hashes them.
"""
import argparse, hashlib, json, os, random, shutil, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
from dk_close import Server, cpu_ns, pin, lat_summary, host_state  # noqa: E402
from ea_s0_premise import SHAPES, args_for, setup, med  # noqa: E402


def block(c, pid, shape, path, k, n, seed):
    text, _ = SHAPES[shape]
    rng = random.Random(seed)
    work = [args_for(shape, rng, n, i) for i in range(k)]
    lats = []
    t0 = cpu_ns(pid)
    if path == "text":
        for a in work:
            q = text.format(*a)
            t = time.perf_counter()
            c.execute(q)
            lats.append(time.perf_counter() - t)
    else:
        for a in work:
            params = [kwp.Connection.int64(x) for x in a]
            t = time.perf_counter()
            c.execute_prepared(shape, params)
            lats.append(time.perf_counter() - t)
    return {"cpu_us": (cpu_ns(pid) - t0) / 1000.0 / k, "lat": lat_summary(lats),
            "host": host_state()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin-a", required=True)
    ap.add_argument("--bin-b", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--rounds", type=int, default=24)
    ap.add_argument("--k", type=int, default=2000)
    ap.add_argument("--n", type=int, default=10000)
    ap.add_argument("--port", type=int, default=15891)
    ap.add_argument("--cpus", default="2,3,4")
    ap.add_argument("--client-cpu", type=int, default=6)
    ap.add_argument("--heartbeat")
    a = ap.parse_args()

    def beat(line):
        if a.heartbeat:
            with open(a.heartbeat, "w") as f:
                f.write(f"{time.time():.0f} {line}\n")

    os.sched_setaffinity(0, {a.client_cpu})
    cpus = [int(x) for x in a.cpus.split(",")]
    arms = {"A": a.bin_a, "A2": a.bin_a, "B": a.bin_b}
    meta = {"rounds": a.rounds, "k": a.k, "n": a.n, "cpus": cpus, "client_cpu": a.client_cpu,
            "host_before": host_state(),
            "started": time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime())}
    for name, path in (("A", a.bin_a), ("B", a.bin_b)):
        with open(path, "rb") as f:
            meta["sha256_" + name] = hashlib.sha256(f.read()).hexdigest()
    beat("starting")
    servers, conns = {}, {}
    try:
        for i, (name, binary) in enumerate(arms.items()):
            servers[name] = Server(binary, a.workdir, "ea-" + name, a.port + i, 1, [cpus[i]])
            pin(servers[name].proc.pid, cpus[i])
            conns[name] = servers[name].conn()
            setup(conns[name], a.n)
            for s in SHAPES:
                conns[name].prepare(s, SHAPES[s][0].format(*(["?"] * SHAPES[s][1])))
            for s in SHAPES:
                for path in ("text", "prepared"):
                    block(conns[name], servers[name].proc.pid, s, path, 500, a.n, 7)
        names = list(arms)
        results = {s: {p: {n: [] for n in names} for p in ("text", "prepared")} for s in SHAPES}
        for r in range(a.rounds):
            for i, name in enumerate(names):  # rotate the server-to-CPU mapping
                pin(servers[name].proc.pid, cpus[(i + r) % len(cpus)])
            order = names[r % 3:] + names[:r % 3]
            for s in SHAPES:
                for path in (("text", "prepared") if r % 2 == 0 else ("prepared", "text")):
                    for name in order:
                        beat(f"round {r} {s} {path} {name}")
                        cell = block(conns[name], servers[name].proc.pid, s, path, a.k, a.n,
                                     1000 * r + 17)
                        cell["cpu"] = cpus[(names.index(name) + r) % len(cpus)]
                        results[s][path][name].append(cell)
        for c in conns.values():
            c.close()
    finally:
        for srv in servers.values():
            srv.stop()
    meta["host_after"] = host_state()
    meta["finished"] = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime())
    summary = {}
    for s in SHAPES:
        summary[s] = {}
        for path in ("text", "prepared"):
            row = {}
            for name in arms:
                cells = results[s][path][name]
                row[name] = {"cpu_us": round(med([c["cpu_us"] for c in cells]), 3),
                             "cpu_min": round(min(c["cpu_us"] for c in cells), 3),
                             "cpu_max": round(max(c["cpu_us"] for c in cells), 3),
                             **{q: round(med([c["lat"][q] for c in cells]), 1)
                                for q in ("p0", "p25", "p50", "p95", "p99")}}
            # Paired per round: the same round's blocks share a host state.
            row["B_minus_A_us"] = round(med([b["cpu_us"] - x["cpu_us"] for b, x in
                                             zip(results[s][path]["B"],
                                                 results[s][path]["A"])]), 3)
            row["A2_minus_A_us"] = round(med([b["cpu_us"] - x["cpu_us"] for b, x in
                                              zip(results[s][path]["A2"],
                                                  results[s][path]["A"])]), 3)
            summary[s][path] = row
    with open(a.out, "w") as f:
        json.dump({"meta": meta, "summary": summary, "rounds": results}, f, indent=1)
    beat("done")
    print(json.dumps(summary, indent=1))


if __name__ == "__main__":
    main()
