#!/usr/bin/env python3
"""CX close regression bisect: N interleaved arms, one server per arm, the CX close
driver's point cells (bench/v3.4.12/archive/cx-close-v3.4.12-115-ga32528a4/cx_close.py)
with the arms a list.  --arm LABEL=BINARY[:conf-line;conf-line] repeated."""
import argparse, json, os, random, sys, time

sys.path.insert(0, "/home/louis/ckdbs/.claude/worktrees/cx-close-regression-bisect/tools")
sys.path.insert(0, "/home/louis/cxclose/run")
import cx_close as cx
from bb_overhead_benchmark import host_state


def status(path, line):
    with open(path + ".tmp", "w") as f: f.write(f"{time.strftime('%FT%TZ', time.gmtime())} {line}\n")
    os.replace(path + ".tmp", path)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--arm", action="append", required=True)
    p.add_argument("--wd", required=True); p.add_argument("--json", required=True); p.add_argument("--status", required=True)
    p.add_argument("--rounds", type=int, default=30); p.add_argument("--ops", type=int, default=20000)
    p.add_argument("--cells", default="pk-point-read,point-join-2rel,point-join-3rel")
    p.add_argument("--server-cpu", default="2"); p.add_argument("--cores", type=int, default=1)
    p.add_argument("--base-port", type=int, default=26701)
    a = p.parse_args()
    os.makedirs(a.wd, exist_ok=True)
    srvs = []
    for i, spec in enumerate(a.arm):
        label, rest = spec.split("=", 1)
        binary, _, extra = rest.partition(":")
        extra = "".join(l + "\n" for l in extra.split(";") if l)
        srvs.append(cx.Srv(label, binary, a.base_port + i, a.wd, a.server_cpu, a.cores, extra))
    R = cx.rows_for(1)
    for s in srvs:
        status(a.status, f"load {s.label}"); s.load(R)
    cells = a.cells.split(",")
    check = {}
    for c in cells:
        q = cx.CELLS[c][1](random.Random("plan"), None)
        sets = {}
        for s in srvs:
            _f, rows, _t, _n = s.conn.execute(q)
            sets[s.label] = sorted(tuple(r) for r in rows)
        check[c] = {"rows": len(sets[srvs[0].label]),
                    "multiset_equal_all": all(v == sets[srvs[0].label] for v in sets.values())}
    json.dump(check, open(a.json + ".check", "w"), indent=1)
    out = []; t0 = time.time(); n = len(srvs)
    for rnd in range(1, a.rounds + 1):
        for c in cells:
            order = srvs[(rnd - 1) % n:] + srvs[:(rnd - 1) % n]
            if rnd % 2 == 0: order = order[::-1]
            for s in order:
                status(a.status, f"round {rnd}/{a.rounds} {c} {s.label} elapsed {time.time() - t0:.0f}s")
                r = cx.run_cell(s, c, a.ops, 1, rnd); r["host"] = host_state()
                out.append(r)
        json.dump(out, open(a.json + ".tmp", "w")); os.replace(a.json + ".tmp", a.json)
    for s in srvs: s.stop()
    status(a.status, "DONE")


if __name__ == "__main__":
    main()
