#!/usr/bin/env python3
"""Same plan, two engines: join_order=written on the A binary (WA) and the B binary (WB) run the driver's spelling
and the swapped spelling (the order B's planner picks); BP is the B binary planned. Separates 'the planned order is
dearer than estimated' from 'the B code runs the same plan dearer'."""
import json, os, random, sys, time
sys.path.insert(0, "/home/cdkbs/bench-runs/cc-close/harness")
import cc_close_run as H
from bn_census import sched_ns

H.PORT.update({"WA": 25711, "WB": 25712, "BP": 25713, "PA": 25714, "PA2": 25715})
H.BIN.update({"WA": H.BIN["A"], "WB": H.BIN["B"], "BP": H.BIN["B"], "PA": H.BIN["A"], "PA2": H.BIN["A"]})
OUT = H.OUT


def swapped_c1(tag):
    return ("SELECT r.model_id, r.period_no, r.pnl_bp, r.equity_bp, r.trades, m.name, m.family "
            f"FROM models_{tag} AS m JOIN model_results_{tag} AS r ON r.model_id = m.id")


def swapped_c2(suffix, org):
    return (f"SELECT c.org_id, SUM(f.cbm) FROM cargos_{suffix} AS c "
            f"JOIN freights_{suffix} AS f ON f.cargo_id = c.id WHERE c.org_id = {org} GROUP BY c.org_id")


CELLS = {
    "c1-compare-all-r10k": (lambda r: H.c1_stmt("r10k"), lambda r: swapped_c1("r10k")),
    "c2-customer-statement-n1k": (lambda r: H.c2_stmt("n1k", r.randint(1, 50)), lambda r: swapped_c2("n1k", r.randint(1, 50))),
    "c2-customer-statement-ca10k": (lambda r: H.c2_stmt("ca", r.randint(1, 300)), lambda r: swapped_c2("ca", r.randint(1, 300))),
    "c2-customer-statement-df200k": (lambda r: H.c2_stmt("df", r.randint(1, 2000)), lambda r: swapped_c2("df", r.randint(1, 2000))),
}
import itertools
UNIQ = itertools.count(1)


def uniq_c3(tag, ns):
    # an always-true conjunct (ret_bp is within +-300) with a literal no execution repeats: every execution misses the memo
    def mk(r):
        k = next(UNIQ)
        return H.c3_stmt(tag, r.randint(0, ns - 1)) + f" AND t.ret_bp > -{1000 + k}"
    return mk


UCELLS = {f"c3-uniq-{t}": uniq_c3(t, ns) for t, ns in (("x200", 25), ("x1k", 125), ("x10k", 1250))}
UARMS = [("PA:uniq", "PA"), ("PA2:uniq", "PA2"), ("BP:uniq", "BP")]
ARMS = [("WA:swap", "WA", 1), ("WB:swap", "WB", 1), ("WA:drv", "WA", 0), ("WB:drv", "WB", 0), ("BP:drv", "BP", 0)]
ROUNDS = 3


def main():
    srvs = {"WA": H.Srv("WA", "written"), "WB": H.Srv("WB", "written"), "BP": H.Srv("BP", "planned"),
            "PA": H.Srv("PA", "planned"), "PA2": H.Srv("PA2", "planned")}
    ops_of = H.load_json("ops-gain.json", None)
    res = []
    for c, (drv, swp) in CELLS.items():          # equal warm-up everywhere, then >= 2 s for the steward
        for s in srvs.values():
            rng = random.Random("w-" + c)
            for _ in range(min(ops_of[c], 600)): s.q(drv(rng))
    for c, mk in UCELLS.items():
        for lab in ("PA", "PA2", "BP"):
            rng = random.Random("w-" + c)
            for _ in range(600): srvs[lab].q(mk(rng))
    time.sleep(3)
    for lab in ("BP",):
        for c, (drv, swp) in CELLS.items():
            H.status(f"extra analyze {c}")
    plans = {}
    for c, (drv, swp) in CELLS.items():
        q = drv(random.Random("plan")); qs = swp(random.Random("plan"))
        plans[c] = {"WA:swap": H.analyze(srvs["WA"], qs)[:5], "WB:swap": H.analyze(srvs["WB"], qs)[:5],
                    "BP:drv": H.analyze(srvs["BP"], q)[:5]}
    H.save_json("extra-plans.json", plans)
    for rnd in range(1, ROUNDS + 1):
        for ci, c in enumerate(CELLS):
            order = list(ARMS)
            k = (rnd + ci) % len(order)
            order = order[k:] + order[:k]
            if (rnd + ci) % 2: order = order[::-1]
            for name, lab, sw in order:
                H.status(f"extra round {rnd}/{ROUNDS} {c} {name}")
                make = CELLS[c][sw]
                r = H.run_block(srvs[lab], make, ops_of[c], f"x-{c}-{rnd}")
                r.update({"round": rnd, "cell": c, "arm": name, "host": H.host_state()})
                res.append(r)
                H.save_json("extra.json", res)
    memo_ops = H.load_json("ops-memo.json", None)
    ures = []
    for rnd in range(1, ROUNDS + 1):
        for ci, c in enumerate(UCELLS):
            order = list(UARMS)
            k = (rnd + ci) % len(order)
            order = order[k:] + order[:k]
            if (rnd + ci) % 2: order = order[::-1]
            for name, lab in order:
                H.status(f"extra-uniq round {rnd}/{ROUNDS} {c} {name}")
                r = H.run_block(srvs[lab], UCELLS[c], memo_ops["c3-same-" + c.split("-")[-1]], f"u-{c}-{rnd}")
                r.update({"round": rnd, "cell": c, "arm": name, "host": H.host_state()})
                ures.append(r)
                H.save_json("extra-uniq.json", ures)
    uplans = {}
    for c, mk in UCELLS.items():
        q = mk(random.Random("plan"))
        uplans[c] = {"PA": H.analyze(srvs["PA"], q)[:4], "BP": H.analyze(srvs["BP"], q)[:4]}
    H.save_json("extra-uniq-plans.json", uplans)
    H.save_json("extra-meta-BP.json", srvs["BP"].meta())
    for s in srvs.values(): s.stop()
    H.status("EXTRA DONE")


if __name__ == "__main__":
    main()
