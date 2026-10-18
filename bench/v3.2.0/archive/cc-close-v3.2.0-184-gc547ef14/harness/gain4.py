#!/usr/bin/env python3
"""Is the slow state B's code or the engine's? The swapped-spelling (FilterScan cargos, then Scan freights + build) plan under
join_order=written on the A binary and on the B binary, each on a server with and without the overhead tables created and loaded."""
import os, random, sys, time
sys.path.insert(0, "/home/cdkbs/bench-runs/cc-close/harness")
import cc_close_run as H

H.PORT.update({"WA0": 25731, "WA1": 25732, "WB0": 25733, "WB1": 25734})
H.BIN.update({"WA0": H.BIN["A"], "WA1": H.BIN["A"], "WB0": H.BIN["B"], "WB1": H.BIN["B"]})
H.ROUNDS = 5


def swapped(suffix, norg):
    return lambda r: (f"SELECT c.org_id, SUM(f.cbm) FROM cargos_{suffix} AS c "
                      f"JOIN freights_{suffix} AS f ON f.cargo_id = c.id WHERE c.org_id = {r.randint(1, norg)} GROUP BY c.org_id")


CELLS = {"c2-swapped-ca10k": swapped("ca", 300), "c2-swapped-df200k": swapped("df", 2000)}


def main():
    srvs = {k: H.Srv(k, "written") for k in ("WA0", "WA1", "WB0", "WB1")}
    for k in ("WA1", "WB1"): H.setup_ov(srvs[k])
    ops_gain = H.load_json("ops-gain.json", None)
    ops = {"c2-swapped-ca10k": ops_gain["c2-customer-statement-ca10k"], "c2-swapped-df200k": ops_gain["c2-customer-statement-df200k"]}
    for c, make in CELLS.items():
        for k, s in srvs.items():
            H.status(f"gain4 warm {c} {k}")
            rng = random.Random("w-" + c)
            for _ in range(min(ops[c], 1500)): s.q(make(rng))
    H.rounds_phase("gain4", srvs, CELLS, [(k, k) for k in srvs], ops)
    for s in srvs.values(): s.stop()
    H.status("GAIN4 DONE")


if __name__ == "__main__":
    main()
