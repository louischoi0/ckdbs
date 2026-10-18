#!/usr/bin/env python3
"""Does the c1 warm-up before the ca10k cell decide the 29 % vs 45 % state? Servers: A (reference), B2 (1a9168d3 warmed on the c2 cells only),
B1 (1a9168d3 warmed c1 r2872, r10k first, then the c2 cells). Output gain5.json."""
import json, os, random, sys, time
sys.path.insert(0, "/home/cdkbs/bench-runs/cc-prior/harness")
import prior_run as P
H = P.H
H.PORT.update({"B1": 25805, "B2": 25806})
H.BIN.update({"B1": H.BIN["B"], "B2": H.BIN["B"]})
H.ROUNDS = 5
OUT = P.OUT


def main():
    G = H.GAIN
    cells = {c: G[c] for c in ("c2-customer-statement-ca10k", "c2-customer-statement-n1k")}
    ops = json.load(open(OUT + "/ops-gain.json"))
    srvs = {k: H.Srv(k) for k in ("A", "B1", "B2")}
    warm = {"A": ["c2-customer-statement-ca10k", "c2-customer-statement-n1k"],
            "B2": ["c2-customer-statement-ca10k", "c2-customer-statement-n1k"],
            "B1": ["c1-compare-all-r2872", "c1-compare-all-r10k", "c2-customer-statement-ca10k", "c2-customer-statement-n1k"]}
    for k, lst in warm.items():
        for c in lst:
            H.status(f"gain5 warm {c} {k}")
            rng = random.Random("w-" + c)
            for _ in range(min(ops[c], 1500)): srvs[k].q(G[c](rng))
    time.sleep(8)
    H.rounds_phase("gain5", srvs, cells, [(k, k) for k in srvs], ops)
    for s in srvs.values(): s.stop()
    H.status("gain5 done")
    open(OUT + "/run.done", "w").write("done\n")


if __name__ == "__main__":
    main()
