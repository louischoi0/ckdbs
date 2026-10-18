#!/usr/bin/env python3
"""Bisect of the full-sequence slow state of B on ca10k / df200k: B0 = gain warm-up only; B1 = ov tables created and
loaded first; B2 = B1 plus the overhead phase's INSERT and UPDATE statements; B3 = B2 plus the cold phase. A for reference."""
import os, random, sys, time
sys.path.insert(0, "/home/cdkbs/bench-runs/cc-close/harness")
import cc_close_run as H

H.PORT.update({"B0": 25721, "B1": 25722, "B2": 25723, "B3": 25724})
for k in ("B0", "B1", "B2", "B3"): H.BIN[k] = H.BIN["B"]
CELLS = ["c2-customer-statement-ca10k", "c2-customer-statement-df200k"]
H.ROUNDS = 5


def main():
    srvs = {k: H.Srv(k) for k in ("A", "B0", "B1", "B2", "B3")}
    ops_gain = H.load_json("ops-gain.json", None)
    ops_memo = H.load_json("ops-memo.json", None)
    ops_ov = H.load_json("ops-overhead.json", None)
    for k in ("A", "B1", "B2", "B3"): H.setup_ov(srvs[k])
    for k in ("A", "B2", "B3"):
        for c in ("ov-insert", "ov-update", "ov-point-read", "ov-join3-tuplekey"):
            H.status(f"gain3 overhead-history {k} {c}")
            rng = random.Random(f"w-{c}")
            for _ in range(min(ops_ov[c], 3000)): srvs[k].q(H.OVERHEAD[c](rng))
            for r in range(5):
                rng = random.Random(f"x-{c}-{r}")
                for _ in range(ops_ov[c]): srvs[k].q(H.OVERHEAD[c](rng))
    for c, make in H.GAIN.items():                      # the cold phase: 20 executions of each gain cell
        rng = random.Random(f"cold-{c}")
        for lab in ("A", "B3"):
            for _ in range(20): srvs[lab].q(make(rng))
    for c, make in list(H.GAIN.items()) + list(H.MEMO.items()):
        n = ops_gain.get(c) or ops_memo[c]
        for lab in srvs:
            H.status(f"gain3 warm {c} {lab}")
            rng = random.Random(f"w-{c}")
            for _ in range(min(n, 1500)): srvs[lab].q(make(rng))
    time.sleep(8)
    cells = {c: H.GAIN[c] for c in CELLS}
    H.rounds_phase("gain3", srvs, cells, [(k, k) for k in srvs], ops_gain)
    H.save_json("gain3-meta.json", {k: srvs[k].meta() for k in srvs})
    for s in srvs.values(): s.stop()
    H.status("GAIN3 DONE")


if __name__ == "__main__":
    main()
