#!/usr/bin/env python3
"""Same clean-server gain cells as prior_run.phase_gain, with a fourth arm BO: the CC-close binary (c547ef14, old prior) started
the same way, so the session state is common to both priors. Outputs gain4.json / plans4.json / gain4-meta.json."""
import json, os, random, sys, time
sys.path.insert(0, "/home/cdkbs/bench-runs/cc-prior/harness")
import prior_run as P
H = P.H

H.PORT.update({"BO": 25804})
H.BIN.update({"BO": "/home/cdkbs/bench-runs/cc-close/kds_server_B"})
H.ROUNDS = 5
OUT = P.OUT


def main():
    CELLS = {c: H.GAIN[c] for c in ("c1-compare-all-r2872", "c1-compare-all-r10k", "c2-customer-statement-ca10k",
                                    "c2-customer-statement-n1k")}
    ops = json.load(open(OUT + "/ops-gain.json"))
    srvs = {k: H.Srv(k) for k in ("A", "A2", "B", "BO")}
    for c, make in CELLS.items():
        for k, s in srvs.items():
            H.status(f"gain4 warm {c} {k}")
            rng = random.Random("w-" + c)
            for _ in range(min(ops[c], 1500)): s.q(make(rng))
    time.sleep(8)
    plans = {}
    for c, make in CELLS.items():
        q = make(random.Random("plan"))
        plans[c] = {"stmt": q, **{k: H.analyze(s, q) for k, s in srvs.items()}}
    json.dump(plans, open(OUT + "/plans4.json", "w"))
    H.rounds_phase("gain4", srvs, CELLS, [(k, k) for k in srvs], ops)
    json.dump({k: P.meta(s) for k, s in srvs.items()}, open(OUT + "/gain4-meta.json", "w"))
    for s in srvs.values(): s.stop()
    H.status("gain4 done")
    open(OUT + "/run.done", "w").write("done\n")


if __name__ == "__main__":
    main()
