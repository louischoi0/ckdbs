#!/usr/bin/env python3
"""Reproduction of the main run's ca10k / df200k / n1k B figures with fresh servers: B warmed with the main run's whole
warm-up history (all gain and memo cells) against BL warmed on the three cells only."""
import os, random, sys, time
sys.path.insert(0, "/home/cdkbs/bench-runs/cc-close/harness")
import cc_close_run as H

H.PORT.update({"BL": 25716})
H.BIN.update({"BL": H.BIN["B"]})
CELLS = ["c2-customer-statement-ca10k", "c2-customer-statement-df200k", "c2-customer-statement-n1k"]
H.ROUNDS = 5


def main():
    srvs = {k: H.Srv(k) for k in ("A", "A2", "B", "BL")}
    ops_gain = H.load_json("ops-gain.json", None)
    ops_memo = H.load_json("ops-memo.json", None)
    # full history on A, A2, B exactly as the main run did it; light history on BL
    for c, make in list(H.GAIN.items()) + list(H.MEMO.items()):
        n = ops_gain.get(c) or ops_memo[c]
        for lab in ("A", "A2", "B"):
            H.status(f"gain2 warm-full {c} {lab}")
            rng = random.Random(f"w-{c}")
            for _ in range(min(n, 1500)): srvs[lab].q(make(rng))
    for c in CELLS:
        rng = random.Random(f"w-{c}")
        for _ in range(min(ops_gain[c], 1500)): srvs["BL"].q(H.GAIN[c](rng))
    time.sleep(8)
    cells = {c: H.GAIN[c] for c in CELLS}
    H.rounds_phase("gain2", srvs, cells, [("A", "A"), ("A2", "A2"), ("B", "B"), ("BL", "BL")], ops_gain)
    H.save_json("gain2-meta.json", {k: srvs[k].meta() for k in ("B", "BL")})
    for s in srvs.values(): s.stop()
    H.status("GAIN2 DONE")


if __name__ == "__main__":
    main()
